// ---------------------------------------------------------------------------
// reuse_temporal.wgsl — temporal reservoir reuse (GRIS merge with the
// reprojected previous-frame reservoir).
//
// The previous reservoir's confidence is capped adaptively using the sample
// duplication map (Lin 2026 §5): heavily duplicated regions get their history
// weight cut toward cCapMin, trading a little bias for much less correlation.
// Disocclusions optionally rescue history from a 3x3 neighborhood around the
// reprojected pixel (a cheap stand-in for dual motion vectors, §6.4 — the
// scene is static, so any validating neighbor history is genuinely the same
// surface).
//
// The merge itself needs no Jacobian: point samples live in area measure and
// direction samples shift by identity.
//
// RF_MUTATE (ours): instead of (or in addition to) cutting confidence, the
// merged reservoir can be *decorrelated without bias* by one intra-face
// Metropolis-Hastings mutation step (cf. Sawhney et al. 2024, made cheap here
// because emitted radiance is constant across an emissive voxel face and the
// reconnection shift has Jacobian 1). Triggered with probability equal to the
// pixel's duplication score; the sample point is jittered within its emissive
// face, accepted with the MH ratio of the pipeline's resampling target (times
// one-DDA-ray visibility), and the contribution weight is rescaled by
// p-hat(y)/p-hat(y') so the pair (y, W) stays GRIS-unbiased:
//   E[W * (p-hat(y)/p-hat(y')) * f(y')] = integral of f
// for any MH kernel that preserves p-hat. Confidence is left unchanged.
// ---------------------------------------------------------------------------

@group(0) @binding(4) var gbufCur  : texture_2d<f32>;
@group(0) @binding(6) var gbufPrev : texture_2d<f32>;
@group(0) @binding(7) var<storage, read_write> reservoirsA : array<Reservoir>;
@group(0) @binding(9) var<storage, read> reservoirsB : array<Reservoir>;
@group(0) @binding(12) var dupTex  : texture_2d<f32>;

fn validPrev(prevPix : vec2<i32>, dims : vec2<i32>, worldPos : vec3<f32>, n1 : vec3<f32>) -> bool {
  if (any(prevPix < vec2<i32>(0)) || any(prevPix >= dims)) { return false; }
  let pg = textureLoad(gbufPrev, prevPix, 0);
  if (pg.w <= 0.0) { return false; }
  let expectedT = length(worldPos - u.prevCamPos.xyz);
  let depthOk = abs(pg.w - expectedT) < 0.08 * expectedT + 0.02;
  let normalOk = dot(pg.xyz, n1) > 0.9;
  return depthOk && normalOk;
}

// Triangle-wave fold of x into [0,1]: reflection at both edges (period-2
// fold). Reflection composes a symmetric jitter kernel with a
// measure-preserving map of the face onto itself, so the folded proposal
// density stays symmetric: q(y -> y') = q(y' -> y), and the plain MH ratio
// applies without a proposal correction.
fn reflect01(x : f32) -> f32 {
  let w = x - 2.0 * floor(x * 0.5);   // w in [0, 2)
  return select(w, 2.0 - w, w > 1.0);
}

// One DDA visibility ray from the receiver, matching the fixed RF_FULLV
// pattern in pathtrace/reuse_spatial: the ray starts at the receiver's
// *offset* surface point and the aim vector is computed from that same
// offset point (aiming with the unoffset x1 falsely self-occludes points).
fn mutVisible(pt : vec3<f32>, x1 : vec3<f32>, n1 : vec3<f32>) -> bool {
  let surf = x1 + n1 * 1e-3;
  let d = pt - surf;
  let r = length(d);
  if (r < 2e-3) { return true; }
  return !traceShadow(surf, d / r, r - 2e-3);
}

