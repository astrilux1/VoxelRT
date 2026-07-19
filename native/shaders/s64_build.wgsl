// GPU build of the sparse voxel 64-tree (docs/S64.md §3). The host
// concatenates gen.wgsl + source.wgsl ahead of this file, so the same pure
// voxel_fetch() source (procedural or fixture buffer) feeds both backends
// (that is what makes the identity gate meaningful).
//
// Layout produced (bit index inside any 4^3 tile is x + y*4 + z*16):
//   tlas[0..2)      TLAS root: 64-bit mask over 4^3 chunk-groups (256^3 vox)
//   tlas[2..130)    64 group masks (lo,hi) over each group's 4^3 chunks
//   tlas[130..4226) chunkRoots: per-chunk root node index into nodes[],
//                   0xffffffff for empty chunks
//   nodes[]         vec4<u32> {maskLo, maskHi, childBase, 0}
//                     [0..numRoots)      chunk roots (children: 16^3 cells);
//                                        childBase = absolute nodes[] index
//                                        of the first L1 child
//                     [numRoots..)       L1 nodes (children: 4^3 cells);
//                                        childBase = leaves[] index
//   leaves[]        vec4<u32> {maskLo, maskHi, matBase, 0}: 64-bit voxel mask
//   mats[]          popcount-compacted materials: solid voxel v of leaf L is
//                   mats[L.matBase + popcnt(mask below v)]
//
// Pass / dispatch structure (allocation always comes from exclusive scans,
// never from racy atomics -> deterministic; masks use atomicOr, which is
// order-independent; every output word has exactly one writer):
//
//   phase A                    dispatch            writes
//     pass_leaf_count          256^3 wg(4,4,4)     leafCount = per-leaf popcount
//     pass_l1                  64^3  wg(4,4,4)     l1Mask, l1ChildCount, l1MatCount
//     pass_chunk               16^3  wg(4,4,4)     chunkMask, rootChildCount, chunkOcc
//     pass_tlas                1     wg(64)        tlas[0..130)
//   4x gpu_scan (scan.wgsl, in place):
//     l1ChildCount   -> leaves[] base per L1 cell     (total = leaf count)
//     l1MatCount     -> mats[] base per L1 cell       (total = solid voxels)
//     rootChildCount -> L1-node base per chunk        (total = L1 count)
//     chunkOcc       -> root slot per chunk           (total = root count)
//   phase B (host wrote sp.numRoots; nodes/leaves/mats now allocated)
//     pass_matbase             64^3  wg(4,4,4)     leafCount := absolute matBase
//     pass_roots               4096  wg(64)        nodes[rootSlot], tlas[130+ci]
//     pass_l1_write            64^3  wg(64)        nodes[l1Slot]
//     pass_leaf_fill           256^3 wg(4,4,4)     leaves[], mats[]
//
// Leaf masks are not stored densely (256^3 x 8 B = 134 MB); pass_leaf_fill
// re-evaluates voxel_fetch instead, exactly like brickmap's pass_fill.

const NONE_S64 : u32 = 0xffffffffu;
const LGRID_B : u32 = 256u;   // leaf grid: 1024 / 4
const L1GRID_B : u32 = 64u;   // L1 grid:   1024 / 16
const CGRID_B : u32 = 16u;    // chunk grid: 1024 / 64

struct S64Params {
  seed : u32,
  numRoots : u32,   // valid in phase B only (host writes it after the scans)
  sourceMode : u32, // 0 = procedural voxel_material, 1 = srcVoxels buffer
  _p1 : u32,
};

