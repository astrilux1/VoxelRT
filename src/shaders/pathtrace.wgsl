// ---------------------------------------------------------------------------
// pathtrace.wgsl — one path per pixel per frame.
// Outputs albedo-demodulated radiance (so the denoiser only ever blurs
// illumination, never voxel texture), plus a G-buffer used for temporal
// reprojection and edge-aware filtering.
// ---------------------------------------------------------------------------

@group(0) @binding(3) var radianceOut : texture_storage_2d<rgba16float, write>;
@group(0) @binding(4) var albedoOut   : texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(5) var gbufOut     : texture_storage_2d<rgba32float, write>;

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  let dims = vec2<u32>(u32(u.params1.x), u32(u.params1.y));
  if (gid.x >= dims.x || gid.y >= dims.y) { return; }

  initRng(gid.xy, u.params0.x);

  // Sub-pixel jitter for anti-aliasing; temporal accumulation integrates it.
  let jitter = rand2() - 0.5;
  let uv = (vec2<f32>(gid.xy) + 0.5 + jitter) / vec2<f32>(dims);

  var ro = u.camPos.xyz;
  var rd = cameraRay(uv);

  var radiance = vec3<f32>(0.0);
  var throughput = vec3<f32>(1.0);

  var primT = -1.0;
  var primN = vec3<f32>(0.0);
  var primAlbedo = vec3<f32>(1.0);

  let nBounces = u.params0.y;

  for (var bounce = 0u; bounce <= nBounces; bounce++) {
    let h = trace(ro, rd, 1e4);

    if (h.t < 0.0) {
      // Sun disc only on the primary ray: secondary sun light is handled by
      // next-event estimation below, so including it here would double count.
      radiance += throughput * skyColor(rd, bounce == 0u);
      break;
    }

    let mat = unpackMaterial(h.mat);
    let pos = ro + rd * h.t;

    if (bounce == 0u) {
      primT = h.t;
      primN = h.n;
      primAlbedo = mat.albedo;
    }

    if (mat.emissive > 0.0) {
      radiance += throughput * mat.albedo * (mat.emissive * EMISSIVE_SCALE);
    }

    let surf = pos + h.n * max(1e-3, h.t * 1e-4);

    // Next-event estimation toward the sun cone.
    let l = sampleSunDir();
    let ndl = dot(h.n, l);
    if (ndl > 0.0 && l.y > 0.0) {
      if (!traceShadow(surf, l, 1e4)) {
        radiance += throughput * mat.albedo * INV_PI * ndl * u.sunRadiance.rgb;
      }
    }

    if (bounce == nBounces) { break; }

    // Cosine-weighted diffuse bounce: BRDF * cos / pdf collapses to albedo.
    rd = cosineHemisphere(h.n);
    ro = surf;
    throughput *= mat.albedo;

    // Russian roulette after the second bounce.
    if (bounce >= 2u) {
      let p = clamp(max(throughput.x, max(throughput.y, throughput.z)), 0.05, 0.95);
      if (rand() > p) { break; }
      throughput /= p;
    }
  }

  // Clamp fireflies so single hot samples don't poison the history buffer.
  radiance = min(radiance, vec3<f32>(48.0));

  let demod = radiance / max(primAlbedo, vec3<f32>(1e-3));

  textureStore(radianceOut, vec2<i32>(gid.xy), vec4<f32>(demod, 1.0));
  textureStore(albedoOut, vec2<i32>(gid.xy), vec4<f32>(sqrt(primAlbedo), 1.0));
  textureStore(gbufOut, vec2<i32>(gid.xy), vec4<f32>(primN, primT));
}
