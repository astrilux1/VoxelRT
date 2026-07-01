// ---------------------------------------------------------------------------
// present.wgsl — fullscreen composite: remodulate albedo, expose, ACES
// tonemap, gamma-encode to the canvas.
// Compiled standalone (not concatenated with common.wgsl), so it carries its
// own copy of the Uniforms layout.
// ---------------------------------------------------------------------------

struct Uniforms {
  invViewProj  : mat4x4<f32>,
  prevViewProj : mat4x4<f32>,
  camPos       : vec4<f32>,
  prevCamPos   : vec4<f32>,
  sunDir       : vec4<f32>,
  sunRadiance  : vec4<f32>,
  params0      : vec4<u32>,
  params1      : vec4<f32>,   // z = exposure
};
@group(0) @binding(0) var<uniform> u : Uniforms;

@group(0) @binding(1) var finalTex  : texture_2d<f32>;
@group(0) @binding(2) var albedoTex : texture_2d<f32>;
@group(0) @binding(3) var linearSamp : sampler;

struct VSOut {
  @builtin(position) pos : vec4<f32>,
  @location(0) uv : vec2<f32>,
};

@vertex
fn vsMain(@builtin(vertex_index) vi : u32) -> VSOut {
  // Single fullscreen triangle.
  var out : VSOut;
  let x = f32(i32(vi & 1u) * 4 - 1);
  let y = f32(i32(vi >> 1u) * 4 - 1);
  out.pos = vec4<f32>(x, y, 0.0, 1.0);
  out.uv = vec2<f32>(x, -y) * 0.5 + 0.5;
  return out;
}

fn acesTonemap(x : vec3<f32>) -> vec3<f32> {
  let a = 2.51; let b = 0.03; let c = 2.43; let d = 0.59; let e = 0.14;
  return clamp((x * (a * x + b)) / (x * (c * x + d) + e), vec3<f32>(0.0), vec3<f32>(1.0));
}

@fragment
fn fsMain(in : VSOut) -> @location(0) vec4<f32> {
  let illum = textureSampleLevel(finalTex, linearSamp, in.uv, 0.0).rgb;
  let albSqrt = textureSampleLevel(albedoTex, linearSamp, in.uv, 0.0).rgb;
  let albedo = albSqrt * albSqrt;   // stored as sqrt for 8-bit precision

  var c = illum * albedo * u.params1.z;
  c = acesTonemap(c);
  c = pow(c, vec3<f32>(1.0 / 2.2));
  return vec4<f32>(c, 1.0);
}
