// Procedural voxel scene at 1/16 m voxel scale: rolling terrain, a
// Cornell-style room to show off color bleeding, boxes, trees and a few
// emissive lanterns for local GI.

export const VOXEL_SIZE = 1 / 16;   // meters
export const BRICK = 8;

// Pack a material: linear-ish byte color + emissive intensity byte (0 = off).
const M = (r, g, b, e = 0) => ((r | (g << 8) | (b << 16) | (e << 24)) >>> 0);

const MAT = {
  grass: (v) => M(88 + v, 130 + v, 60, 0),
  dirt: (v) => M(110 + v, 82 + v, 58, 0),
  rock: (v) => M(120 + v, 120 + v, 125 + v, 0),
  white: M(225, 222, 215),
  red: M(200, 45, 40),
  green: M(55, 175, 60),
  floorA: M(180, 150, 110),
  floorB: M(150, 118, 84),
  boxTall: M(200, 200, 210),
  boxShort: M(210, 170, 90),
  trunk: M(95, 68, 42),
  leaf: (v) => M(50 + v, 120 + v, 45, 0),
  lampPost: M(60, 60, 65),
  lampWarm: M(255, 190, 110, 255),
  lampCyan: M(90, 220, 255, 255),
  lampMagenta: M(255, 90, 200, 255),
  ceilLight: M(255, 240, 220, 210),
};

// Deterministic 2D value noise for the terrain.
function makeNoise(seed) {
  const hash = (x, y) => {
    let h = (x * 374761393 + y * 668265263 + seed * 1442695041) | 0;
    h = Math.imul(h ^ (h >>> 13), 1274126177);
    return ((h ^ (h >>> 16)) >>> 0) / 4294967296;
  };
  const smooth = (t) => t * t * (3 - 2 * t);
  const noise2 = (x, y) => {
    const xi = Math.floor(x), yi = Math.floor(y);
    const xf = smooth(x - xi), yf = smooth(y - yi);
    const a = hash(xi, yi), b = hash(xi + 1, yi);
    const c = hash(xi, yi + 1), d = hash(xi + 1, yi + 1);
    return a + (b - a) * xf + (c - a) * yf + (a - b - c + d) * xf * yf;
  };
  return (x, y) => {
    let v = 0, amp = 0.5, f = 1;
    for (let o = 0; o < 4; o++) { v += noise2(x * f, y * f) * amp; amp *= 0.5; f *= 2; }
    return v;
  };
}

