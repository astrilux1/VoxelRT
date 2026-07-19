// Sparse voxel 64-tree traversal, optimization rung over variant 2
// (docs/S64OPT.md). The promoted control kernel s64_trace_stackless.wgsl is
// untouched and stays selectable via --traversal stackless; this file is
// selected via --traversal stackless-opt. Hierarchy, buffer layout, bit
// conventions, restart-hazard handling and DDA tie-breaks are identical to
// the control (see its header for the relocation-coordinate derivation);
// every deviation sits behind its own module-scope const so the levers can
// be measured in isolation (Table-1 discipline, docs/S64OPT.md §4).
//
// Lever flags (the host string-replaces these declarations — each must stay
// on one line, exactly as written — via `--opt-levers <comma list>` with
// tokens memo,mirror,skip,anyhit, or "none" to disable all; absent flag
// means all levers on; see parse_args/trace_source in src/main.rs):
//
//   ENABLE_MEMO   ("memo") — ancestor memoization: retain the current chunk
//     root (64^3 voxel region) and L1 node (16^3 region) — mask, childBase,
//     origin — in registers; on relocation, test containment innermost-first
//     (L1 then chunk) and re-enter the descent at the deepest containing
//     level instead of the TLAS root; fall back to root descent on chunk
//     exit. Bitwise-exact vs the control: the memoized values equal what a
//     root re-descent would read (the structure is static), so only the
//     number of mask/node reads changes, never a traversal decision.
//
//   ENABLE_MIRROR ("mirror") — ray-octant mirroring: reflect the ray into
//     the all-non-negative direction octant (x -> GRID - x on each negative
//     axis) and run the whole traversal in mirrored space; mask bits are
//     indexed with un-mirrored cell coordinates (bit = x + y*4 + z*16 with
//     cell -> 3 - cell on mirrored axes; chunkRoots with chunk -> 15 -
//     chunk), and the hit normal is un-mirrored on output. NOT bitwise-exact
//     vs the control: the mirror transform (GRID - ro) rounds, so t can
//     differ by ULPs and knife-edge rays may resolve to a neighboring
//     face/cell — the class identity gate v2's divergence budget covers.
//
//   ENABLE_SKIP   ("skip") — bitmask skip-coalescing: when the 4-bit row of
//     the current node's 64-bit mask along the ray's dominant axis is empty
//     AND all remaining row crossings precede any lateral crossing (checked
//     with the control's exact per-axis tie-breaks), consume the rest of the
//     row and the node exit in ONE jump instead of per-cell steps. tMax/t
//     are advanced by the same serial additions the per-cell loop would
//     perform, so the committed state is bitwise-identical to the control.
//
//   ENABLE_ANYHIT ("anyhit") — shadow any-hit: on a solid-voxel hit, skip
//     the material fetch and normal bookkeeping and report occupancy only
//     (t is still exact, so the `hit.t >= 0.0` occlusion boolean and the
//     occlusion counts are unchanged; mat is a dummy 1u, n stays -rd).
//     bench.wgsl's group-0 interface and entry points are frozen and
//     trace() cannot know its caller, so the specialization is per MODULE:
//     the host compiles a second module with this const flipped to true and
//     builds only the shadow/shadow_compact pipelines from it (primary,
//     bounce and the gate always compile with it false).
//
// With all four flags false this kernel is operation-for-operation the
// control kernel: same arithmetic, same tie-breaks, same entry/exit
// conventions. The CPU cross-check (src/verify.rs tests) traces a
// deterministic >=10k-ray bundle through 1:1 Rust ports of this logic and
// asserts bitwise equality with the control port for memo/skip/anyhit and
// tolerance+budget equality for mirror, against an f64 voxel-march oracle.

const GRID_T : i32 = 1024;
const VOXEL_SIZE : f32 = 0.0625;      // 1/16 m, matches the browser renderer
const INV_VOXEL : f32 = 16.0;

const ENABLE_MEMO : bool = true;
const ENABLE_MIRROR : bool = true;
const ENABLE_SKIP : bool = true;
const ENABLE_ANYHIT : bool = false;

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

