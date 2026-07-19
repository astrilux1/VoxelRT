// Sparse voxel 64-tree traversal, variant 2: stackless re-descent
// (docs/S64.md §4 budget, second of the two registered traversal variants).
// Hierarchy, buffer layout and bit conventions are identical to
// s64_trace.wgsl (variant 1): five uniform 4^3 levels, TLAS root (256^3-cell
// granularity) -> chunk-group (64^3) -> chunk root (16^3) -> L1 (4^3) ->
// bitmask leaf (voxels); child index = childBase + popcnt(mask below bit).
//
// Unlike variant 1's explicit depth-5 stack, this traversal keeps DDA state
// for the CURRENT node only. The ray DDA-steps inside the deepest
// proven-empty node; when a step enters an occupied cell or leaves the node
// it RELOCATES: it computes the integer voxel coordinate just past the
// crossed boundary and re-descends from the TLAS root (<= 4 mask reads) to
// the deepest node containing that coordinate — either a solid voxel (hit)
// or the next empty node to DDA in.
//
// The classic restart hazard is relocating back into the cell that was just
// exited (t + epsilon, floored, can round back inside -> infinite loop;
// variant 1's comments explain it avoided this by never re-deriving position
// from t). Here the relocation coordinate is assembled per-axis from
// integers the DDA already knows, with no epsilon on t:
//   - stepped axis: the exact crossing-plane coordinate taken from the
//     integer cell index (the entered cell's low face when stepping +, its
//     high face - 1 when stepping -) — strictly outside the exited cell by
//     construction, so re-descent can never land back in it;
//   - other axes: floor(ro + rd*t) clamped into the current cell's integer
//     interval [base, base + cs - 1], which the DDA invariant (their tMax
//     has not been crossed) guarantees contains the ray at the crossing t;
//     the clamp only corrects f32 rounding at boundaries.
// Every tMax is recomputed from integer plane coordinates on relocation, so
// no epsilon or drift accumulates across restarts.
//
// World clip, entry-normal convention, DDA tie-breaks and the maxT hit test
// are copied verbatim from brickmap_trace.wgsl / s64_trace.wgsl: the --gate
// mode (invariant 1) requires identical hit results across backends.

const GRID_T : i32 = 1024;
const VOXEL_SIZE : f32 = 0.0625;      // 1/16 m, matches the browser renderer
const INV_VOXEL : f32 = 16.0;

// Buffers as produced by s64_build.wgsl (see its header for the layout).
@group(1) @binding(0) var<storage, read> tlas : array<u32>;
@group(1) @binding(1) var<storage, read> nodes : array<vec4<u32>>;
@group(1) @binding(2) var<storage, read> leaves : array<vec4<u32>>;
@group(1) @binding(3) var<storage, read> mats : array<u32>;

struct Hit {
  t   : f32,          // world meters, < 0 on miss
  n   : vec3<f32>,
  mat : u32,
};

fn safeInv(v : vec3<f32>) -> vec3<f32> {
  let s = vec3<f32>(
    select(-1.0, 1.0, v.x >= 0.0),
    select(-1.0, 1.0, v.y >= 0.0),
    select(-1.0, 1.0, v.z >= 0.0));
  return s / max(abs(v), vec3<f32>(1e-8));
}

fn mask_bit(m : vec2<u32>, b : u32) -> bool {
  if (b < 32u) { return (m.x & (1u << b)) != 0u; }
  return (m.y & (1u << (b - 32u))) != 0u;
}

// Number of set bits strictly below bit b of a 64-bit (lo,hi) mask.
fn popcnt_below(m : vec2<u32>, b : u32) -> u32 {
  if (b == 0u) { return 0u; }
  if (b < 32u) { return countOneBits(m.x & ((1u << b) - 1u)); }
  return countOneBits(m.x) + countOneBits(m.y & ((1u << (b - 32u)) - 1u));
}

