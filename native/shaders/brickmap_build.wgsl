// GPU build of the sparse brickmap: 1024^3 world, 8^3 bricks, 128^3 pointer
// grid. Three passes over the pure voxel_fetch() source switch from
// source.wgsl (the host concatenates gen.wgsl + source.wgsl ahead of this
// file):
//
//   pass_occupancy: one workgroup per brick (8^3 = 512 threads), workgroup-OR
//     of voxel occupancy -> brickOccupied[bi] = 0/1 count word
//   (host-side or GPU exclusive scan over brickOccupied -> brickSlot[])
//   pass_fill: one workgroup per *occupied* brick: writes the 512-bit mask
//     (16 u32 words) and 512 material u32s into the brick's allocated slot,
//     and the pointer grid entry.
//
// The scan is done in scan.wgsl (generic exclusive scan over u32s).

const BRICK : u32 = 8u;
const BGRID : u32 = 128u;   // GRID / BRICK

struct GenParams {
  seed : u32,
  gridShift : u32,   // log2(GRID) - unused hook for other grid sizes
  sourceMode : u32,  // 0 = procedural voxel_material, 1 = srcVoxels buffer
  _pad1 : u32,
};

@group(0) @binding(0) var<uniform> gp : GenParams;
@group(0) @binding(1) var<storage, read_write> brickOccupied : array<u32>; // 1 if any voxel solid
@group(0) @binding(2) var<storage, read> brickSlot : array<u32>;           // exclusive scan of occupied
@group(0) @binding(3) var<storage, read_write> brickPtr : array<u32>;      // pointer grid, 0xffffffff = empty
@group(0) @binding(4) var<storage, read_write> brickMasks : array<u32>;    // 16 u32 per allocated brick
@group(0) @binding(5) var<storage, read_write> brickMats : array<u32>;     // 512 u32 per allocated brick

var<workgroup> wg_any : atomic<u32>;

@compute @workgroup_size(8, 8, 8)
fn pass_occupancy(@builtin(workgroup_id) wid : vec3<u32>,
                  @builtin(local_invocation_id) lid : vec3<u32>,
                  @builtin(local_invocation_index) li : u32) {
  if (li == 0u) { atomicStore(&wg_any, 0u); }
  workgroupBarrier();
  let p = wid * BRICK + lid;
  if (voxel_fetch(p, gp.seed, gp.sourceMode) != 0u) { atomicOr(&wg_any, 1u); }
  workgroupBarrier();
  if (li == 0u) {
    let bi = wid.x + wid.y * BGRID + wid.z * BGRID * BGRID;
    brickOccupied[bi] = atomicLoad(&wg_any);
  }
}

var<workgroup> wg_mask : array<atomic<u32>, 16>;

@compute @workgroup_size(8, 8, 8)
fn pass_fill(@builtin(workgroup_id) wid : vec3<u32>,
             @builtin(local_invocation_id) lid : vec3<u32>,
             @builtin(local_invocation_index) li : u32) {
  let bi = wid.x + wid.y * BGRID + wid.z * BGRID * BGRID;
  if (li < 16u) { atomicStore(&wg_mask[li], 0u); }
  workgroupBarrier();

  let occupied = brickOccupied[bi] != 0u;
  let slot = brickSlot[bi];
  if (occupied) {
    // li == lid.x + lid.y*8 + lid.z*64 (WGSL local_invocation_index layout),
    // i.e. exactly the linear voxel index brickVoxelOccupied() expects for
    // both the mask bit and the material slot.
    let m = voxel_fetch(wid * BRICK + lid, gp.seed, gp.sourceMode);
    if (m != 0u) { atomicOr(&wg_mask[li >> 5u], 1u << (li & 31u)); }
    brickMats[slot * 512u + li] = m;
  }
  workgroupBarrier();
  if (li == 0u) {
    // Unoccupied bricks carry the slot value of the next occupied brick
    // (exclusive scan does not advance on zeros), so only the pointer entry
    // may ever be written for them.
    brickPtr[bi] = select(0xffffffffu, slot, occupied);
  }
  if (occupied && li < 16u) {
    brickMasks[slot * 16u + li] = atomicLoad(&wg_mask[li]);
  }
}