@group(0) @binding(0) var<uniform> sp : S64Params;
@group(0) @binding(1) var<storage, read_write> leafCount : array<u32>;      // popcount, then absolute matBase
@group(0) @binding(2) var<storage, read_write> l1Mask : array<vec2<u32>>;
@group(0) @binding(3) var<storage, read_write> l1ChildCount : array<u32>;   // popcount, then scanned leaf base
@group(0) @binding(4) var<storage, read_write> l1MatCount : array<u32>;     // solid sum, then scanned mat base
@group(0) @binding(5) var<storage, read_write> chunkMask : array<vec2<u32>>;
@group(0) @binding(6) var<storage, read_write> rootChildCount : array<u32>; // popcount, then scanned L1 base
@group(0) @binding(7) var<storage, read_write> chunkOcc : array<u32>;       // 0/1, then scanned root slot
@group(0) @binding(8) var<storage, read_write> tlas : array<u32>;
@group(0) @binding(9) var<storage, read_write> nodes : array<vec4<u32>>;
@group(0) @binding(10) var<storage, read_write> leaves : array<vec4<u32>>;
@group(0) @binding(11) var<storage, read_write> mats : array<u32>;

fn popcnt64(m : vec2<u32>) -> u32 {
  return countOneBits(m.x) + countOneBits(m.y);
}

// Number of set bits strictly below bit b of a 64-bit (lo,hi) mask.
fn popcnt_below(m : vec2<u32>, b : u32) -> u32 {
  if (b == 0u) { return 0u; }
  if (b < 32u) { return countOneBits(m.x & ((1u << b) - 1u)); }
  return countOneBits(m.x) + countOneBits(m.y & ((1u << (b - 32u)) - 1u));
}

var<workgroup> wg_mask : array<atomic<u32>, 2>;
var<workgroup> wg_count : atomic<u32>;

// --- phase A ---------------------------------------------------------------

// One workgroup per 4^3 leaf cell; local_invocation_index == the leaf bit
// (x + y*4 + z*16), so the OR target falls out directly.
@compute @workgroup_size(4, 4, 4)
fn pass_leaf_count(@builtin(workgroup_id) wid : vec3<u32>,
                   @builtin(local_invocation_id) lid : vec3<u32>,
                   @builtin(local_invocation_index) li : u32) {
  if (li < 2u) { atomicStore(&wg_mask[li], 0u); }
  workgroupBarrier();
  let p = wid * 4u + lid;
  if (voxel_fetch(p, sp.seed, sp.sourceMode) != 0u) {
    atomicOr(&wg_mask[li >> 5u], 1u << (li & 31u));
  }
  workgroupBarrier();
  if (li == 0u) {
    let m = vec2<u32>(atomicLoad(&wg_mask[0]), atomicLoad(&wg_mask[1]));
    leafCount[wid.x + wid.y * LGRID_B + wid.z * LGRID_B * LGRID_B] = popcnt64(m);
  }
}

// One workgroup per L1 cell (4^3 leaves): occupancy mask over child leaves,
// child count for leaf allocation, and summed solid count for material
// allocation (u32 atomicAdd commutes -> order-independent).
@compute @workgroup_size(4, 4, 4)
fn pass_l1(@builtin(workgroup_id) wid : vec3<u32>,
           @builtin(local_invocation_id) lid : vec3<u32>,
           @builtin(local_invocation_index) li : u32) {
  if (li < 2u) { atomicStore(&wg_mask[li], 0u); }
  if (li == 0u) { atomicStore(&wg_count, 0u); }
  workgroupBarrier();
  let lc = wid * 4u + lid;   // leaf coord in 256^3
  let n = leafCount[lc.x + lc.y * LGRID_B + lc.z * LGRID_B * LGRID_B];
  if (n != 0u) {
    atomicOr(&wg_mask[li >> 5u], 1u << (li & 31u));
    atomicAdd(&wg_count, n);
  }
  workgroupBarrier();
  if (li == 0u) {
    let m = vec2<u32>(atomicLoad(&wg_mask[0]), atomicLoad(&wg_mask[1]));
    let i = wid.x + wid.y * L1GRID_B + wid.z * L1GRID_B * L1GRID_B;
    l1Mask[i] = m;
    l1ChildCount[i] = popcnt64(m);
    l1MatCount[i] = atomicLoad(&wg_count);
  }
}

