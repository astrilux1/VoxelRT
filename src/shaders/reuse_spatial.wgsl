// ---------------------------------------------------------------------------
// reuse_spatial.wgsl — spatial reservoir reuse + final shading.
//
// Neighbors come from *paired reuse textures* (Lin 2026 §3): self-inverting
// Gaussian permutations, so if A reuses from B then B reuses from A. Pairing
// makes reuse symmetric (each sample can be adopted by at most one partner
// per texture, curbing the sample-duplication blowup of random neighbor
// picks) and concentrates neighbors under a Gaussian, which the paper shows
// beats a uniform disk at equal mean distance. A uniform-disk fallback
// (classic ReSTIR spatial reuse) is kept for ablations.
//
// Candidates are merged with a confidence-weighted balance heuristic; the
// shift is a pure reconnection re-evaluation (area-measure samples), so MIS
// terms are analytic and cost no rays. Reconnections shorter than the
// footprint criterion (§4) are skipped. Shading uses vector-valued
// resampling weights (§6.3): every candidate's full contribution (including
// its own visibility ray) is accumulated, so chroma noise averages out and
// occlusion is exact per candidate.
//
// Output: demodulated radiance = pass-through direct + reservoir shading,
// and the post-reuse reservoir that becomes next frame's temporal history.
// ---------------------------------------------------------------------------

@group(0) @binding(3) var radianceOut : texture_storage_2d<rgba16float, write>;
@group(0) @binding(4) var gbufCur : texture_2d<f32>;
@group(0) @binding(7) var<storage, read> reservoirsA : array<Reservoir>;
@group(0) @binding(8) var directTex : texture_2d<f32>;
@group(0) @binding(9) var<storage, read_write> reservoirsB : array<Reservoir>;
// Concatenated pairing textures (2 x i16 coordinate deltas per texel).
@group(0) @binding(13) var<storage, read> pairing : array<u32>;

// Pairing texture sizes are mutually near-coprime so tiling periods do not
// align (cf. Lin 2026 footnote 3).
fn pairSize(ti : u32) -> i32 {
  if (ti == 0u) { return 254; }
  if (ti == 1u) { return 230; }
  return 210;
}

fn pairOff(ti : u32) -> u32 {
  if (ti == 0u) { return 0u; }
  if (ti == 1u) { return 64516u; }        // 254^2
  return 117416u;                          // + 230^2
}

fn wrapMod(v : i32, s : i32) -> i32 {
  return ((v % s) + s) % s;
}

// Mutual partner of `pix` for pairing texture `ti`, varied per frame by a
// random offset/mirror/swap (the texture is self-inverting; the transform
// keeps it so). Returns pix itself when the partner falls off screen.
fn pairedPartner(pix : vec2<i32>, ti : u32) -> vec2<i32> {
  let s = pairSize(ti);
  let h = pcgHash(u.params0.x * 2654435761u + ti * 0x68bc21ebu + 0x9e3779b9u);
  let h2 = pcgHash(h);
  let off = vec2<i32>(i32(h % u32(s)), i32(h2 % u32(s)));
  let mx = (h & 0x40000000u) != 0u;
  let my = (h & 0x20000000u) != 0u;
  let sw = (h & 0x10000000u) != 0u;

  var p = vec2<i32>(wrapMod(pix.x + off.x, s), wrapMod(pix.y + off.y, s));
  if (sw) { p = p.yx; }
  if (mx) { p.x = s - 1 - p.x; }
  if (my) { p.y = s - 1 - p.y; }

  let word = pairing[pairOff(ti) + u32(p.y * s + p.x)];
  let dT = vec2<i32>(i32(word << 16u) >> 16u, i32(word) >> 16u);

  var q = vec2<i32>(wrapMod(p.x + dT.x, s), wrapMod(p.y + dT.y, s));
  if (mx) { q.x = s - 1 - q.x; }
  if (my) { q.y = s - 1 - q.y; }
  if (sw) { q = q.yx; }
  var d = vec2<i32>(wrapMod(q.x - off.x - pix.x, s), wrapMod(q.y - off.y - pix.y, s));
  if (d.x > s / 2) { d.x -= s; }
  if (d.y > s / 2) { d.y -= s; }
  return pix + d;
}

fn surfaceAt(q : vec2<i32>, dims : vec2<f32>, t : f32) -> vec3<f32> {
  let uv = (vec2<f32>(q) + 0.5) / dims;
  return u.camPos.xyz + cameraRay(uv) * t;
}

