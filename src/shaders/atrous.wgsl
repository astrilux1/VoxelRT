// ---------------------------------------------------------------------------
// atrous.wgsl — edge-aware à-trous wavelet filter (SVGF-style, simplified).
// Runs a few iterations with growing step size; weights fall off across
// normal, depth and luminance edges. Filter strength relaxes as temporal
// history accumulates, so a converged image is barely blurred.
// ---------------------------------------------------------------------------

@group(0) @binding(3) var srcTex  : texture_2d<f32>;
@group(0) @binding(4) var gbufCur : texture_2d<f32>;
@group(0) @binding(5) var dstTex  : texture_storage_2d<rgba16float, write>;
// Per-pixel estimator-quality signal q in the alpha channel (written by
// reuse_spatial.wgsl); only read when RF_CONFDENOISE is set.
@group(0) @binding(7) var radianceTex : texture_2d<f32>;

struct AtrousParams {
  stepSize : vec4<i32>,   // x = step in pixels
};
@group(0) @binding(6) var<uniform> ap : AtrousParams;

const KERNEL = array<f32, 3>(0.375, 0.25, 0.0625);   // B3-spline

fn luminance(c : vec3<f32>) -> f32 {
  return dot(c, vec3<f32>(0.2126, 0.7152, 0.0722));
}

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  let dims = vec2<i32>(i32(u.params1.x), i32(u.params1.y));
  let pix = vec2<i32>(gid.xy);
  if (pix.x >= dims.x || pix.y >= dims.y) { return; }

  let center = textureLoad(srcTex, pix, 0);
  let g = textureLoad(gbufCur, pix, 0);

  if (g.w < 0.0) {   // sky: nothing to denoise
    textureStore(dstTex, pix, center);
    return;
  }

  let history = max(center.a, 1.0);
  let lumC = luminance(center.rgb);
  // Less smoothing once the temporal accumulation has converged.
  var relax = 2.0 / sqrt(history);
  if (rflag(RF_CONFDENOISE)) {
    // Reservoir-quality-driven strength on top of the history relaxation:
    // sigmaL scales by 1 + confk*(1-q) (confk = params5.z, ?confk= knob).
    // q = 1 (confident, decorrelated reservoir) leaves the filter at its
    // baseline; q = 0 (disocclusion / correlation blob) widens the
    // luminance kernel by (1 + confk). History is already shortened for
    // low q by temporal.wgsl, so the two effects compound where the
    // estimator is genuinely poor and vanish where it is effectively
    // many-sample.
    let q = clamp(textureLoad(radianceTex, pix, 0).a, 0.0, 1.0);
    relax *= 1.0 + u.params5.z * (1.0 - q);
  }
  let sigmaL = max(0.08, relax);

  var sum = center.rgb * KERNEL[0] * KERNEL[0];
  var wSum = KERNEL[0] * KERNEL[0];

  for (var dy = -2; dy <= 2; dy++) {
    for (var dx = -2; dx <= 2; dx++) {
      if (dx == 0 && dy == 0) { continue; }
      let q = pix + vec2<i32>(dx, dy) * ap.stepSize.x;
      if (q.x < 0 || q.y < 0 || q.x >= dims.x || q.y >= dims.y) { continue; }

      let cq = textureLoad(srcTex, q, 0);
      let gq = textureLoad(gbufCur, q, 0);
      if (gq.w < 0.0) { continue; }

      let wKernel = KERNEL[abs(dx)] * KERNEL[abs(dy)];
      let wNormal = pow(max(dot(g.xyz, gq.xyz), 0.0), 32.0);
      let wDepth = exp(-abs(g.w - gq.w) / (0.05 * g.w + 0.01));
      let wLum = exp(-abs(lumC - luminance(cq.rgb)) / sigmaL);

      let w = wKernel * wNormal * wDepth * wLum;
      sum += cq.rgb * w;
      wSum += w;
    }
  }

  textureStore(dstTex, pix, vec4<f32>(sum / max(wSum, 1e-6), center.a));
}
