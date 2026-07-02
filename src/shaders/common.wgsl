// ---------------------------------------------------------------------------
// common.wgsl — shared declarations for the VoxelRT path tracer.
// Constants GRID / BRICK / VOXEL_SIZE are injected by the host (main.js)
// ahead of this file.
// ---------------------------------------------------------------------------

const PI : f32 = 3.14159265358979;
const INV_PI : f32 = 0.31830988618;
const BGRID : i32 = GRID / BRICK;
const INV_VOXEL : f32 = 1.0 / VOXEL_SIZE;
const EMISSIVE_SCALE : f32 = 24.0;

struct Uniforms {
  invViewProj  : mat4x4<f32>,
  prevViewProj : mat4x4<f32>,
  camPos       : vec4<f32>,   // xyz = position (m), w = time (s)
  prevCamPos   : vec4<f32>,   // xyz = previous frame position
  sunDir       : vec4<f32>,   // xyz = direction TO the sun, w = cos(angular radius)
  sunRadiance  : vec4<f32>,   // rgb = sun irradiance E, w = sky intensity
  params0      : vec4<u32>,   // x = frame index, y = indirect bounce count, z = flags, w = unused
  params1      : vec4<f32>,   // x = render width, y = render height, z = exposure, w = unused
  params2      : vec4<u32>,   // x = restir flags, y = light face count, z = spatial taps, w = unused
  params3      : vec4<f32>,   // x = cCap, y = cCapMin, z = dup alpha, w = footprint c
  params4      : vec4<f32>,   // x = spatial sigma/radius (px), y = max accum history,
                              // z = contribution clamp, w = unused
};

@group(0) @binding(0) var<uniform> u : Uniforms;

// params2.x flag bits — which parts of the ReSTIR pipeline are active.
const RF_RESTIR    : u32 = 1u;     // reservoir pipeline on (off = plain 1-spp PT)
const RF_TEMPORAL  : u32 = 2u;     // temporal reservoir reuse
const RF_SPATIAL   : u32 = 4u;     // spatial reservoir reuse
const RF_PAIRED    : u32 = 8u;     // paired Gaussian reuse (Lin 2026 §3); off = random disk
const RF_DUPMAP    : u32 = 16u;    // duplication-map adaptive cCap (Lin 2026 §5)
const RF_FOOTPRINT : u32 = 32u;    // footprint reconnection criterion (Lin 2026 §4)
const RF_VECTOR    : u32 = 64u;    // vector-valued shading weights (Lin 2026 §6.3)
const RF_UNIFIED   : u32 = 128u;   // unified DI+GI reservoir with light-list NEE (Lin 2026 §6.1)
const RF_PLANE     : u32 = 256u;   // ours: exact voxel-plane neighbor validation
const RF_RESCUE    : u32 = 512u;   // ours: disocclusion history rescue (3x3 search)
const RF_FULLV     : u32 = 1024u;  // ours: revalidate canonical visibility when shading
const RF_CLAMP     : u32 = 2048u;  // ours: reservoir contribution clamp

fn rflag(bit : u32) -> bool { return (u.params2.x & bit) != 0u; }

// ---------------------------------------------------------------------------
// RNG — PCG hash, one stream per pixel per frame.
// ---------------------------------------------------------------------------

var<private> rngState : u32;

fn pcgHash(vIn : u32) -> u32 {
  var v = vIn * 747796405u + 2891336453u;
  let w = ((v >> ((v >> 28u) + 4u)) ^ v) * 277803737u;
  return (w >> 22u) ^ w;
}

fn initRng(pix : vec2<u32>, frame : u32) {
  rngState = pcgHash(pix.x + pcgHash(pix.y + pcgHash(frame)));
}

fn rand() -> f32 {
  rngState = pcgHash(rngState);
  return f32(rngState) * (1.0 / 4294967296.0);
}

fn rand2() -> vec2<f32> { return vec2<f32>(rand(), rand()); }

// ---------------------------------------------------------------------------
// Camera rays.
// ---------------------------------------------------------------------------

fn cameraRay(uv : vec2<f32>) -> vec3<f32> {
  // uv in [0,1], origin top-left. NDC y is flipped.
  let ndc = vec2<f32>(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
  let far = u.invViewProj * vec4<f32>(ndc, 1.0, 1.0);
  return normalize(far.xyz / far.w - u.camPos.xyz);
}

// ---------------------------------------------------------------------------
// Sky + sun.
// ---------------------------------------------------------------------------

fn skyColor(rd : vec3<f32>, withSunDisc : bool) -> vec3<f32> {
  let up = clamp(rd.y, -1.0, 1.0);
  var sky = mix(vec3<f32>(0.55, 0.68, 0.85), vec3<f32>(0.15, 0.32, 0.62), pow(max(up, 0.0), 0.55));
  if (up < 0.0) {
    sky = mix(sky, vec3<f32>(0.28, 0.26, 0.24), min(1.0, -up * 4.0));
  }
  let sunAmt = max(dot(rd, u.sunDir.xyz), 0.0);
  sky += vec3<f32>(1.0, 0.75, 0.5) * pow(sunAmt, 8.0) * 0.2;   // halo
  if (withSunDisc && sunAmt > u.sunDir.w) {
    // Disc radiance ~ irradiance / solid angle.
    let solidAngle = 2.0 * PI * (1.0 - u.sunDir.w);
    sky += u.sunRadiance.rgb / max(solidAngle, 1e-4);
  }
  return sky * u.sunRadiance.w;
}

// Sample a direction inside the sun cone (uniform over solid angle).
fn sampleSunDir() -> vec3<f32> {
  let r = rand2();
  let cosT = mix(1.0, u.sunDir.w, r.x);
  let sinT = sqrt(max(0.0, 1.0 - cosT * cosT));
  let phi = 2.0 * PI * r.y;
  let w = u.sunDir.xyz;
  let a = select(vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(0.0, 1.0, 0.0), abs(w.x) > 0.9);
  let tang = normalize(cross(a, w));
  let bitan = cross(w, tang);
  return normalize(tang * (cos(phi) * sinT) + bitan * (sin(phi) * sinT) + w * cosT);
}

fn cosineHemisphere(n : vec3<f32>) -> vec3<f32> {
  let r = rand2();
  let phi = 2.0 * PI * r.x;
  let sr = sqrt(r.y);
  let a = select(vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(0.0, 1.0, 0.0), abs(n.x) > 0.9);
  let tang = normalize(cross(a, n));
  let bitan = cross(n, tang);
  let d = tang * (cos(phi) * sr) + bitan * (sin(phi) * sr) + n * sqrt(max(0.0, 1.0 - r.y));
  return normalize(d);
}