// Un-mirrored 4^3 bit index of a (possibly mirrored-space) cell: within any
// 4^3 tile, mirroring an axis reverses cell order along it, so the stored
// (un-mirrored) bit uses 3 - cell on mirrored axes. With mir = false this is
// exactly the control's bit = x + y*4 + z*16.
fn cell_bit(cell : vec3<i32>, mir : vec3<bool>) -> u32 {
  let cu = select(cell, vec3<i32>(3) - cell, mir);
  return u32(cu.x + cu.y * 4 + cu.z * 16);
}

// Skip-coalescing row test: true iff all 4 cells along `axis` at the
// (un-mirrored) lateral coordinates of cu are empty in the 64-bit mask.
// axis 0: one nibble; axis 1: 0x1111 within one 16-bit z-slice; axis 2:
// bit pairs (b, b+16) in each word. The `axis` component of cu is unused.
fn row_empty(m : vec2<u32>, axis : i32, cu : vec3<i32>) -> bool {
  if (axis == 0) {
    let s = u32(cu.y * 4 + cu.z * 16);
    if (s < 32u) { return ((m.x >> s) & 0xfu) == 0u; }
    return ((m.y >> (s - 32u)) & 0xfu) == 0u;
  }
  if (axis == 1) {
    let s = u32(cu.x + (cu.z & 1) * 16);
    let w = select(m.x, m.y, cu.z >= 2);
    return ((w >> s) & 0x1111u) == 0u;
  }
  let s = u32(cu.x + cu.y * 4);
  return ((m.x >> s) & 0x10001u) == 0u && ((m.y >> s) & 0x10001u) == 0u;
}

// Would the control's 3-way step tie-break select `axis` at crossing time
// tCand, given the other axes' pending tMax values? (x wins ties over y
// over z, exactly as in the control's if/else chain.)
fn axis_selected(axis : i32, tCand : f32, tMax : vec3<f32>) -> bool {
  if (axis == 0) { return tCand <= tMax.y && tCand <= tMax.z; }
  if (axis == 1) { return tCand < tMax.x && tCand <= tMax.z; }
  return tCand < tMax.x && tCand < tMax.y;
}

