// ---------------------------------------------------------------------------
// restir.wgsl — shared reservoir data model for the unified ReSTIR pipeline.
//
// One reservoir per pixel holds a single light-transport sample in a *unified*
// domain (Lin 2026 §6.1): either a surface point x2 carrying the outgoing
// radiance Lo(x2 -> receiver), or a distant direction carrying sky/sun
// radiance. Every voxel surface is Lambertian, so Lo is exactly
// direction-independent and the reconnection shift needs no random replay:
// point samples live in *area measure*, which makes the shift Jacobian
// identically 1 — each receiving pixel simply re-evaluates its own geometry
// term. This is the voxel specialization of ReSTIR PT's hybrid shift: the
// reconnection vertex is always x2 and always valid.
//
// Packed layout (32 bytes/pixel, cf. Lin 2026 §6.2.1 which compresses 88->64):
//   posW.xyz  world position of x2 (SK_POINT) or unit direction (SK_DIR)
//   posW.w    W — unbiased contribution weight of the sample
//   data.x    radiance RG as 2x f16
//   data.y    radiance B as f16 | kind << 16 | confidence (u8) << 18
//   data.z    octahedral-encoded normal at x2 (2x snorm16); unused for SK_DIR
//   data.w    sample seed: identifies the initial candidate this sample was
//             resampled from — shared seeds mean duplicated samples (§5)
// ---------------------------------------------------------------------------

struct Reservoir {
  posW : vec4<f32>,
  data : vec4<u32>,
};

const SK_NONE  : u32 = 0u;   // invalid / sky pixel
const SK_POINT : u32 = 1u;   // surface point sample (area measure)
const SK_DIR   : u32 = 2u;   // distant direction sample (sky or sun)

struct Sample {
  kind : u32,
  pos  : vec3<f32>,   // point (m) or direction
  n    : vec3<f32>,   // surface normal at the point (SK_POINT only)
  rad  : vec3<f32>,   // Lo for points, L for directions
  W    : f32,
  c    : f32,         // confidence weight
  seed : u32,
};

fn octEncode(nIn : vec3<f32>) -> vec2<f32> {
  let n = nIn / (abs(nIn.x) + abs(nIn.y) + abs(nIn.z));
  var v = n.xy;
  if (n.z < 0.0) {
    v = (1.0 - abs(n.yx)) * vec2<f32>(select(-1.0, 1.0, n.x >= 0.0), select(-1.0, 1.0, n.y >= 0.0));
  }
  return v;
}

fn octDecode(e : vec2<f32>) -> vec3<f32> {
  var n = vec3<f32>(e, 1.0 - abs(e.x) - abs(e.y));
  if (n.z < 0.0) {
    let xy = (1.0 - abs(n.yx)) * vec2<f32>(select(-1.0, 1.0, n.x >= 0.0), select(-1.0, 1.0, n.y >= 0.0));
    n = vec3<f32>(xy, n.z);
  }
  return normalize(n);
}

fn packReservoir(s : Sample) -> Reservoir {
  var r : Reservoir;
  r.posW = vec4<f32>(s.pos, s.W);
  let cQ = min(u32(s.c + 0.5), 255u);
  r.data = vec4<u32>(
    pack2x16float(s.rad.rg),
    (pack2x16float(vec2<f32>(s.rad.b, 0.0)) & 0xffffu) | (s.kind << 16u) | (cQ << 18u),
    pack2x16snorm(octEncode(select(s.n, vec3<f32>(0.0, 1.0, 0.0), s.kind != SK_POINT))),
    s.seed);
  return r;
}

fn unpackReservoir(r : Reservoir) -> Sample {
  var s : Sample;
  s.pos = r.posW.xyz;
  s.W = r.posW.w;
  let rg = unpack2x16float(r.data.x);
  s.rad = vec3<f32>(rg, unpack2x16float(r.data.y & 0xffffu).x);
  s.kind = (r.data.y >> 16u) & 3u;
  s.c = f32((r.data.y >> 18u) & 0xffu);
  s.n = octDecode(unpack2x16snorm(r.data.z));
  s.seed = r.data.w;
  return s;
}

fn emptyReservoir() -> Reservoir {
  return Reservoir(vec4<f32>(0.0), vec4<u32>(0u));
}

fn lum(c : vec3<f32>) -> f32 {
  return dot(c, vec3<f32>(0.2126, 0.7152, 0.0722));
}

// Demodulated integrand of a sample as seen from a receiver surface point
// (x1, n1), *excluding* visibility: f/albedo * cos1 * L * G. G is the
// area-measure geometry term for point samples and 1 for directions —
// exactly the reconnection-shift re-evaluation, so no Jacobian is needed.
fn evalF(s : Sample, x1 : vec3<f32>, n1 : vec3<f32>) -> vec3<f32> {
  if (s.kind == SK_POINT) {
    let d = s.pos - x1;
    let r2 = max(dot(d, d), 1e-4);
    let dir = d * inverseSqrt(r2);
    let cos1 = dot(n1, dir);
    let cos2 = dot(s.n, -dir);
    if (cos1 <= 0.0 || cos2 <= 0.0) { return vec3<f32>(0.0); }
    return s.rad * (INV_PI * cos1 * cos2 / r2);
  }
  if (s.kind == SK_DIR) {
    let cos1 = dot(n1, s.pos);
    if (cos1 <= 0.0) { return vec3<f32>(0.0); }
    return s.rad * (INV_PI * cos1);
  }
  return vec3<f32>(0.0);
}

// Scalar resampling target p-hat = luminance of the (visibility-free) integrand.
fn evalTarget(s : Sample, x1 : vec3<f32>, n1 : vec3<f32>) -> f32 {
  return lum(evalF(s, x1, n1));
}

// Footprint-based reconnection criterion (Lin 2026 §4, Eq. 5 adapted).
// For a diffuse world the inverse ray footprint of the reconnection is
// pi*r^2/(cos1*cos2); requiring it to exceed a multiple of the primary-ray
// footprint t^2/(cosView/(4pi)) forbids reconnections whose area density
// would change sharply between neighboring pixels (short reconnections near
// corners), independent of scene scale.
fn footprintOK(s : Sample, x1 : vec3<f32>, n1 : vec3<f32>, tPrim : f32, cosView : f32) -> bool {
  if (s.kind != SK_POINT) { return true; }
  let d = s.pos - x1;
  let r2 = dot(d, d);
  let dir = d * inverseSqrt(max(r2, 1e-8));
  let cos1 = max(dot(n1, dir), 0.0);
  let cos2 = max(dot(s.n, -dir), 0.0);
  return r2 >= u.params3.w * tPrim * tPrim * cos1 * cos2 / max(cosView, 0.05);
}
