// ---------------------------------------------------------------------------
// pathtrace.wgsl — primary visibility + initial reservoir candidates.
//
// Baseline mode (RF_RESTIR off): the original 1-spp path tracer, radiance
// written directly (albedo-demodulated).
//
// ReSTIR mode: one *path tree* per pixel (Lin 2026 §6.1) produces up to three
// initial candidates for the pixel's unified reservoir:
//   1. a sun-cone NEE sample at the primary hit          (direct light)
//   2. a light-list NEE sample at the primary hit, itself picked by RIS
//      over several emissive voxel faces                 (direct light)
//   3. a BSDF bounce path whose reconnection vertex x2 carries the full
//      outgoing radiance estimated by continuing the path (indirect light)
// Initial RIS selects one; temporal/spatial passes then reuse it. Radiance
// that never flows through the reservoir (primary-hit emission, sky, and —
// when unification is disabled — analytic sun NEE) goes to `directOut`.
// ---------------------------------------------------------------------------

@group(0) @binding(3) var radianceOut : texture_storage_2d<rgba16float, write>;
@group(0) @binding(4) var albedoOut   : texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(5) var gbufOut     : texture_storage_2d<rgba32float, write>;
// Emissive voxel faces: x = coords/face (x|y<<9|z<<18|face<<27), y = material.
@group(0) @binding(6) var<storage, read> lights : array<vec2<u32>>;
@group(0) @binding(7) var<storage, read_write> reservoirsA : array<Reservoir>;
@group(0) @binding(8) var directOut   : texture_storage_2d<rgba16float, write>;
// x = alias acceptance probability, y = alias index, z = selection PDF.
@group(0) @binding(9) var<storage, read> lightAlias : array<vec4<f32>>;

const FIREFLY_CLAMP : f32 = 48.0;

struct LightPoint {
  pos  : vec3<f32>,   // world position on the face
  n    : vec3<f32>,   // face normal
  Le   : vec3<f32>,   // emitted radiance
  pdfA : f32,         // area-measure pdf
};

fn sampleLightFace() -> LightPoint {
  var lp : LightPoint;
  let count = u.params2.y;
  var li : u32;
  var pdfSelect : f32;
  if (rflag(RF_LIGHTPOWER)) {
    let uLight = rand() * f32(count);
    let base = min(u32(uLight), count - 1u);
    let frac = uLight - f32(base);
    let rec = lightAlias[base];
    let aliasLi = min(u32(rec.y + 0.5), count - 1u);
    li = select(base, aliasLi, frac >= rec.x);
    pdfSelect = max(lightAlias[li].z, 1e-8);
  } else {
    li = min(u32(rand() * f32(count)), count - 1u);
    pdfSelect = 1.0 / f32(count);
  }
  let l = lights[li];
  let v = vec3<f32>(f32(l.x & 0x1ffu), f32((l.x >> 9u) & 0x1ffu), f32((l.x >> 18u) & 0x1ffu));
  let face = (l.x >> 27u) & 7u;
  let axis = face >> 1u;
  let pos = select(0.0, 1.0, (face & 1u) == 0u);   // +side faces sit at v+1
  var q = v + vec3<f32>(rand(), rand(), rand());   // jitter; axis coord replaced
  var n = vec3<f32>(0.0);
  if (axis == 0u)      { q.x = v.x + pos; n.x = select(-1.0, 1.0, pos > 0.5); }
  else if (axis == 1u) { q.y = v.y + pos; n.y = select(-1.0, 1.0, pos > 0.5); }
  else                 { q.z = v.z + pos; n.z = select(-1.0, 1.0, pos > 0.5); }
  let mat = unpackMaterial(l.y);
  lp.pos = q * VOXEL_SIZE;
  lp.n = n;
  lp.Le = mat.albedo * (mat.emissive * EMISSIVE_SCALE);
  lp.pdfA = pdfSelect / (VOXEL_SIZE * VOXEL_SIZE);
  return lp;
}

