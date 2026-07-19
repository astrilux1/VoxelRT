// Backend-agnostic ray-bench kernels. The host concatenates:
//   [backend trace file] + this file
// so `trace(ro, rd, maxT) -> Hit` is provided by whichever backend runs.
//
// Three entry points = three timestamp-isolated ray classes:
//   primary : camera rays; writes hit t/n/mat to the hit buffer + shades
//             a simple sun+emissive image (identity-gate artifact)
//   bounce  : one cosine-hemisphere ray from each primary hit (incoherent)
//   shadow  : one ray toward a fixed key light from each primary hit
//
// RNG is seeded by (pixel, frame) only — never by backend — so identical
// hits imply byte-identical images across backends.

struct BenchParams {
  width : u32,
  height : u32,
  frame : u32,
  _pad : u32,
  eye : vec4<f32>,
  // Camera basis, row-ish: right.xyz = half-width vector, up.xyz, fwd.xyz
  right : vec4<f32>,
  up : vec4<f32>,
  fwd : vec4<f32>,
  sunDir : vec4<f32>,
};

@group(0) @binding(0) var<uniform> bp : BenchParams;
@group(0) @binding(1) var<storage, read_write> imageOut : array<u32>;   // rgba8 packed
@group(0) @binding(2) var<storage, read_write> hitT : array<f32>;
@group(0) @binding(3) var<storage, read_write> hitNMat : array<vec4<u32>>; // n(oct16 in x), mat in y, pos in zw? keep simple: n packed, mat
@group(0) @binding(4) var<storage, read_write> stats : array<atomic<u32>>; // [0]=primary hits, [1]=bounce hits, [2]=shadow occluded

fn pcg(v : u32) -> u32 {
  let s = v * 747796405u + 2891336453u;
  let w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
  return (w >> 22u) ^ w;
}

fn rand2(pix : u32, frame : u32, salt : u32) -> vec2<f32> {
  let a = pcg(pix ^ (frame * 0x9e3779b9u) ^ (salt * 0x85ebca6bu));
  let b = pcg(a);
  return vec2<f32>(f32(a & 0xffffffu), f32(b & 0xffffffu)) / 16777216.0;
}

fn pack_rgba8(c : vec3<f32>) -> u32 {
  let q = vec3<u32>(clamp(c, vec3<f32>(0.0), vec3<f32>(1.0)) * 255.0 + 0.5);
  return q.x | (q.y << 8u) | (q.z << 16u) | (255u << 24u);
}

fn pack_normal(n : vec3<f32>) -> u32 {
  // Face normals only (6 values): 3 bits axis+sign
  var code = 0u;
  if (abs(n.x) > 0.5) { code = select(1u, 0u, n.x > 0.0); }
  else if (abs(n.y) > 0.5) { code = select(3u, 2u, n.y > 0.0); }
  else { code = select(5u, 4u, n.z > 0.0); }
  return code;
}

fn unpack_normal(code : u32) -> vec3<f32> {
  switch code {
    case 0u: { return vec3<f32>(1.0, 0.0, 0.0); }
    case 1u: { return vec3<f32>(-1.0, 0.0, 0.0); }
    case 2u: { return vec3<f32>(0.0, 1.0, 0.0); }
    case 3u: { return vec3<f32>(0.0, -1.0, 0.0); }
    case 4u: { return vec3<f32>(0.0, 0.0, 1.0); }
    default: { return vec3<f32>(0.0, 0.0, -1.0); }
  }
}

fn unpackAlbedo(m : u32) -> vec3<f32> {
  var a = vec3<f32>(f32(m & 0xffu), f32((m >> 8u) & 0xffu), f32((m >> 16u) & 0xffu)) / 255.0;
  return a * a;
}

fn camera_ray(pix : vec2<u32>) -> vec3<f32> {
  let uv = (vec2<f32>(pix) + 0.5) / vec2<f32>(f32(bp.width), f32(bp.height)) * 2.0 - 1.0;
  return normalize(bp.fwd.xyz + bp.right.xyz * uv.x - bp.up.xyz * uv.y);
}

@compute @workgroup_size(8, 8)
fn primary(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (gid.x >= bp.width || gid.y >= bp.height) { return; }
  let idx = gid.x + gid.y * bp.width;
  let rd = camera_ray(gid.xy);
  let h = trace(bp.eye.xyz, rd, 1e4);

  hitT[idx] = h.t;
  var packed = vec4<u32>(0u);
  var color = vec3<f32>(0.35, 0.55, 0.85);            // sky
  if (h.t >= 0.0) {
    atomicAdd(&stats[0], 1u);
    packed.x = pack_normal(h.n);
    packed.y = h.mat;
    let alb = unpackAlbedo(h.mat);
    let em = f32((h.mat >> 24u) & 0xffu) / 255.0;
    let ndl = max(dot(h.n, bp.sunDir.xyz), 0.0);
    color = alb * (0.15 + 0.85 * ndl) + alb * em * 4.0;
  }
  hitNMat[idx] = packed;
  imageOut[idx] = pack_rgba8(color);
}

fn cosine_hemisphere(n : vec3<f32>, r : vec2<f32>) -> vec3<f32> {
  let a = 6.2831853 * r.x;
  let s = sqrt(r.y);
  // Build a tangent frame around n (n is axis-aligned here, still generic).
  var t = vec3<f32>(1.0, 0.0, 0.0);
  if (abs(n.x) > 0.9) { t = vec3<f32>(0.0, 1.0, 0.0); }
  let b1 = normalize(cross(n, t));
  let b2 = cross(n, b1);
  return normalize(b1 * (cos(a) * s) + b2 * (sin(a) * s) + n * sqrt(1.0 - r.y));
}

@compute @workgroup_size(8, 8)
fn bounce(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (gid.x >= bp.width || gid.y >= bp.height) { return; }
  let idx = gid.x + gid.y * bp.width;
  let t = hitT[idx];
  if (t < 0.0) { return; }
  let rdPrim = camera_ray(gid.xy);
  let n = unpack_normal(hitNMat[idx].x);
  let pos = bp.eye.xyz + rdPrim * t + n * 1e-3;
  let rd = cosine_hemisphere(n, rand2(idx, bp.frame, 17u));
  let h = trace(pos, rd, 1e4);
  if (h.t >= 0.0) { atomicAdd(&stats[1], 1u); }
}

@compute @workgroup_size(8, 8)
fn shadow(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (gid.x >= bp.width || gid.y >= bp.height) { return; }
  let idx = gid.x + gid.y * bp.width;
  let t = hitT[idx];
  if (t < 0.0) { return; }
  let rdPrim = camera_ray(gid.xy);
  let n = unpack_normal(hitNMat[idx].x);
  let pos = bp.eye.xyz + rdPrim * t + n * 1e-3;
  let h = trace(pos, bp.sunDir.xyz, 1e4);
  if (h.t >= 0.0) { atomicAdd(&stats[2], 1u); }
}