// Can `s` illuminate the receiver? One shadow ray.
fn sampleVisible(s : Sample, x1 : vec3<f32>, n1 : vec3<f32>) -> bool {
  let surf = x1 + n1 * 1e-3;
  if (s.kind == SK_DIR) {
    return !traceShadow(surf, s.pos, 1e4);
  }
  let d = s.pos - x1;
  let r = length(d);
  if (r < 2e-3) { return true; }
  return !traceShadow(surf, d / r, r - 2e-3);
}

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  let dims = vec2<u32>(u32(u.params1.x), u32(u.params1.y));
  if (gid.x >= dims.x || gid.y >= dims.y) { return; }
  let pix = vec2<i32>(gid.xy);
  let pixIdx = gid.y * dims.x + gid.x;
  let fdims = vec2<f32>(dims);

  let direct = textureLoad(directTex, pix, 0).rgb;
  let g = textureLoad(gbufCur, pix, 0);
  if (g.w <= 0.0) {
    // Sky: direct already carries the analytic sky radiance.
    textureStore(radianceOut, pix, vec4<f32>(direct, 1.0));
    reservoirsB[pixIdx] = emptyReservoir();
    return;
  }

  initRng(gid.xy, u.params0.x ^ 0x2c1b3c6du);

  let x1 = surfaceAt(pix, fdims, g.w);
  let n1 = g.xyz;
  let tPrim = g.w;
  let cosView = max(dot(n1, normalize(u.camPos.xyz - x1)), 0.0);

  // Gather candidates: canonical + up to 3 spatial neighbors.
  var cand : array<Sample, 4>;
  var candX1 : array<vec3<f32>, 4>;
  var candN1 : array<vec3<f32>, 4>;
  var nCand = 0u;

  let canonical = unpackReservoir(reservoirsA[pixIdx]);
  if (canonical.kind != SK_NONE) {
    cand[0] = canonical;
    candX1[0] = x1;
    candN1[0] = n1;
    nCand = 1u;
  }

  let spatialOn = rflag(RF_SPATIAL);
  let paired = rflag(RF_PAIRED);
  let planeExact = rflag(RF_PLANE);
  let taps = min(u.params2.z, 3u);

  if (spatialOn) {
    for (var ti = 0u; ti < taps; ti++) {
      var q : vec2<i32>;
      if (paired) {
        q = pairedPartner(pix, ti);
      } else {
        // Classic random neighbor in a uniform disk (radius in params4.x).
        let r = u.params4.x * sqrt(rand());
        let a = 2.0 * PI * rand();
        q = pix + vec2<i32>(vec2<f32>(cos(a), sin(a)) * r);
      }
      if (all(q == pix) || any(q < vec2<i32>(0)) || any(q >= vec2<i32>(dims))) { continue; }

      let gq = textureLoad(gbufCur, q, 0);
      if (gq.w <= 0.0) { continue; }
      let x1q = surfaceAt(q, fdims, gq.w);

      // Neighbor compatibility. The baseline test (normal + relative depth)
      // is what generic ReSTIR uses; the plane-exact test additionally
      // accepts any point on the *same voxel face plane* regardless of
      // depth — on axis-aligned voxel geometry this is a guaranteed-valid
      // reuse partner, not a heuristic.
      let nDot = dot(n1, gq.xyz);
      let genericOk = nDot > 0.9 && abs(gq.w - g.w) < 0.1 * g.w + 0.02;
      let samePlane = nDot > 0.99 && abs(dot(n1, x1q - x1)) < 0.02;
      let ok = select(genericOk, genericOk || samePlane, planeExact);
      if (!ok) { continue; }

      let rq = unpackReservoir(reservoirsA[u32(q.y) * dims.x + u32(q.x)]);
      if (rq.kind == SK_NONE) { continue; }
      if (rflag(RF_FOOTPRINT) && !footprintOK(rq, x1, n1, tPrim, cosView)) { continue; }

      cand[nCand] = rq;
      candX1[nCand] = x1q;
      candN1[nCand] = gq.xyz;
      nCand++;
    }
  }

  if (nCand == 0u) {
    textureStore(radianceOut, pix, vec4<f32>(direct, 1.0));
    reservoirsB[pixIdx] = emptyReservoir();
    return;
  }

  // Confidence-weighted balance-heuristic MIS across candidate domains —
  // analytic here (the reconnection shift needs no rays), so we can afford
  // the full heuristic instead of pairwise approximations.
  let vector = rflag(RF_VECTOR);
  let fullV = rflag(RF_FULLV);

  var wSum = 0.0;
  var contrib = vec3<f32>(0.0);
  var sel : Sample;
  sel.kind = SK_NONE;
  var selP = 0.0;
  var selJ = 0u;
  var cTotal = 0.0;

  for (var j = 0u; j < nCand; j++) {
    let s = cand[j];
    cTotal += s.c;

    var denom = 0.0;
    for (var l = 0u; l < nCand; l++) {
      denom += cand[l].c * evalTarget(s, candX1[l], candN1[l]);
    }
    if (denom <= 0.0) { continue; }
    let m = cand[j].c * evalTarget(s, candX1[j], candN1[j]) / denom;

    let pMe = evalTarget(s, x1, n1);
    let w = m * pMe * max(s.W, 0.0);
    if (w > 0.0) {
      wSum += w;
      if (rand() * wSum < w) { sel = s; selP = pMe; selJ = j; }
    }

    if (vector && w > 0.0) {
      // Vector-valued shading weight (§6.3) with true per-candidate
      // visibility: unbiased, and chroma noise averages across candidates.
      var vis = true;
      if (j > 0u || fullV) { vis = sampleVisible(s, x1, n1); }
      if (vis) {
        contrib += evalF(s, x1, n1) * (m * max(s.W, 0.0));
      }
    }
  }

  if (sel.kind == SK_NONE || selP <= 0.0 || wSum <= 0.0) {
    textureStore(radianceOut, pix, vec4<f32>(direct, 1.0));
    reservoirsB[pixIdx] = emptyReservoir();
    return;
  }

  sel.W = wSum / selP;
  sel.c = min(cTotal, 255.0);

  if (!vector) {
    var vis = true;
    if (selJ > 0u || fullV) { vis = sampleVisible(sel, x1, n1); }
    if (vis) { contrib = evalF(sel, x1, n1) * sel.W; }
  }

  if (rflag(RF_CLAMP)) {
    let l = lum(contrib);
    if (l > u.params4.z) { contrib *= u.params4.z / l; }
  }

  textureStore(radianceOut, pix, vec4<f32>(direct + contrib, 1.0));
  reservoirsB[pixIdx] = packReservoir(sel);
}