// RF_MUTATE: one Metropolis-Hastings mutation of the merged reservoir sample
// within its emissive voxel face (see header). Applies only to SK_POINT
// samples that store the face-constant emitted radiance Le; everything else
// is returned unchanged. On acceptance the sample point moves and W is
// rescaled by p-hat(y)/p-hat(y'); confidence, radiance, normal and seed are
// never touched.
fn mutateOnEmissiveFace(selIn : Sample, x1 : vec3<f32>, n1 : vec3<f32>) -> Sample {
  var sel = selIn;
  if (sel.kind != SK_POINT) { return sel; }

  // Identify the voxel that owns the sampled face: the solid side of the
  // face plane sits half a voxel against the stored (axis-aligned, exactly
  // oct-roundtripped) face normal.
  let pv = sel.pos * INV_VOXEL;
  let ivox = vec3<i32>(floor(pv - sel.n * 0.5));
  if (any(ivox < vec3<i32>(0)) || any(ivox >= vec3<i32>(GRID))) { return sel; }
  let mat = unpackMaterial(voxelAt(ivox));
  if (mat.emissive <= 0.0) { return sel; }

  // Sample-class check. Only light-list NEE samples carry the face-constant
  // Le; a BSDF bounce sample that merely landed on an emissive voxel stores
  // a stochastic path estimate of Lo that is NOT constant across the face
  // and must not be moved. Le quantized through the reservoir's f16 packing
  // equals the stored radiance exactly for the NEE class.
  let le = mat.albedo * (mat.emissive * EMISSIVE_SCALE);
  let leQ = vec3<f32>(unpack2x16float(pack2x16float(le.rg)),
                      unpack2x16float(pack2x16float(vec2<f32>(le.b, 0.0))).x);
  if (any(sel.rad != leQ)) { return sel; }

  // MH target p-hat = the pipeline's scalar resampling target for this class
  // (evalTarget: luminance of the visibility-free area-measure integrand,
  // including its distance clamping) times binary receiver visibility.
  // p-hat(y) == 0 (zero target or occluded): skip the mutation entirely.
  let pCur = evalTarget(sel, x1, n1);
  if (pCur <= 0.0) { return sel; }
  if (!mutVisible(sel.pos, x1, n1)) { return sel; }

  // Symmetric proposal: uniform square jitter of half-width params5.w
  // (?mutscale=, in units of the face size) in the face tangent plane,
  // reflected at the face edges to stay on the face. The normal-axis
  // coordinate is left untouched so the point stays exactly on the plane.
  var axis = 2u;
  if (abs(sel.n.x) > 0.5) { axis = 0u; } else if (abs(sel.n.y) > 0.5) { axis = 1u; }
  let t1 = (axis + 1u) % 3u;
  let t2 = (axis + 2u) % 3u;
  let jit = (rand2() * 2.0 - 1.0) * u.params5.w;
  var q = pv;
  q[t1] = f32(ivox[t1]) + reflect01(pv[t1] - f32(ivox[t1]) + jit.x);
  q[t2] = f32(ivox[t2]) + reflect01(pv[t2] - f32(ivox[t2]) + jit.y);

  // Re-encode the proposal into the reservoir's stored representation FIRST
  // and evaluate the MH ratio on the decoded result: stationarity must hold
  // on exactly the states the reservoir can represent, or quantization
  // breaks the invariant distribution.
  var prop = sel;
  prop.pos = q * VOXEL_SIZE;
  let propQ = unpackReservoir(packReservoir(prop));
  let pProp = evalTarget(propQ, x1, n1);

  // Accept with a = min(1, p-hat(y')/p-hat(y)); the proposal's visibility
  // ray is only worth tracing when its visibility-free target is nonzero.
  if (pProp > 0.0 && mutVisible(propQ.pos, x1, n1)) {
    if (rand() * pCur < pProp) {
      sel.pos = propQ.pos;
      sel.W = sel.W * (pCur / pProp);   // contribution-weight rescale
    }
  }
  return sel;
}

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  let dims = vec2<u32>(u32(u.params1.x), u32(u.params1.y));
  if (gid.x >= dims.x || gid.y >= dims.y) { return; }
  if (!rflag(RF_RESTIR) || !rflag(RF_TEMPORAL)) { return; }

  let pix = vec2<i32>(gid.xy);
  let pixIdx = gid.y * dims.x + gid.x;
  let g = textureLoad(gbufCur, pix, 0);
  if (g.w <= 0.0) { return; }

  initRng(gid.xy, u.params0.x ^ 0x5f356495u);

  let uv = (vec2<f32>(gid.xy) + 0.5) / vec2<f32>(dims);
  let x1 = u.camPos.xyz + cameraRay(uv) * g.w;
  let n1 = g.xyz;

  // Reproject into the previous frame.
  let prevClip = u.prevViewProj * vec4<f32>(x1, 1.0);
  if (prevClip.w <= 0.0) { return; }
  let prevNdc = prevClip.xyz / prevClip.w;
  let prevUv = vec2<f32>(prevNdc.x, -prevNdc.y) * 0.5 + 0.5;
  var prevPix = vec2<i32>(prevUv * vec2<f32>(dims));

  let idims = vec2<i32>(dims);
  var found = validPrev(prevPix, idims, x1, n1);
  if (!found && rflag(RF_RESCUE)) {
    // Disocclusion rescue: any validating neighbor history is the same
    // surface in a static world.
    for (var dy = -1; dy <= 1 && !found; dy++) {
      for (var dx = -1; dx <= 1 && !found; dx++) {
        let q = prevPix + vec2<i32>(dx, dy);
        if ((dx != 0 || dy != 0) && validPrev(q, idims, x1, n1)) {
          prevPix = q;
          found = true;
        }
      }
    }
  }
  if (!found) { return; }

  let prevIdx = u32(prevPix.y) * dims.x + u32(prevPix.x);
  let st = unpackReservoir(reservoirsB[prevIdx]);
  if (st.kind == SK_NONE) { return; }

  // Duplication score at the reprojected pixel — shared lookup for the
  // adaptive cCap (§5) and the RF_MUTATE trigger (the dupmap pass is
  // dispatched whenever either flag is on).
  var dupScore = 0.0;
  if (rflag(RF_DUPMAP) || rflag(RF_MUTATE)) {
    dupScore = clamp(textureLoad(dupTex, prevPix, 0).x, 0.0, 1.0);
  }

  // Adaptive confidence cap from the duplication score (§5).
  var cap = u.params3.x;
  if (rflag(RF_DUPMAP)) {
    cap = mix(u.params3.x, u.params3.y, pow(dupScore, u.params3.z));
  }
  let cT = min(st.c, cap);

  let sc = unpackReservoir(reservoirsA[pixIdx]);
  let cC = max(sc.c, 0.0);

  // Constant (confidence-proportional) MIS: the temporal domain is the same
  // surface, so target functions match and this stays unbiased in practice.
  let pC = evalTarget(sc, x1, n1);
  let pT = evalTarget(st, x1, n1);
  let mDen = max(cC + cT, 1e-3);
  let wC = (cC / mDen) * pC * max(sc.W, 0.0);
  let wT = (cT / mDen) * pT * max(st.W, 0.0);
  let wSum = wC + wT;
  if (wSum <= 0.0) { return; }

  var sel = sc;
  var selP = pC;
  if (rand() * wSum >= wC) {
    sel = st;
    selP = pT;
  }
  if (selP <= 0.0) { return; }

  sel.W = wSum / selP;
  sel.c = cC + cT;

  // RF_MUTATE: MCMC decorrelation of the merged reservoir before spatial
  // reuse. Trigger probability = the pixel's duplication score (dup = 0
  // never mutates), so mutation effort goes exactly where correlation is.
  if (rflag(RF_MUTATE) && rand() < dupScore) {
    sel = mutateOnEmissiveFace(sel, x1, n1);
  }

  reservoirsA[pixIdx] = packReservoir(sel);
}
