// Generation source switch (docs/S64.md §3): both builders read voxels via
// voxel_fetch(), which either evaluates the procedural voxel_material()
// (sourceMode 0, sparse1024) or samples an uploaded voxel buffer
// (sourceMode 1, fixture256: the VXF1 payload from test/dump-scene.mjs,
// 256^3 u32 in x + y*N + z*N^2 order, occupying the [0,256)^3 corner of the
// 1024^3 grid). The buffer lives in its own bind group so the builders'
// group-0 layouts are untouched; procedural runs bind a 4-byte dummy that is
// never read.

const SRC_GRID : u32 = 256u;

@group(1) @binding(0) var<storage, read> srcVoxels : array<u32>;

fn voxel_fetch(p : vec3<u32>, seed : u32, sourceMode : u32) -> u32 {
  if (sourceMode == 0u) { return voxel_material(p, seed); }
  if (any(p >= vec3<u32>(SRC_GRID))) { return 0u; }
  return srcVoxels[p.x + p.y * SRC_GRID + p.z * SRC_GRID * SRC_GRID];
}