fn trace(roWorld : vec3<f32>, rd : vec3<f32>, maxTWorld : f32) -> Hit {
  var hit : Hit;
  hit.t = -1.0;
  hit.mat = 0u;
  hit.n = -rd;

  let ro = roWorld * INV_VOXEL;
  let maxT = maxTWorld * INV_VOXEL;
  let invRd = safeInv(rd);
  let stepDir = vec3<i32>(sign(rd));
  let stepF = vec3<f32>(stepDir);
  let stepGE = step(vec3<f32>(0.0), rd);
  let stepPos = vec3<bool>(stepDir.x > 0, stepDir.y > 0, stepDir.z > 0);

  // World clip: identical to brickmap_trace.wgsl (identity invariant 1).
  let t0 = (vec3<f32>(0.0) - ro) * invRd;
  let t1 = (vec3<f32>(f32(GRID_T)) - ro) * invRd;
  let tmin3 = min(t0, t1);
  let tmax3 = max(t0, t1);
  let tEnter = max(max(tmin3.x, tmin3.y), max(tmin3.z, 0.0));
  let tExit = min(min(tmax3.x, tmax3.y), min(tmax3.z, maxT));
  if (tEnter > tExit) { return hit; }

  // Entry-face axis, same convention and tie-break as brickmap_trace.wgsl.
  var amask : vec3<f32>;
  if (tmin3.x >= tmin3.y && tmin3.x >= tmin3.z) { amask = vec3<f32>(1.0, 0.0, 0.0); }
  else if (tmin3.y >= tmin3.z)                  { amask = vec3<f32>(0.0, 1.0, 0.0); }
  else                                          { amask = vec3<f32>(0.0, 0.0, 1.0); }

  var t = tEnter;
  // Entry voxel: same +1e-4 entry nudge and clamp as variant 1's level-0
  // entry (safe here — there is no exited cell yet to fall back into).
  var vox = clamp(vec3<i32>(floor(ro + rd * (t + 1e-4))),
                  vec3<i32>(0), vec3<i32>(GRID_T - 1));

  // One outer iteration per restart (re-descent from the root).
  for (var iter = 0; iter < 8192; iter++) {
    // --- Re-descend from the TLAS root to the deepest node holding vox ----
    var level : i32 = 0;
    var sh : u32 = 8u;                 // cell size = 1 << sh: 256/64/16/4/1
    var origin = vec3<i32>(0);
    var mask = vec2<u32>(tlas[0], tlas[1]);
    var base : u32 = 0u;               // level 2: L1 node base; 3: leaf base;
                                       // 4: matBase — only read at 2..4
    var cell = vox >> vec3<u32>(sh);
    loop {
      let bit = u32(cell.x + cell.y * 4 + cell.z * 16);
      if (!mask_bit(mask, bit)) { break; }   // deepest empty level: DDA here
      if (level == 4) {
        // Solid voxel: same maxT semantics as brickmap's inner voxel loop.
        if (t > maxT) { return hit; }
        hit.t = t * VOXEL_SIZE;
        hit.n = -amask * stepF;
        hit.mat = mats[base + popcnt_below(mask, bit)];
        return hit;
      }
      let corg = origin + (cell << vec3<u32>(sh));
      if (level == 0) {
        // TLAS root cell -> chunk-group mask.
        mask = vec2<u32>(tlas[2u + bit * 2u], tlas[3u + bit * 2u]);
      } else if (level == 1) {
        // Group cell -> chunk root node via the chunkRoots table; the bit
        // being set guarantees a valid reference.
        let cc = corg >> vec3<u32>(6u);
        let n = nodes[tlas[130u + u32(cc.x + cc.y * 16 + cc.z * 256)]];
        mask = n.xy;
        base = n.z;
      } else if (level == 2) {
        let n = nodes[base + popcnt_below(mask, bit)];
        mask = n.xy;
        base = n.z;
      } else {
        let lf = leaves[base + popcnt_below(mask, bit)];
        mask = lf.xy;
        base = lf.z;
      }
      level += 1;
      sh -= 2u;
      origin = corg;
      cell = (vox - origin) >> vec3<u32>(sh);
    }

    // --- DDA inside the empty node (tie-breaks identical to variant 1) ----
    let cs = i32(1u << sh);
    let csf = f32(cs);
    let tDelta = abs(invRd) * csf;
    var tMax = (vec3<f32>(origin) + (vec3<f32>(cell) + stepGE) * csf - ro) * invRd;
    loop {
      if (tMax.x <= tMax.y && tMax.x <= tMax.z) {
        t = tMax.x; tMax.x += tDelta.x; cell.x += stepDir.x; amask = vec3<f32>(1.0, 0.0, 0.0);
      } else if (tMax.y <= tMax.z) {
        t = tMax.y; tMax.y += tDelta.y; cell.y += stepDir.y; amask = vec3<f32>(0.0, 1.0, 0.0);
      } else {
        t = tMax.z; tMax.z += tDelta.z; cell.z += stepDir.z; amask = vec3<f32>(0.0, 0.0, 1.0);
      }
      if (t > tExit) { return hit; }
      if (all(cell >= vec3<i32>(0)) && all(cell <= vec3<i32>(3)) &&
          !mask_bit(mask, u32(cell.x + cell.y * 4 + cell.z * 16))) {
        continue;                      // empty sibling: keep stepping here
      }
      // Entered an occupied cell, or left the node: relocate (header note).
      // cellBase is valid even when cell is out of [0,3] on the stepped axis
      // (it then names the neighbor region's base at this cell size).
      let cellBase = origin + (cell << vec3<u32>(sh));
      let clamped = clamp(vec3<i32>(floor(ro + rd * t)),
                          cellBase, cellBase + vec3<i32>(cs - 1));
      let crossing = select(cellBase + vec3<i32>(cs - 1), cellBase, stepPos);
      vox = select(clamped, crossing, amask > vec3<f32>(0.5));
      if (any(vox < vec3<i32>(0)) || any(vox >= vec3<i32>(GRID_T))) {
        return hit;                    // left the world grid
      }
      break;                           // outer loop re-descends at vox
    }
  }
  return hit;
}