fn trace(roWorld : vec3<f32>, rd : vec3<f32>, maxTWorld : f32) -> Hit {
  var hit : Hit;
  hit.t = -1.0;
  hit.mat = 0u;
  hit.n = -rd;

  let ro0 = roWorld * INV_VOXEL;
  let maxT = maxTWorld * INV_VOXEL;

  // --- Lever 2: octant mirroring. With ENABLE_MIRROR off, mir is all-false,
  // every select below is the identity and the kernel runs the control's
  // exact arithmetic on (ro, rd). With it on, negative-direction axes are
  // reflected (x -> GRID - x) so the transformed direction is non-negative
  // on every axis; the traversal below is the unchanged control code
  // operating on the transformed ray, plus un-mirroring at mask indexing
  // (cell_bit / chunkRoots) and at the normal output.
  var mir = vec3<bool>(false);
  if (ENABLE_MIRROR) { mir = rd < vec3<f32>(0.0); }
  let ro = select(ro0, vec3<f32>(f32(GRID_T)) - ro0, mir);
  let rdT = select(rd, -rd, mir);

  let invRd = safeInv(rdT);
  let stepDir = vec3<i32>(sign(rdT));
  let stepF = vec3<f32>(stepDir);
  let stepGE = step(vec3<f32>(0.0), rdT);
  let stepPos = vec3<bool>(stepDir.x > 0, stepDir.y > 0, stepDir.z > 0);
  // Original-sign step for the un-mirrored hit normal (mirrored axes stepped
  // + in traversal space but - in world space).
  let stepOut = select(stepF, -stepF, mir);

  // World clip: identical to the control (identity invariant 1).
  let t0 = (vec3<f32>(0.0) - ro) * invRd;
  let t1 = (vec3<f32>(f32(GRID_T)) - ro) * invRd;
  let tmin3 = min(t0, t1);
  let tmax3 = max(t0, t1);
  let tEnter = max(max(tmin3.x, tmin3.y), max(tmin3.z, 0.0));
  let tExit = min(min(tmax3.x, tmax3.y), min(tmax3.z, maxT));
  if (tEnter > tExit) { return hit; }

  // Entry-face axis, same convention and tie-break as the control.
  var amask : vec3<f32>;
  if (tmin3.x >= tmin3.y && tmin3.x >= tmin3.z) { amask = vec3<f32>(1.0, 0.0, 0.0); }
  else if (tmin3.y >= tmin3.z)                  { amask = vec3<f32>(0.0, 1.0, 0.0); }
  else                                          { amask = vec3<f32>(0.0, 0.0, 1.0); }

  var t = tEnter;
  // Entry voxel: same +1e-4 entry nudge and clamp as the control.
  var vox = clamp(vec3<i32>(floor(ro + rdT * (t + 1e-4))),
                  vec3<i32>(0), vec3<i32>(GRID_T - 1));

  // --- Lever 3 setup: dominant axis (largest |direction| component; x wins
  // ties over y over z). Fixed per ray.
  var dom : i32 = 0;
  if (ENABLE_SKIP) {
    let ard = abs(rd);
    if (ard.y > ard.x && ard.y >= ard.z) { dom = 1; }
    else if (ard.z > ard.x && ard.z > ard.y) { dom = 2; }
  }

  // --- Lever 1 state: memoized ancestors of the current position, in
  // traversal (possibly mirrored) space. Invariants: when haveL1 is set, the
  // L1 region is nested inside the memoized chunk region; the memo always
  // describes nodes on the descent path of the position that recorded it,
  // and the structure is static, so re-entering at a memoized level yields
  // exactly the state a full root re-descent would reach.
  var haveChunk = false;
  var haveL1 = false;
  var chunkOrg = vec3<i32>(0);
  var chunkMask = vec2<u32>(0u);
  var chunkBase = 0u;
  var l1Org = vec3<i32>(0);
  var l1Mask = vec2<u32>(0u);
  var l1Base = 0u;

  // One outer iteration per restart (re-descent, from the root or a memo).
  for (var iter = 0; iter < 8192; iter++) {
    // --- Re-descend to the deepest node holding vox, entering at the
    // deepest memoized ancestor that contains it (ENABLE_MEMO) or at the
    // TLAS root (control behavior).
    var level : i32 = 0;
    var sh : u32 = 8u;                 // cell size = 1 << sh: 256/64/16/4/1
    var origin = vec3<i32>(0);
    var mask = vec2<u32>(tlas[0], tlas[1]);
    var base : u32 = 0u;               // level 2: L1 node base; 3: leaf base;
                                       // 4: matBase — only read at 2..4
    if (ENABLE_MEMO) {
      if (haveL1 && all(((vox >> vec3<u32>(4u)) << vec3<u32>(4u)) == l1Org)) {
        level = 3; sh = 2u; origin = l1Org; mask = l1Mask; base = l1Base;
      } else if (haveChunk && all(((vox >> vec3<u32>(6u)) << vec3<u32>(6u)) == chunkOrg)) {
        level = 2; sh = 4u; origin = chunkOrg; mask = chunkMask; base = chunkBase;
        haveL1 = false;                // stale: belongs to the previous path
      } else {
        haveChunk = false;             // chunk exit: full root descent
        haveL1 = false;
      }
    }
    var cell = (vox - origin) >> vec3<u32>(sh);
    loop {
      let bit = cell_bit(cell, mir);
      if (!mask_bit(mask, bit)) { break; }   // deepest empty level: DDA here
      if (level == 4) {
        // Solid voxel: same maxT semantics as the control.
        if (t > maxT) { return hit; }
        hit.t = t * VOXEL_SIZE;
        if (ENABLE_ANYHIT) {
          hit.mat = 1u;                // occupancy only; n stays -rd
        } else {
          hit.n = -amask * stepOut;
          hit.mat = mats[base + popcnt_below(mask, bit)];
        }
        return hit;
      }
      let corg = origin + (cell << vec3<u32>(sh));
      if (level == 0) {
        // TLAS root cell -> chunk-group mask (un-mirrored bit index).
        mask = vec2<u32>(tlas[2u + bit * 2u], tlas[3u + bit * 2u]);
      } else if (level == 1) {
        // Group cell -> chunk root node via the chunkRoots table; the bit
        // being set guarantees a valid reference. The table is indexed by
        // un-mirrored global chunk coordinates.
        let ccm = corg >> vec3<u32>(6u);
        let cc = select(ccm, vec3<i32>(15) - ccm, mir);
        let n = nodes[tlas[130u + u32(cc.x + cc.y * 16 + cc.z * 256)]];
        mask = n.xy;
        base = n.z;
        if (ENABLE_MEMO) {
          haveChunk = true; haveL1 = false;
          chunkOrg = corg; chunkMask = mask; chunkBase = base;
        }
      } else if (level == 2) {
        let n = nodes[base + popcnt_below(mask, bit)];
        mask = n.xy;
        base = n.z;
        if (ENABLE_MEMO) {
          haveL1 = true;
          l1Org = corg; l1Mask = mask; l1Base = base;
        }
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

    // --- DDA inside the empty node (tie-breaks identical to the control) --
    let cs = i32(1u << sh);
    let csf = f32(cs);
    let tDelta = abs(invRd) * csf;
    var tMax = (vec3<f32>(origin) + (vec3<f32>(cell) + stepGE) * csf - ro) * invRd;
    loop {
      // --- Lever 3: if the mask row along the dominant axis is empty and
      // the remaining row crossings all precede any lateral crossing (it
      // suffices to test the LAST one: crossings grow monotonically and the
      // selection predicates are monotone in t), consume the rest of the row
      // — ending outside the node along dom — in one jump. t/tMax advance by
      // the same serial additions per-cell stepping would perform, and the
      // skipped cells are in-bounds and empty by the row test, so the
      // committed state is bitwise-identical to the control's.
      var jumped = false;
      if (ENABLE_SKIP) {
        let cu = select(cell, vec3<i32>(3) - cell, mir);
        if (row_empty(mask, dom, cu)) {
          let sd = stepDir[dom];
          if (sd != 0) {
            let nSteps = select(cell[dom] + 1, 4 - cell[dom], sd > 0);
            var tLast = tMax[dom];
            if (nSteps > 1) { tLast += tDelta[dom]; }
            if (nSteps > 2) { tLast += tDelta[dom]; }
            if (nSteps > 3) { tLast += tDelta[dom]; }
            if (axis_selected(dom, tLast, tMax)) {
              cell[dom] += sd * nSteps;      // now outside [0,3] along dom
              t = tLast;
              tMax[dom] = tLast + tDelta[dom];
              amask = vec3<f32>(0.0);
              amask[dom] = 1.0;
              jumped = true;
            }
          }
        }
      }
      if (!jumped) {
        if (tMax.x <= tMax.y && tMax.x <= tMax.z) {
          t = tMax.x; tMax.x += tDelta.x; cell.x += stepDir.x; amask = vec3<f32>(1.0, 0.0, 0.0);
        } else if (tMax.y <= tMax.z) {
          t = tMax.y; tMax.y += tDelta.y; cell.y += stepDir.y; amask = vec3<f32>(0.0, 1.0, 0.0);
        } else {
          t = tMax.z; tMax.z += tDelta.z; cell.z += stepDir.z; amask = vec3<f32>(0.0, 0.0, 1.0);
        }
      }
      if (t > tExit) { return hit; }
      if (all(cell >= vec3<i32>(0)) && all(cell <= vec3<i32>(3)) &&
          !mask_bit(mask, cell_bit(cell, mir))) {
        continue;                      // empty sibling: keep stepping here
      }
      // Entered an occupied cell, or left the node: relocate (control header
      // note). cellBase is valid even when cell is out of [0,3] on the
      // stepped axis (it then names the neighbor region's base at this cell
      // size). Pure traversal-space geometry: unchanged under mirroring.
      let cellBase = origin + (cell << vec3<u32>(sh));
      let clamped = clamp(vec3<i32>(floor(ro + rdT * t)),
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