// One workgroup per chunk (4^3 L1 cells): chunk root mask + counts.
@compute @workgroup_size(4, 4, 4)
fn pass_chunk(@builtin(workgroup_id) wid : vec3<u32>,
              @builtin(local_invocation_id) lid : vec3<u32>,
              @builtin(local_invocation_index) li : u32) {
  if (li < 2u) { atomicStore(&wg_mask[li], 0u); }
  workgroupBarrier();
  let c = wid * 4u + lid;    // L1 coord in 64^3
  if (l1ChildCount[c.x + c.y * L1GRID_B + c.z * L1GRID_B * L1GRID_B] != 0u) {
    atomicOr(&wg_mask[li >> 5u], 1u << (li & 31u));
  }
  workgroupBarrier();
  if (li == 0u) {
    let m = vec2<u32>(atomicLoad(&wg_mask[0]), atomicLoad(&wg_mask[1]));
    let i = wid.x + wid.y * CGRID_B + wid.z * CGRID_B * CGRID_B;
    chunkMask[i] = m;
    rootChildCount[i] = popcnt64(m);
    chunkOcc[i] = select(0u, 1u, (m.x | m.y) != 0u);
  }
}

// Single 64-thread workgroup: thread g owns chunk-group g, builds its 64-bit
// chunk mask, then all threads OR the group-occupancy bits into the TLAS
// root mask (§3: root 64-bit mask over 4^3 chunk-groups).
@compute @workgroup_size(64)
fn pass_tlas(@builtin(local_invocation_index) li : u32) {
  if (li < 2u) { atomicStore(&wg_mask[li], 0u); }
  workgroupBarrier();
  let g = vec3<u32>(li & 3u, (li >> 2u) & 3u, li >> 4u);
  var m = vec2<u32>(0u, 0u);
  for (var c = 0u; c < 64u; c++) {
    let cl = vec3<u32>(c & 3u, (c >> 2u) & 3u, c >> 4u);
    let cc = g * 4u + cl;
    if (chunkOcc[cc.x + cc.y * CGRID_B + cc.z * CGRID_B * CGRID_B] != 0u) {
      if (c < 32u) { m.x |= (1u << c); } else { m.y |= (1u << (c - 32u)); }
    }
  }
  tlas[2u + li * 2u] = m.x;
  tlas[3u + li * 2u] = m.y;
  if ((m.x | m.y) != 0u) { atomicOr(&wg_mask[li >> 5u], 1u << (li & 31u)); }
  workgroupBarrier();
  if (li < 2u) { tlas[li] = atomicLoad(&wg_mask[li]); }
}

// --- phase B ---------------------------------------------------------------

var<workgroup> wg_scan : array<u32, 64>;

// One workgroup per L1 cell: exclusive scan of its 64 child-leaf solid
// counts (li order == bit order), rebasing leafCount[] from per-leaf count
// to absolute matBase = scanned l1MatCount + local exclusive prefix.
@compute @workgroup_size(4, 4, 4)
fn pass_matbase(@builtin(workgroup_id) wid : vec3<u32>,
                @builtin(local_invocation_id) lid : vec3<u32>,
                @builtin(local_invocation_index) li : u32) {
  let lc = wid * 4u + lid;
  let leafIdx = lc.x + lc.y * LGRID_B + lc.z * LGRID_B * LGRID_B;
  let own = leafCount[leafIdx];
  wg_scan[li] = own;
  // Hillis-Steele inclusive scan (same idiom as scan.wgsl).
  var offset = 1u;
  loop {
    if (offset >= 64u) { break; }
    workgroupBarrier();
    var v = wg_scan[li];
    if (li >= offset) { v += wg_scan[li - offset]; }
    workgroupBarrier();
    wg_scan[li] = v;
    offset = offset << 1u;
  }
  workgroupBarrier();
  let base = l1MatCount[wid.x + wid.y * L1GRID_B + wid.z * L1GRID_B * L1GRID_B];
  leafCount[leafIdx] = base + wg_scan[li] - own;   // exclusive prefix
}

