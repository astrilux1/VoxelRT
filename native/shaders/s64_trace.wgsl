// Sparse voxel 64-tree traversal (docs/S64.md §3): hierarchical DDA over
// five uniform 4^3 levels, descending TLAS root (256^3 cells) -> chunk-group
// (64^3) -> chunk root node (16^3) -> L1 node (4^3) -> bitmask leaf (voxels).
// This is the "small-stack descent" variant from the §4 budget: an explicit
// per-level stack of depth 5 holds {mask, childBase, origin, DDA cell, DDA
// tMax}, so empty space is stepped at the largest proven-empty level (one
// 64-bit mask read resolves 4^3 cells, §1) and a pop simply restores the
// parent's DDA and advances it — the child grid tiles the parent cell
// exactly, so leaving the child *is* crossing the parent's next boundary.
// No position is ever re-derived from t after entry, which avoids the
// classic re-descent epsilon hazards of restart-style traversals.
//
// World clip, entry-normal convention, DDA tie-breaks and the maxT hit test
// are copied verbatim from brickmap_trace.wgsl: the --gate mode (invariant 1)
// requires identical hit results across backends.

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

  // Per-level stack, depth 5 (levels 0..4; cell size 256/64/16/4/1 voxels).
  var stMask : array<vec2<u32>, 5>;    // 64-bit occupancy mask of the node
  var stBase : array<u32, 5>;          // childBase (level 4: matBase)
  var stOrigin : array<vec3<i32>, 5>;  // node min corner, voxel coords
  var stCell : array<vec3<i32>, 5>;    // parent DDA state saved on descend
  var stTMax : array<vec3<f32>, 5>;

  var level : i32 = 0;
  var cs : i32 = 256;                  // cell size at current level
  var t = tEnter;
  stMask[0] = vec2<u32>(tlas[0], tlas[1]);
  stBase[0] = 0u;
  stOrigin[0] = vec3<i32>(0);

  // Enter level 0 (clamped, like brickmap's brick entry).
  var cell : vec3<i32>;
  var tMax : vec3<f32>;
  var tDelta : vec3<f32>;
  {
    let p = ro + rd * (t + 1e-4);
    cell = clamp(vec3<i32>(floor(p / f32(cs))), vec3<i32>(0), vec3<i32>(3));
    tDelta = abs(invRd) * f32(cs);
    tMax = ((vec3<f32>(cell) + stepGE) * f32(cs) - ro) * invRd;
  }

  for (var iter = 0; iter < 8192; iter++) {
    let bit = u32(cell.x + cell.y * 4 + cell.z * 16);
    if (mask_bit(stMask[level], bit)) {
      if (level == 4) {
        // Solid voxel: same maxT semantics as brickmap's inner voxel loop.
        if (t > maxT) { return hit; }
        hit.t = t * VOXEL_SIZE;
        hit.n = -amask * stepF;
        hit.mat = mats[stBase[4] + popcnt_below(stMask[4], bit)];
        return hit;
      }
      // Descend: save this level's DDA, resolve the child reference (§3).
      stCell[level] = cell;
      stTMax[level] = tMax;
      let corg = stOrigin[level] + cell * cs;
      if (level == 0) {
        // TLAS root cell -> chunk-group mask.
        stMask[1] = vec2<u32>(tlas[2u + bit * 2u], tlas[3u + bit * 2u]);
        stBase[1] = 0u;
      } else if (level == 1) {
        // Group cell -> chunk root node via the chunkRoots table; the bit
        // being set guarantees a valid reference.
        let cc = corg / 64;
        let n = nodes[tlas[130u + u32(cc.x + cc.y * 16 + cc.z * 256)]];
        stMask[2] = n.xy;
        stBase[2] = n.z;
      } else if (level == 2) {
        let n = nodes[stBase[2] + popcnt_below(stMask[2], bit)];
        stMask[3] = n.xy;
        stBase[3] = n.z;
      } else {
        let lf = leaves[stBase[3] + popcnt_below(stMask[3], bit)];
        stMask[4] = lf.xy;
        stBase[4] = lf.z;
      }
      level += 1;
      cs = cs / 4;
      stOrigin[level] = corg;
      let p = ro + rd * (t + 1e-4);
      cell = clamp(vec3<i32>(floor((p - vec3<f32>(corg)) / f32(cs))), vec3<i32>(0), vec3<i32>(3));
      tDelta = abs(invRd) * f32(cs);
      tMax = (vec3<f32>(corg) + (vec3<f32>(cell) + stepGE) * f32(cs) - ro) * invRd;
      continue;
    }
    // Advance the DDA at this level (tie-breaks identical to
    // brickmap_trace.wgsl). Leaving the node pops to the parent and takes
    // the parent's step instead: the child grid tiles the parent cell, so
    // the exit plane is exactly the parent's next tMax boundary.
    loop {
      if (tMax.x <= tMax.y && tMax.x <= tMax.z) {
        t = tMax.x; tMax.x += tDelta.x; cell.x += stepDir.x; amask = vec3<f32>(1.0, 0.0, 0.0);
      } else if (tMax.y <= tMax.z) {
        t = tMax.y; tMax.y += tDelta.y; cell.y += stepDir.y; amask = vec3<f32>(0.0, 1.0, 0.0);
      } else {
        t = tMax.z; tMax.z += tDelta.z; cell.z += stepDir.z; amask = vec3<f32>(0.0, 0.0, 1.0);
      }
      if (t > tExit) { return hit; }
      if (all(cell >= vec3<i32>(0)) && all(cell <= vec3<i32>(3))) { break; }
      if (level == 0) { return hit; }     // left the world grid
      level -= 1;
      cs = cs * 4;
      cell = stCell[level];
      tMax = stTMax[level];
      tDelta = abs(invRd) * f32(cs);
    }
  }
  return hit;
}
