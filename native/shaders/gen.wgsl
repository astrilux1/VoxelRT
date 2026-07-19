// Deterministic GPU scene generation for the sparse1024 tier.
//
// The world is defined by a pure function voxel_material(p, seed) -> u32 material
// (0 = air), evaluated on the GPU so no voxel data ever crosses the bus.
// Both acceleration-structure builders consume the same function, which is
// what makes the byte-identity gates meaningful.
//
// World recipe (seeded, ~2% occupancy at 1024^3):
//   - terrain shell: y < height(x, z) with fbm-ish value noise, but only a
//     24-voxel-thick crust is solid (true sparse occupancy, like streamed
//     game worlds, instead of a half-full solid volume). Expected solids:
//     24 * 1024^2 = 25.2M crust voxels, ~8% carved by caves -> ~23M,
//     towers ~0.1M -> ~2.2% of 1024^3 (target band 1-3%).
//   - caves: 3D value-noise threshold carves the crust
//   - structures: hash-scattered towers and slabs on the surface
//   - materials: stratified albedo by depth + noise; sparse emissive seams

const GRID : u32 = 1024u;

fn hash3(p : vec3<u32>, seed : u32) -> u32 {
  var h = p.x * 374761393u + p.y * 668265263u + p.z * 2147483647u + seed * 3266489917u;
  h = (h ^ (h >> 13u)) * 1274126177u;
  return h ^ (h >> 16u);
}

fn hashf(p : vec3<u32>, seed : u32) -> f32 {
  return f32(hash3(p, seed) & 0xffffffu) / 16777216.0;
}

// Trilinear value noise over a lattice with cell size `cs` (power of two).
fn vnoise(p : vec3<u32>, cs : u32, seed : u32) -> f32 {
  let cell = p / cs;
  let f = vec3<f32>(p % cs) / f32(cs);
  let u = f * f * (3.0 - 2.0 * f);
  var acc = 0.0;
  for (var i = 0u; i < 8u; i++) {
    let o = vec3<u32>(i & 1u, (i >> 1u) & 1u, (i >> 2u) & 1u);
    let w = mix(1.0 - u.x, u.x, f32(o.x)) * mix(1.0 - u.y, u.y, f32(o.y)) * mix(1.0 - u.z, u.z, f32(o.z));
    acc += w * hashf(cell + o, seed);
  }
  return acc;
}

// 2D specialization of vnoise on the y = 0 lattice plane: there u.y = 0, so
// the four o.y = 1 taps carry exactly zero weight; summing only the o.y = 0
// corners in the same order is bit-identical to vnoise(vec3(x, 0, z)).
fn vnoise2(xz : vec2<u32>, cs : u32, seed : u32) -> f32 {
  let cell = xz / cs;
  let f = vec2<f32>(xz % cs) / f32(cs);
  let u = f * f * (3.0 - 2.0 * f);
  var acc = 0.0;
  for (var i = 0u; i < 4u; i++) {
    let o = vec2<u32>(i & 1u, (i >> 1u) & 1u);
    let w = mix(1.0 - u.x, u.x, f32(o.x)) * mix(1.0 - u.y, u.y, f32(o.y));
    acc += w * hashf(vec3<u32>(cell.x + o.x, 0u, cell.y + o.y), seed);
  }
  return acc;
}

fn pack_mat(r : u32, g : u32, b : u32, e : u32) -> u32 {
  return r | (g << 8u) | (b << 16u) | (e << 24u);
}

fn terrain_height(xz : vec2<u32>, seed : u32) -> f32 {
  let h = vnoise2(xz, 256u, seed) * 0.55 + vnoise2(xz, 64u, seed + 1u) * 0.30 + vnoise2(xz, 16u, seed + 2u) * 0.15;
  return 256.0 + h * 384.0;   // surface lives in y = 256..640
}

fn voxel_material(p : vec3<u32>, seed : u32) -> u32 {
  let fy = f32(p.y);
  let h = terrain_height(vec2<u32>(p.x, p.z), seed);

  // Structures: scattered towers on a 64-voxel site grid.
  let site = vec3<u32>(p.x / 64u, 0u, p.z / 64u);
  let sh = hash3(site, seed + 77u);
  if ((sh & 31u) == 3u) {                      // ~3% of sites carry a tower
    let cx = f32(site.x * 64u + 32u);
    let cz = f32(site.z * 64u + 32u);
    let half = 3.0 + f32(sh >> 28u);           // 3..18 voxel half-width
    if (abs(f32(p.x) - cx) <= half && abs(f32(p.z) - cz) <= half) {
      let base = terrain_height(vec2<u32>(site.x * 64u + 32u, site.z * 64u + 32u), seed);
      let top = base + 48.0 + f32((sh >> 8u) & 63u);
      if (fy > base - 4.0 && fy < top) {
        // hollow interior, 1-voxel walls
        if (abs(f32(p.x) - cx) >= half - 1.0 || abs(f32(p.z) - cz) >= half - 1.0 || fy >= top - 1.0) {
          let em = select(0u, 180u, ((hash3(p, seed + 5u) & 255u) == 7u)); // sparse emissive windows
          return pack_mat(150u, 150u, 160u, em);
        }
        return 0u;
      }
    }
  }

  // Terrain crust: solid only within 24 voxels below the surface.
  if (fy < h && fy > h - 24.0) {
    // Caves carve the crust.
    if (vnoise(p, 32u, seed + 9u) > 0.72) { return 0u; }
    let strata = vnoise(p, 8u, seed + 13u);
    if (fy > h - 1.5) { return pack_mat(96u + u32(strata * 40.0), 140u, 60u, 0u); }  // grass-ish
    return pack_mat(120u, 100u + u32(strata * 30.0), 80u, 0u);                        // soil
  }
  return 0u;
}