// One thread per chunk: write the compacted root node and the TLAS
// chunk-root reference (every tlas[130+ci] word gets exactly one writer).
@compute @workgroup_size(64)
fn pass_roots(@builtin(global_invocation_id) gid : vec3<u32>) {
  let ci = gid.x;            // dispatch(64) x wg(64) = 4096 chunks
  let m = chunkMask[ci];
  if ((m.x | m.y) != 0u) {
    let slot = chunkOcc[ci];                 // scanned: root slot
    nodes[slot] = vec4<u32>(m.x, m.y, sp.numRoots + rootChildCount[ci], 0u);
    tlas[130u + ci] = slot;
  } else {
    tlas[130u + ci] = NONE_S64;
  }
}

// One thread per L1 cell: write the allocated L1 node. Its slot is the
// parent chunk's L1 base plus the popcount rank of its bit, so siblings are
// contiguous in bit order (required by childBase + popcnt indexing).
@compute @workgroup_size(64)
fn pass_l1_write(@builtin(global_invocation_id) gid : vec3<u32>) {
  let i = gid.x;             // dispatch(4096) x wg(64) = 64^3 L1 cells
  let m = l1Mask[i];
  if ((m.x | m.y) == 0u) { return; }
  let lc = vec3<u32>(i % L1GRID_B, (i / L1GRID_B) % L1GRID_B, i / (L1GRID_B * L1GRID_B));
  let cc = lc / 4u;
  let ci = cc.x + cc.y * CGRID_B + cc.z * CGRID_B * CGRID_B;
  let bit = (lc.x & 3u) + (lc.y & 3u) * 4u + (lc.z & 3u) * 16u;
  let slot = sp.numRoots + rootChildCount[ci] + popcnt_below(chunkMask[ci], bit);
  nodes[slot] = vec4<u32>(m.x, m.y, l1ChildCount[i], 0u);   // childBase into leaves[]
}

// One workgroup per leaf cell: recompute the mask (same evaluation as
// pass_leaf_count) and write the leaf record plus compacted materials.
@compute @workgroup_size(4, 4, 4)
fn pass_leaf_fill(@builtin(workgroup_id) wid : vec3<u32>,
                  @builtin(local_invocation_id) lid : vec3<u32>,
                  @builtin(local_invocation_index) li : u32) {
  if (li < 2u) { atomicStore(&wg_mask[li], 0u); }
  workgroupBarrier();
  let p = wid * 4u + lid;
  let m = voxel_fetch(p, sp.seed, sp.sourceMode);
  if (m != 0u) { atomicOr(&wg_mask[li >> 5u], 1u << (li & 31u)); }
  workgroupBarrier();
  let mask = vec2<u32>(atomicLoad(&wg_mask[0]), atomicLoad(&wg_mask[1]));
  if ((mask.x | mask.y) == 0u) { return; }
  let leafIdx = wid.x + wid.y * LGRID_B + wid.z * LGRID_B * LGRID_B;
  let l1 = wid / 4u;
  let l1Idx = l1.x + l1.y * L1GRID_B + l1.z * L1GRID_B * L1GRID_B;
  let matBase = leafCount[leafIdx];          // rebased by pass_matbase
  if (li == 0u) {
    let bit = (wid.x & 3u) + (wid.y & 3u) * 4u + (wid.z & 3u) * 16u;
    let slot = l1ChildCount[l1Idx] + popcnt_below(l1Mask[l1Idx], bit);
    leaves[slot] = vec4<u32>(mask.x, mask.y, matBase, 0u);
  }
  if (m != 0u) {
    mats[matBase + popcnt_below(mask, li)] = m;
  }
}
