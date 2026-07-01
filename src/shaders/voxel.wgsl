// ---------------------------------------------------------------------------
// voxel.wgsl — voxel world bindings + two-level DDA traversal.
// Included only by shaders that actually trace rays (pathtrace), so that
// auto pipeline layouts for the other passes don't see these bindings.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Voxel world access.
// A voxel is a packed u32: R | G<<8 | B<<16 | emissive<<24 (0 = empty air).
// `bricks` holds one u32 per BRICK^3 block: non-zero when any voxel inside
// is solid, letting the traversal skip empty space at brick granularity.
// ---------------------------------------------------------------------------

@group(0) @binding(1) var<storage, read> voxels : array<u32>;
@group(0) @binding(2) var<storage, read> bricks : array<u32>;

fn voxelAt(p : vec3<i32>) -> u32 {
  return voxels[u32(p.x) + u32(p.y) * u32(GRID) + u32(p.z) * u32(GRID * GRID)];
}

fn brickAt(p : vec3<i32>) -> u32 {
  return bricks[u32(p.x) + u32(p.y) * u32(BGRID) + u32(p.z) * u32(BGRID * BGRID)];
}

struct Material {
  albedo   : vec3<f32>,
  emissive : f32,   // 0..1 emissive intensity
};

fn unpackMaterial(m : u32) -> Material {
  var mat : Material;
  mat.albedo = vec3<f32>(
    f32(m & 0xffu),
    f32((m >> 8u) & 0xffu),
    f32((m >> 16u) & 0xffu)) * (1.0 / 255.0);
  // Perceptual -> linear-ish so authored byte colors look right.
  mat.albedo = mat.albedo * mat.albedo;
  mat.emissive = f32((m >> 24u) & 0xffu) * (1.0 / 255.0);
  return mat;
}

// ---------------------------------------------------------------------------
// Ray traversal — two-level DDA (Amanatides & Woo) in voxel space.
// The outer DDA walks BRICK-sized cells; occupied bricks are refined with an
// inner per-voxel DDA. Distances returned in world meters.
// ---------------------------------------------------------------------------

struct Hit {
  t   : f32,          // world-space distance, < 0 on miss
  n   : vec3<f32>,    // geometric normal of the hit face
  mat : u32,          // packed material
};

fn safeInv(v : vec3<f32>) -> vec3<f32> {
  let s = vec3<f32>(
    select(-1.0, 1.0, v.x >= 0.0),
    select(-1.0, 1.0, v.y >= 0.0),
    select(-1.0, 1.0, v.z >= 0.0));
  return s / max(abs(v), vec3<f32>(1e-8));
}

fn trace(roWorld : vec3<f32>, rd : vec3<f32>, maxTWorld : f32) -> Hit {
  var hit : Hit;
  hit.t = -1.0;
  hit.mat = 0u;
  hit.n = -rd;

  let ro = roWorld * INV_VOXEL;              // work in voxel units
  let maxT = maxTWorld * INV_VOXEL;
  let invRd = safeInv(rd);
  let stepDir = vec3<i32>(sign(rd));
  let stepGE = step(vec3<f32>(0.0), rd);     // 1 where rd >= 0

  // Clip the ray against the whole grid.
  let t0 = (vec3<f32>(0.0) - ro) * invRd;
  let t1 = (vec3<f32>(f32(GRID)) - ro) * invRd;
  let tmin3 = min(t0, t1);
  let tmax3 = max(t0, t1);
  let tEnter = max(max(tmin3.x, tmin3.y), max(tmin3.z, 0.0));
  let tExit = min(min(tmax3.x, tmax3.y), min(tmax3.z, maxT));
  if (tEnter > tExit) { return hit; }

  // Face normal for the grid entry point.
  var mask : vec3<f32>;
  if (tmin3.x >= tmin3.y && tmin3.x >= tmin3.z) { mask = vec3<f32>(1.0, 0.0, 0.0); }
  else if (tmin3.y >= tmin3.z)                  { mask = vec3<f32>(0.0, 1.0, 0.0); }
  else                                          { mask = vec3<f32>(0.0, 0.0, 1.0); }

  var t = tEnter;
  let startP = ro + rd * (t + 1e-4);
  var bpos = clamp(vec3<i32>(floor(startP / f32(BRICK))), vec3<i32>(0), vec3<i32>(BGRID - 1));
  let tDeltaB = abs(invRd) * f32(BRICK);
  var tMaxB = ((vec3<f32>(bpos) + stepGE) * f32(BRICK) - ro) * invRd;

  for (var iter = 0; iter < 3 * BGRID + 2; iter++) {
    if (t > tExit) { break; }

    if (brickAt(bpos) != 0u) {
      // Refine inside this brick with a per-voxel DDA.
      let bMin = bpos * BRICK;
      let bMax = bMin + vec3<i32>(BRICK - 1);
      let p = ro + rd * (t + 1e-4);
      var vpos = clamp(vec3<i32>(floor(p)), bMin, bMax);
      var tMaxV = ((vec3<f32>(vpos) + stepGE) - ro) * invRd;
      var tCur = t;
      var vmask = mask;

      for (var j = 0; j < 3 * BRICK; j++) {
        let m = voxelAt(vpos);
        if (m != 0u) {
          if (tCur > maxT) { return hit; }
          hit.t = tCur * VOXEL_SIZE;
          hit.n = -vmask * vec3<f32>(stepDir);
          hit.mat = m;
          return hit;
        }
        // Step to the next voxel.
        if (tMaxV.x <= tMaxV.y && tMaxV.x <= tMaxV.z) {
          tCur = tMaxV.x; tMaxV.x += abs(invRd.x); vpos.x += stepDir.x; vmask = vec3<f32>(1.0, 0.0, 0.0);
        } else if (tMaxV.y <= tMaxV.z) {
          tCur = tMaxV.y; tMaxV.y += abs(invRd.y); vpos.y += stepDir.y; vmask = vec3<f32>(0.0, 1.0, 0.0);
        } else {
          tCur = tMaxV.z; tMaxV.z += abs(invRd.z); vpos.z += stepDir.z; vmask = vec3<f32>(0.0, 0.0, 1.0);
        }
        if (any(vpos < bMin) || any(vpos > bMax)) { break; }
      }
    }

    // Step to the next brick.
    if (tMaxB.x <= tMaxB.y && tMaxB.x <= tMaxB.z) {
      t = tMaxB.x; tMaxB.x += tDeltaB.x; bpos.x += stepDir.x; mask = vec3<f32>(1.0, 0.0, 0.0);
    } else if (tMaxB.y <= tMaxB.z) {
      t = tMaxB.y; tMaxB.y += tDeltaB.y; bpos.y += stepDir.y; mask = vec3<f32>(0.0, 1.0, 0.0);
    } else {
      t = tMaxB.z; tMaxB.z += tDeltaB.z; bpos.z += stepDir.z; mask = vec3<f32>(0.0, 0.0, 1.0);
    }
    if (any(bpos < vec3<i32>(0)) || any(bpos >= vec3<i32>(BGRID))) { break; }
  }
  return hit;
}

fn traceShadow(ro : vec3<f32>, rd : vec3<f32>, maxT : f32) -> bool {
  return trace(ro, rd, maxT).t >= 0.0;
}