export function generateScene(N) {
  const vox = new Uint32Array(N * N * N);
  const idx = (x, y, z) => x + y * N + z * N * N;
  const inb = (x, y, z) => x >= 0 && y >= 0 && z >= 0 && x < N && y < N && z < N;
  const set = (x, y, z, m) => { if (inb(x, y, z)) vox[idx(x, y, z)] = m; };

  const box = (x0, y0, z0, x1, y1, z1, m) => {
    for (let z = z0; z <= z1; z++)
      for (let y = y0; y <= y1; y++)
        for (let x = x0; x <= x1; x++) set(x, y, z, typeof m === 'function' ? m(x, y, z) : m);
  };

  const noise = makeNoise(1337);
  const worldM = N * VOXEL_SIZE;                    // world extent in meters
  const V = (meters) => Math.round(meters / VOXEL_SIZE);

  // --- Terrain -------------------------------------------------------------
  const baseH = Math.floor(N * 0.16);
  const heights = new Int32Array(N * N);
  for (let z = 0; z < N; z++) {
    for (let x = 0; x < N; x++) {
      const h = baseH + Math.floor(noise(x * 0.02, z * 0.02) * N * 0.10);
      heights[x + z * N] = h;
      for (let y = 0; y <= h; y++) {
        const v = (x * 7 + y * 13 + z * 5) % 9;
        let m;
        if (y === h) m = MAT.grass(v);
        else if (y > h - 4) m = MAT.dirt(v);
        else m = MAT.rock(v);
        vox[idx(x, y, z)] = m;
      }
    }
  }
  const groundAt = (x, z) => heights[Math.max(0, Math.min(N - 1, x)) + Math.max(0, Math.min(N - 1, z)) * N];

  // --- Cornell-style room (white walls, red left, green right, opening at
  // the front, warm area light strip in the ceiling) ------------------------
  const roomW = Math.min(V(5), Math.floor(N * 0.45));   // interior extent
  const roomH = Math.min(V(3.2), Math.floor(N * 0.3));
  const cx = Math.floor(N * 0.5);
  const cz = Math.floor(N * 0.55);
  const x0 = cx - (roomW >> 1), x1 = cx + (roomW >> 1);
  const z0 = cz - (roomW >> 1), z1 = cz + (roomW >> 1);
  const floorY = groundAt(cx, cz) + 1;
  const y1 = floorY + roomH;
  const T = 2;   // wall thickness (2 voxels = 12.5 cm)

  // Level the terrain under the room footprint, then carve the interior.
  box(x0 - T, floorY, z0 - T, x1 + T, floorY, z1 + T, (x, y, z) => MAT.dirt((x + z) % 9));
  box(x0 - T, floorY + 1, z0 - T, x1 + T, y1 + T + 4, z1 + T, 0);

  // Checkerboard floor.
  box(x0 - T, floorY, z0 - T, x1 + T, floorY, z1 + T,
    (x, y, z) => ((x >> 3) + (z >> 3)) % 2 ? MAT.floorA : MAT.floorB);

  // Walls / ceiling.
  box(x0 - T, floorY + 1, z1 + 1, x1 + T, y1, z1 + T, MAT.white);          // back
  box(x0 - T, floorY + 1, z0 - T, x1 + T, y1, z0 - 1, MAT.white);          // front
  box(x0 - T, floorY + 1, z0 - T, x0 - 1, y1, z1 + T, MAT.red);            // left (-x)
  box(x1 + 1, floorY + 1, z0 - T, x1 + T, y1, z1 + T, MAT.green);          // right (+x)
  box(x0 - T, y1 + 1, z0 - T, x1 + T, y1 + T, z1 + T, MAT.white);          // ceiling

  // Front opening (doorway) so sun and camera can see inside.
  const doorHalf = Math.floor(roomW * 0.28);
  box(cx - doorHalf, floorY + 1, z0 - T, cx + doorHalf, y1 - 2, z0 - 1, 0);

  // Window in the right (+x) wall for direct sun shafts.
  const winY0 = floorY + Math.floor(roomH * 0.45);
  const winY1 = floorY + Math.floor(roomH * 0.8);
  box(x1 + 1, winY0, cz - Math.floor(roomW * 0.3), x1 + T, winY1, cz + Math.floor(roomW * 0.3), 0);

  // Ceiling light strip (area light).
  const lHalf = Math.floor(roomW * 0.18);
  box(cx - lHalf, y1, cz - lHalf, cx + lHalf, y1, cz + lHalf, MAT.ceilLight);

  // Classic two boxes.
  const tallW = Math.floor(roomW * 0.16), tallH = Math.floor(roomH * 0.55);
  box(cx - Math.floor(roomW * 0.28) - tallW, floorY + 1, cz + Math.floor(roomW * 0.05),
      cx - Math.floor(roomW * 0.28), floorY + tallH, cz + Math.floor(roomW * 0.05) + tallW, MAT.boxTall);
  const shortW = Math.floor(roomW * 0.18), shortH = Math.floor(roomH * 0.28);
  box(cx + Math.floor(roomW * 0.10), floorY + 1, cz - Math.floor(roomW * 0.25),
      cx + Math.floor(roomW * 0.10) + shortW, floorY + shortH, cz - Math.floor(roomW * 0.25) + shortW, MAT.boxShort);

  // --- Trees ---------------------------------------------------------------
  const tree = (tx, tz) => {
    const g = groundAt(tx, tz);
    const trunkH = V(1.4) + ((tx * 31 + tz * 17) % V(0.8));
    for (let y = g + 1; y <= g + trunkH; y++) { set(tx, y, tz, MAT.trunk); set(tx + 1, y, tz, MAT.trunk); }
    const cy = g + trunkH + V(0.35);
    const r = V(0.7) + ((tx + tz) % 3);
    for (let dz = -r; dz <= r; dz++)
      for (let dy = -r; dy <= r; dy++)
        for (let dx = -r; dx <= r; dx++) {
          const d2 = dx * dx + dy * dy * 1.6 + dz * dz;
          if (d2 <= r * r && ((dx * 13 + dy * 7 + dz * 11) % 5 !== 0 || d2 < r * r * 0.5)) {
            set(tx + dx, cy + dy, tz + dz, MAT.leaf((dx * 3 + dy * 5 + dz * 7) % 12 + ((dx + dy + dz) % 2) * 6));
          }
        }
  };
  const treeSpots = [[0.15, 0.2], [0.82, 0.25], [0.2, 0.8], [0.86, 0.78], [0.12, 0.55], [0.65, 0.12]];
  for (const [fx, fz] of treeSpots) tree(Math.floor(N * fx), Math.floor(N * fz));

  // --- Lantern posts (colored emissive cubes: local GI sources) ------------
  const lantern = (lx, lz, mat) => {
    const g = groundAt(lx, lz);
    const h = V(1.1);
    for (let y = g + 1; y <= g + h; y++) set(lx, y, lz, MAT.lampPost);
    box(lx - 1, g + h + 1, lz - 1, lx + 1, g + h + 3, lz + 1, mat);
  };
  lantern(Math.floor(N * 0.35), Math.floor(N * 0.25), MAT.lampWarm);
  lantern(Math.floor(N * 0.68), Math.floor(N * 0.30), MAT.lampCyan);
  lantern(Math.floor(N * 0.25), Math.floor(N * 0.65), MAT.lampMagenta);

  // A warm lamp inside the room, tucked in a corner behind the tall box.
  box(x0 + 2, floorY + 1, z1 - 4, x0 + 4, floorY + 3, z1 - 2, MAT.lampWarm);

  // --- Brick occupancy map (built last so carved air is accounted for) -----
  const BG = N / BRICK;
  const bricks = new Uint32Array(BG * BG * BG);
  for (let bz = 0; bz < BG; bz++)
    for (let by = 0; by < BG; by++)
      for (let bx = 0; bx < BG; bx++) {
        let occ = 0;
        outer:
        for (let z = bz * BRICK; z < (bz + 1) * BRICK; z++)
          for (let y = by * BRICK; y < (by + 1) * BRICK; y++)
            for (let x = bx * BRICK; x < (bx + 1) * BRICK; x++)
              if (vox[idx(x, y, z)] !== 0) { occ = 1; break outer; }
        bricks[bx + by * BG + bz * BG * BG] = occ;
      }

  // --- Emissive face list (area lights for NEE / unified ReSTIR) -----------
  // Every exposed face of an emissive voxel becomes a sampleable area light:
  // word0 = x | y<<9 | z<<18 | face<<27 (face: 0..5 = +x,-x,+y,-y,+z,-z),
  // word1 = packed material. Requires N <= 512.
  const faceDirs = [[1, 0, 0], [-1, 0, 0], [0, 1, 0], [0, -1, 0], [0, 0, 1], [0, 0, -1]];
  const lightWords = [];
  for (let z = 0; z < N; z++)
    for (let y = 0; y < N; y++)
      for (let x = 0; x < N; x++) {
        const m = vox[idx(x, y, z)];
        if (((m >>> 24) & 0xff) === 0 || m === 0) continue;
        for (let f = 0; f < 6; f++) {
          const [dx, dy, dz] = faceDirs[f];
          const nx = x + dx, ny = y + dy, nz = z + dz;
          if (!inb(nx, ny, nz) || vox[idx(nx, ny, nz)] === 0) {
            lightWords.push((x | (y << 9) | (z << 18) | (f << 27)) >>> 0, m);
          }
        }
      }
  const lights = new Uint32Array(lightWords.length ? lightWords : [0, 0]);
  const lightCount = lightWords.length / 2;

  // Spawn just outside the doorway, eye height, facing the room (+Z).
  const spawn = {
    pos: [
      cx * VOXEL_SIZE,
      (groundAt(cx, z0 - V(3)) + 1) * VOXEL_SIZE + 1.65,
      (z0 - V(3.5)) * VOXEL_SIZE,
    ],
    yaw: 0,
    pitch: -0.05,
  };

  return { vox, bricks, spawn, worldM, lights, lightCount };
}
