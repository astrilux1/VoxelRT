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

  // Adaptive confidence cap from the duplication score (§5).
  var cap = u.params3.x;
  if (rflag(RF_DUPMAP)) {
    let d = clamp(textureLoad(dupTex, prevPix, 0).x, 0.0, 1.0);
    cap = mix(u.params3.x, u.params3.y, pow(d, u.params3.z));
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
  reservoirsA[pixIdx] = packReservoir(sel);
}