// Emissive-NEE RIS candidate budget at bounce k (primary hit = bounce 0).
// Fixed at the historical 4 (primary) / 1 (secondary) split unless
// RF_ADAPTCAND is set, in which case the primary-hit budget grows
// logarithmically with the scene's light-face count (u.params2.y), scaled by
// the ?candscale= knob (u.params5.y), and halves per bounce — the analog of
// the paper's 32/2^k schedule (Lin 2026 §6.1/§7):
//   N0 = clamp(round(candscale * 4 * (1 + log2(1 + lights/256))), 1, 16)
//   Nk = max(1, N0 >> k)
// N depends only on uniforms, so candidate loops stay uniform control flow.
fn emissiveCandCount(bounce : u32) -> u32 {
  if (!rflag(RF_ADAPTCAND)) { return select(1u, 4u, bounce == 0u); }
  let n0 = clamp(round(u.params5.y * 4.0 * (1.0 + log2(1.0 + f32(u.params2.y) / 256.0))), 1.0, 16.0);
  return max(u32(n0) >> min(bounce, 31u), 1u);
}

// Direct emissive light at a path vertex: RIS over nCand light-list samples
// (full, non-demodulated contribution; used at secondary vertices). Target
// p-hat = lum(f); each candidate carries v = f/pdfA and resampling weight
// lum(f)/(M*pdfA) = lum(v)/M, so the estimator V(y)*v_y*wSum/lum(v_y) is
// unbiased for any M; one shadow ray on the survivor only. At M = 1 this
// reduces exactly (values and RNG stream) to the plain one-sample estimate.
fn emissiveNEE(pos : vec3<f32>, n : vec3<f32>, albedo : vec3<f32>, nCand : u32) -> vec3<f32> {
  if (u.params2.y == 0u) { return vec3<f32>(0.0); }
  let surf = pos + n * 1e-3;
  let invM = 1.0 / f32(nCand);
  var wSum = 0.0;
  var sel = vec3<f32>(0.0);   // f/pdfA of the selected candidate
  var selLum = 0.0;
  var selDir = vec3<f32>(0.0);
  var selR2 = 0.0;
  for (var k = 0u; k < nCand; k++) {
    let lp = sampleLightFace();
    let d = lp.pos - surf;
    let r2 = max(dot(d, d), 1e-6);
    let dir = d * inverseSqrt(r2);
    let cos1 = dot(n, dir);
    let cos2 = dot(lp.n, -dir);
    if (cos1 <= 0.0 || cos2 <= 0.0) { continue; }
    let v = albedo * INV_PI * cos1 * cos2 / r2 * lp.Le / lp.pdfA;
    let w = lum(v) * invM;
    if (w <= 0.0) { continue; }
    let first = wSum == 0.0;
    wSum += w;
    // The first valid candidate wins with probability 1: skipping its rand()
    // keeps the RNG stream identical to the pre-adaptive path when M = 1.
    if (first || rand() * wSum < w) {
      sel = v; selLum = lum(v); selDir = dir; selR2 = r2;
    }
  }
  if (wSum <= 0.0) { return vec3<f32>(0.0); }
  let r = sqrt(selR2);
  if (traceShadow(surf, selDir, r - 2e-3)) { return vec3<f32>(0.0); }
  return sel * (wSum / selLum);
}

