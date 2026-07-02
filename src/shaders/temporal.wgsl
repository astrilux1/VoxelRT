// ---------------------------------------------------------------------------
// temporal.wgsl — reprojects the previous frame's accumulated illumination
// into the current frame (the scene is static, so camera-only reprojection is
// exact) and blends the new 1-spp estimate in. History length rides in alpha.
// ---------------------------------------------------------------------------

@group(0) @binding(3) var radianceCur : texture_2d<f32>;
@group(0) @binding(4) var gbufCur     : texture_2d<f32>;
@group(0) @binding(5) var accumPrev   : texture_2d<f32>;
@group(0) @binding(6) var gbufPrev    : texture_2d<f32>;
@group(0) @binding(7) var accumOut    : texture_storage_2d<rgba16float, write>;
@group(0) @binding(8) var linearSamp  : sampler;

// Maximum accumulated history length comes from params4.y (default 64;
// the benchmark's reference mode raises it to accumulate indefinitely).

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  let dims = vec2<u32>(u32(u.params1.x), u32(u.params1.y));
  if (gid.x >= dims.x || gid.y >= dims.y) { return; }
  let pix = vec2<i32>(gid.xy);

  let cur = textureLoad(radianceCur, pix, 0).rgb;
  let g = textureLoad(gbufCur, pix, 0);
  let temporalOn = (u.params0.z & 1u) != 0u;

  if (g.w < 0.0 || !temporalOn) {
    // Sky (analytic, nearly noise-free) or temporal disabled: no history.
    textureStore(accumOut, pix, vec4<f32>(cur, 1.0));
    return;
  }

  // Reconstruct the world position of this pixel and project it into the
  // previous frame.
  let uv = (vec2<f32>(gid.xy) + 0.5) / vec2<f32>(dims);
  let worldPos = u.camPos.xyz + cameraRay(uv) * g.w;
  let prevClip = u.prevViewProj * vec4<f32>(worldPos, 1.0);

  var color = cur;
  var history = 1.0;

  if (prevClip.w > 0.0) {
    let prevNdc = prevClip.xyz / prevClip.w;
    let prevUv = vec2<f32>(prevNdc.x, -prevNdc.y) * 0.5 + 0.5;

    if (all(prevUv > vec2<f32>(0.0)) && all(prevUv < vec2<f32>(1.0))) {
      let prevPix = vec2<i32>(prevUv * vec2<f32>(dims));
      let pg = textureLoad(gbufPrev, prevPix, 0);

      // Validate: same surface (depth along previous ray) and similar normal.
      let expectedT = length(worldPos - u.prevCamPos.xyz);
      let depthOk = pg.w > 0.0 && abs(pg.w - expectedT) < 0.08 * expectedT + 0.02;
      let normalOk = dot(pg.xyz, g.xyz) > 0.85;

      if (depthOk && normalOk) {
        let prev = textureSampleLevel(accumPrev, linearSamp, prevUv, 0.0);
        history = min(prev.a, u.params4.y) + 1.0;
        let alpha = 1.0 / history;
        color = mix(prev.rgb, cur, alpha);
      }
    }
  }

  textureStore(accumOut, pix, vec4<f32>(color, history));
}
