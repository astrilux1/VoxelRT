// Sparse-brickmap traversal: the native port of src/shaders/voxel.wgsl's
// two-level DDA, with one change required at 1024^3 scale: the dense voxel
// array is replaced by a pointer grid (brickPtr) into allocated bricks
// (brickMasks: 16 u32 / brick, brickMats: 512 u32 / brick). Traversal
// semantics are otherwise identical: outer DDA over 8^3 bricks, inner
// per-voxel DDA inside occupied bricks.

const GRID_T : i32 = 1024;
const BRICK_T : i32 = 8;
const BGRID_T : i32 = 128;
const VOXEL_SIZE : f32 = 0.0625;      // 1/16 m, matches the browser renderer
const INV_VOXEL : f32 = 16.0;

@group(1) @binding(0) var<storage, read> brickPtr : array<u32>;
@group(1) @binding(1) var<storage, read> brickMasks : array<u32>;
@group(1) @binding(2) var<storage, read> brickMats : array<u32>;

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

fn brickIndex(p : vec3<i32>) -> u32 {
  return u32(p.x) + u32(p.y) * u32(BGRID_T) + u32(p.z) * u32(BGRID_T * BGRID_T);
}

fn brickVoxelOccupied(slot : u32, local : vec3<i32>) -> bool {
  let bit = u32(local.x + local.y * BRICK_T + local.z * BRICK_T * BRICK_T);
  let word = brickMasks[slot * 16u + (bit >> 5u)];
  return (word & (1u << (bit & 31u))) != 0u;
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
  let stepGE = step(vec3<f32>(0.0), rd);

  let t0 = (vec3<f32>(0.0) - ro) * invRd;
  let t1 = (vec3<f32>(f32(GRID_T)) - ro) * invRd;
  let tmin3 = min(t0, t1);
  let tmax3 = max(t0, t1);
  let tEnter = max(max(tmin3.x, tmin3.y), max(tmin3.z, 0.0));
  let tExit = min(min(tmax3.x, tmax3.y), min(tmax3.z, maxT));
  if (tEnter > tExit) { return hit; }

  var mask : vec3<f32>;
  if (tmin3.x >= tmin3.y && tmin3.x >= tmin3.z) { mask = vec3<f32>(1.0, 0.0, 0.0); }
  else if (tmin3.y >= tmin3.z)                  { mask = vec3<f32>(0.0, 1.0, 0.0); }
  else                                          { mask = vec3<f32>(0.0, 0.0, 1.0); }

  var t = tEnter;
  let startP = ro + rd * (t + 1e-4);
  var bpos = clamp(vec3<i32>(floor(startP / f32(BRICK_T))), vec3<i32>(0), vec3<i32>(BGRID_T - 1));
  let tDeltaB = abs(invRd) * f32(BRICK_T);
  var tMaxB = ((vec3<f32>(bpos) + stepGE) * f32(BRICK_T) - ro) * invRd;

  for (var iter = 0; iter < 3 * BGRID_T + 2; iter++) {
    if (t > tExit) { break; }

    let slot = brickPtr[brickIndex(bpos)];
    if (slot != 0xffffffffu) {
      let bMin = bpos * BRICK_T;
      let bMax = bMin + vec3<i32>(BRICK_T - 1);
      let p = ro + rd * (t + 1e-4);
      var vpos = clamp(vec3<i32>(floor(p)), bMin, bMax);
      var tMaxV = ((vec3<f32>(vpos) + stepGE) - ro) * invRd;
      var tCur = t;
      var vmask = mask;

      for (var j = 0; j < 3 * BRICK_T; j++) {
        let local = vpos - bMin;
        if (brickVoxelOccupied(slot, local)) {
          if (tCur > maxT) { return hit; }
          let bit = u32(local.x + local.y * BRICK_T + local.z * BRICK_T * BRICK_T);
          hit.t = tCur * VOXEL_SIZE;
          hit.n = -vmask * vec3<f32>(stepDir);
          hit.mat = brickMats[slot * 512u + bit];
          return hit;
        }
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

    if (tMaxB.x <= tMaxB.y && tMaxB.x <= tMaxB.z) {
      t = tMaxB.x; tMaxB.x += tDeltaB.x; bpos.x += stepDir.x; mask = vec3<f32>(1.0, 0.0, 0.0);
    } else if (tMaxB.y <= tMaxB.z) {
      t = tMaxB.y; tMaxB.y += tDeltaB.y; bpos.y += stepDir.y; mask = vec3<f32>(0.0, 1.0, 0.0);
    } else {
      t = tMaxB.z; tMaxB.z += tDeltaB.z; bpos.z += stepDir.z; mask = vec3<f32>(0.0, 0.0, 1.0);
    }
    if (any(bpos < vec3<i32>(0)) || any(bpos >= vec3<i32>(BGRID_T))) { break; }
  }
  return hit;
}