// Outgoing radiance Lo(x2 -> x1) estimated by continuing the path from x2.
// With unification on, emission on BSDF hits is dropped everywhere (covered
// by light-list NEE at the previous vertex); with it off, emission rides on
// hits and there is no light-list NEE — matching the baseline estimator.
fn estimateLo(x2 : vec3<f32>, n2In : vec3<f32>, mat2 : Material, unified : bool) -> vec3<f32> {
  var Lo = vec3<f32>(0.0);
  var tp = vec3<f32>(1.0);
  var pos = x2;
  var n = n2In;
  var mat = mat2;
  let nBounces = u.params0.y;

  for (var v = 1u; v <= nBounces; v++) {
    if (!unified && mat.emissive > 0.0 && v > 1u) {
      Lo += tp * mat.albedo * (mat.emissive * EMISSIVE_SCALE);
    }
    let surf = pos + n * 1e-3;

    let l = sampleSunDir();
    let ndl = dot(n, l);
    if (ndl > 0.0 && l.y > 0.0 && !traceShadow(surf, l, 1e4)) {
      Lo += tp * mat.albedo * INV_PI * ndl * u.sunRadiance.rgb;
    }
    if (unified) {
      Lo += tp * emissiveNEE(pos, n, mat.albedo, emissiveCandCount(v));
    }
    if (v == nBounces) { break; }

    let rd = cosineHemisphere(n);
    tp *= mat.albedo;
    if (v >= 2u) {
      let p = clamp(max(tp.x, max(tp.y, tp.z)), 0.05, 0.95);
      if (rand() > p) { break; }
      tp /= p;
    }
    let h = trace(surf, rd, 1e4);
    if (h.t < 0.0) {
      Lo += tp * skyColor(rd, false);
      break;
    }
    pos = surf + rd * h.t;
    n = h.n;
    mat = unpackMaterial(h.mat);
  }
  return min(Lo, vec3<f32>(FIREFLY_CLAMP));
}

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  let dims = vec2<u32>(u32(u.params1.x), u32(u.params1.y));
  if (gid.x >= dims.x || gid.y >= dims.y) { return; }
  let pix = vec2<i32>(gid.xy);
  let pixIdx = gid.y * dims.x + gid.x;

  initRng(gid.xy, u.params0.x);

  // Sub-pixel jitter for anti-aliasing; temporal accumulation integrates it.
  let jitter = rand2() - 0.5;
  let uv = (vec2<f32>(gid.xy) + 0.5 + jitter) / vec2<f32>(dims);

  var ro = u.camPos.xyz;
  var rd = cameraRay(uv);

  let restir = rflag(RF_RESTIR);
  let h0 = trace(ro, rd, 1e4);

  if (h0.t < 0.0) {
    // Sky: analytic, nearly noise-free; bypasses the reservoir entirely.
    let sky = skyColor(rd, true);
    textureStore(radianceOut, pix, vec4<f32>(sky, 1.0));
    textureStore(directOut, pix, vec4<f32>(sky, 1.0));
    textureStore(albedoOut, pix, vec4<f32>(1.0));
    textureStore(gbufOut, pix, vec4<f32>(-rd, -1.0));
    if (restir) { reservoirsA[pixIdx] = emptyReservoir(); }
    return;
  }

  let mat1 = unpackMaterial(h0.mat);
  let x1 = ro + rd * h0.t;
  let n1 = h0.n;
  let surf1 = x1 + n1 * max(1e-3, h0.t * 1e-4);

  textureStore(albedoOut, pix, vec4<f32>(sqrt(mat1.albedo), 1.0));
  textureStore(gbufOut, pix, vec4<f32>(n1, h0.t));

  if (!restir) {
    // ------------------------------------------------------------------
    // Baseline: original 1-spp multi-bounce path tracer.
    // ------------------------------------------------------------------
    var radiance = vec3<f32>(0.0);
    var throughput = vec3<f32>(1.0);
    var pos = x1;
    var n = n1;
    var mat = mat1;
    let nBounces = u.params0.y;

    for (var bounce = 0u; bounce <= nBounces; bounce++) {
      if (mat.emissive > 0.0) {
        radiance += throughput * mat.albedo * (mat.emissive * EMISSIVE_SCALE);
      }
      let surf = pos + n * 1e-3;
      let l = sampleSunDir();
      let ndl = dot(n, l);
      if (ndl > 0.0 && l.y > 0.0 && !traceShadow(surf, l, 1e4)) {
        radiance += throughput * mat.albedo * INV_PI * ndl * u.sunRadiance.rgb;
      }
      if (bounce == nBounces) { break; }

      rd = cosineHemisphere(n);
      throughput *= mat.albedo;
      if (bounce >= 2u) {
        let p = clamp(max(throughput.x, max(throughput.y, throughput.z)), 0.05, 0.95);
        if (rand() > p) { break; }
        throughput /= p;
      }
      let h = trace(surf, rd, 1e4);
      if (h.t < 0.0) {
        radiance += throughput * skyColor(rd, false);
        break;
      }
      pos = surf + rd * h.t;
      n = h.n;
      mat = unpackMaterial(h.mat);
    }

    radiance = min(radiance, vec3<f32>(FIREFLY_CLAMP));
    let demod = radiance / max(mat1.albedo, vec3<f32>(1e-3));
    textureStore(radianceOut, pix, vec4<f32>(demod, 1.0));
    return;
  }

  // --------------------------------------------------------------------
  // ReSTIR initial sampling: build the candidate set, RIS-select one.
  // --------------------------------------------------------------------
  let unified = rflag(RF_UNIFIED);

  // Radiance that bypasses the reservoir (demodulated).
  var direct = vec3<f32>(mat1.emissive * EMISSIVE_SCALE);

  var wSum = 0.0;
  var sel : Sample;
  sel.kind = SK_NONE;
  var selP = 0.0;

  // Candidate: sun cone NEE at x1.
  {
    let l = sampleSunDir();
    let ndl = dot(n1, l);
    if (ndl > 0.0 && l.y > 0.0 && !traceShadow(surf1, l, 1e4)) {
      if (unified) {
        let coneSA = 2.0 * PI * (1.0 - u.sunDir.w);
        var s : Sample;
        s.kind = SK_DIR;
        s.pos = l;
        s.rad = u.sunRadiance.rgb / max(coneSA, 1e-5);
        let p = evalTarget(s, x1, n1);
        let w = p * coneSA;                       // W = 1/pdf = cone solid angle
        if (w > 0.0) {
          wSum += w;
          if (rand() * wSum < w) { sel = s; selP = p; }
        }
      } else {
        direct += INV_PI * ndl * u.sunRadiance.rgb;
      }
    }
  }

  // Candidate: light-list NEE at x1, RIS over emissiveCandCount(0) emissive
  // faces (§6.1) — 4 fixed, or the adaptive budget under RF_ADAPTCAND.
  if (unified && u.params2.y > 0u) {
    let nCand = emissiveCandCount(0u);
    var eSum = 0.0;
    var eSel : Sample;
    eSel.kind = SK_NONE;
    var eSelP = 0.0;
    var eSelPdf = 1.0;
    for (var k = 0u; k < nCand; k++) {
      let lp = sampleLightFace();
      var s : Sample;
      s.kind = SK_POINT;
      s.pos = lp.pos;
      s.n = lp.n;
      s.rad = lp.Le;
      let p = evalTarget(s, x1, n1);
      // 1/M term of the RIS weight uses the ACTUAL candidate count so the
      // resampled estimator stays unbiased for any budget.
      let w = p / (f32(nCand) * lp.pdfA);
      if (w > 0.0) {
        eSum += w;
        if (rand() * eSum < w) { eSel = s; eSelP = p; }
      }
    }
    if (eSel.kind != SK_NONE && eSelP > 0.0) {
      let d = eSel.pos - surf1;
      let r = length(d);
      if (!traceShadow(surf1, d / r, r - 2e-3)) {
        let We = eSum / eSelP;                    // RIS contribution weight
        let w = eSelP * We;                       // = eSum, folded for clarity
        wSum += w;
        if (rand() * wSum < w) { sel = eSel; selP = eSelP; }
      }
    }
  }

  // Candidate: BSDF bounce path — reconnection vertex x2 carries Lo.
  if (u.params0.y > 0u) {
    let brd = cosineHemisphere(n1);
    let h = trace(surf1, brd, 1e4);
    var s : Sample;
    var W = 0.0;
    if (h.t < 0.0) {
      s.kind = SK_DIR;
      s.pos = brd;
      s.rad = skyColor(brd, false);
      let cos1 = max(dot(n1, brd), 1e-4);
      W = PI / cos1;                              // 1/pdf_omega
    } else {
      let x2 = surf1 + brd * h.t;
      let mat2 = unpackMaterial(h.mat);
      var Lo = estimateLo(x2, h.n, mat2, unified);
      if (!unified && mat2.emissive > 0.0) {
        Lo += mat2.albedo * (mat2.emissive * EMISSIVE_SCALE);
      }
      s.kind = SK_POINT;
      s.pos = x2;
      s.n = h.n;
      s.rad = min(Lo, vec3<f32>(FIREFLY_CLAMP));
      let d = x2 - x1;
      let r2 = max(dot(d, d), 1e-6);
      let cos1 = max(dot(n1, normalize(d)), 1e-4);
      let cos2 = max(dot(h.n, -normalize(d)), 1e-4);
      W = PI / cos1 * r2 / cos2;                  // 1/pdf in area measure
    }
    let p = evalTarget(s, x1, n1);
    let w = p * W;
    if (w > 0.0) {
      wSum += w;
      if (rand() * wSum < w) { sel = s; selP = p; }
    }
  }

  var out : Sample;
  out.kind = SK_NONE;
  if (sel.kind != SK_NONE && selP > 0.0) {
    out = sel;
    out.W = wSum / selP;
    out.c = 1.0;
    out.seed = pcgHash(pixIdx + u.params0.x * (dims.x * dims.y + 1u));
  }
  reservoirsA[pixIdx] = packReservoir(out);
  textureStore(directOut, pix, vec4<f32>(direct, 1.0));
}
