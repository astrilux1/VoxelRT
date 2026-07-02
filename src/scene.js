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
  // --- `lamps` variant palette (heterogeneous emitters) ---------------------
  // Per-face emitted luminance = lum(albedo^2) * (e/255) * EMISSIVE_SCALE(24):
  lampHot: M(255, 214, 150, 255),      // ~17.8 — small very bright warm point
  panelOrange: M(255, 140, 40, 200),   // ~8.1  — medium colored panel
  panelTeal: M(40, 220, 200, 165),     // ~9.0  — medium colored panel
  panelViolet: M(190, 80, 255, 245),   // ~6.0  — medium colored panel
  panelLime: M(150, 255, 60, 125),     // ~9.3  — medium colored panel
  dimStrip: M(140, 180, 255, 45),      // ~2.1  — large dim cool strip (~0.12x
                                       //         the default ceiling strip)
  pillar: M(118, 116, 126),
  shelf: M(140, 105, 70),
  // Dimmed exterior lanterns for the lamps variant: at full power the three
  // exterior lanterns hold ~30% of total emitted power while being invisible
  // from the interior poses, which would blunt the power-sampling signal the
  // variant exists to provide.
  lampWarmDim: M(255, 190, 110, 64),
  lampCyanDim: M(90, 220, 255, 64),
  lampMagentaDim: M(255, 90, 200, 64),
};

function emissiveFaceWeight(m) {
  const e = ((m >>> 24) & 0xff) / 255;
  if (e <= 0) return 0;
  const r = (m & 0xff) / 255;
  const g = ((m >>> 8) & 0xff) / 255;
  const b = ((m >>> 16) & 0xff) / 255;
  return e * (0.2126 * r * r + 0.7152 * g * g + 0.0722 * b * b);
}

function makeAliasTable(weights) {
  const n = weights.length;
  if (n === 0) return new Float32Array([1, 0, 1, 0]);

  const total = weights.reduce((s, w) => s + Math.max(0, w), 0);
  const prob = total > 0
    ? weights.map((w) => Math.max(0, w) / total)
    : weights.map(() => 1 / n);
  const scaled = prob.map((p) => p * n);
  const alias = new Uint32Array(n);
  const q = new Float32Array(n);
  const small = [];
  const large = [];

  for (let i = 0; i < n; i++) {
    if (scaled[i] < 1) small.push(i);
    else large.push(i);
  }

  while (small.length && large.length) {
    const s = small.pop();
    const l = large.pop();
    q[s] = scaled[s];
    alias[s] = l;
    scaled[l] = scaled[l] + scaled[s] - 1;
    if (scaled[l] < 1) small.push(l);
    else large.push(l);
  }

  for (const i of large) { q[i] = 1; alias[i] = i; }
  for (const i of small) { q[i] = 1; alias[i] = i; }

  const table = new Float32Array(n * 4);
  for (let i = 0; i < n; i++) {
    table[i * 4 + 0] = q[i];
    table[i * 4 + 1] = alias[i];
    table[i * 4 + 2] = prob[i] > 0 ? prob[i] : 1 / n;
    table[i * 4 + 3] = 0;
  }
  return table;
}

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

// variant:
//   'default' — the original scene (byte-identical to the pre-variant output).
//   'lamps'   — same world/terrain/room, but the uniform ceiling strip is
//               replaced by strongly heterogeneous emitters (a few tiny very
//               bright warm lamps, several medium colored panels, large dim
//               cool strips) plus occluder pillars/shelving, so that light
//               selection techniques (power sampling, light grids, adaptive
//               candidate budgets) have signal to exploit.
export function generateScene(N, variant = 'default') {
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

  if (variant === 'lamps') {
    // --- Heterogeneous interior lighting (all placements are pure room-
    // coordinate arithmetic: deterministic, no RNG) ------------------------

    // Large DIM cool strips: three long thin ceiling strips + one strip high
    // on the back wall. Individually weak (~0.12x the default strip's
    // per-face power) but they dominate the face *count*, so uniform face
    // sampling wastes most of its budget here.
    const stripLen = Math.floor(roomW * 0.5);
    const sx0 = cx - (stripLen >> 1), sx1 = sx0 + stripLen - 1;
    const stripDz = Math.floor(roomW * 0.22);
    for (const dz of [-stripDz, 0, stripDz]) {
      box(sx0, y1, cz + dz - 1, sx1, y1, cz + dz + 1, MAT.dimStrip);
    }
    box(sx0, y1 - 4, z1, sx1, y1 - 3, z1, MAT.dimStrip);          // back wall

    // Medium colored panels, one per wall, distinct hues (chroma noise is
    // measurable per-panel). Each sits one voxel proud of its wall. Kept
    // small (6x4) so their face-count share stays well below their power
    // share — that asymmetry is what power sampling exploits.
    const pW = 5, pH = 3;                                          // 6x4 voxels
    const pMidY = floorY + Math.floor(roomH * 0.35);
    box(x0, pMidY, cz - 9, x0, pMidY + pH, cz - 9 + pW, MAT.panelOrange);          // left (red) wall
    box(x1, floorY + Math.floor(roomH * 0.2), cz - 22,
        x1, floorY + Math.floor(roomH * 0.2) + pH, cz - 22 + pW, MAT.panelTeal);   // right wall, below window
    box(cx + 10, floorY + Math.floor(roomH * 0.5), z1,
        cx + 10 + pW, floorY + Math.floor(roomH * 0.5) + pH, z1, MAT.panelViolet); // back wall
    box(x0 + 6, floorY + Math.floor(roomH * 0.4), z0,
        x0 + 6 + pW, floorY + Math.floor(roomH * 0.4) + pH, z0, MAT.panelLime);    // front wall, beside doorway

    // Occluders first, so lamps can hide behind/under them: two pillars and
    // a shelf on the right wall — DI visibility varies sharply per receiver.
    const pAx = cx - Math.floor(roomW * 0.12), pAz = cz + Math.floor(roomW * 0.15);
    box(pAx - 1, floorY + 1, pAz - 1, pAx + 2, y1, pAz + 2, MAT.pillar);
    const pBx = cx + Math.floor(roomW * 0.22), pBz = cz - Math.floor(roomW * 0.12);
    box(pBx - 1, floorY + 1, pBz - 1, pBx + 2, floorY + Math.floor(roomH * 0.75), pBz + 2, MAT.pillar);
    const shelfY = floorY + Math.floor(roomH * 0.3);
    box(x1 - 10, shelfY, cz + 10, x1 - 1, shelfY + 1, cz + 26, MAT.shelf);

    // Small VERY bright warm lamps (1-2 voxels, e=255 — the encoding's max;
    // ~8.5x the dim strips' per-face power). Spread spatially with varied
    // occlusion: ceiling pendants, an under-shelf lamp, floor lamps tucked
    // behind the pillars, wall lamps front and back.
    const pendant = (lx, lz) => {
      for (let y = y1 - 2; y <= y1; y++) set(lx, y, lz, MAT.lampPost);
      box(lx, y1 - 4, lz, lx, y1 - 3, lz, MAT.lampHot);
    };
    pendant(cx + Math.floor(roomW * 0.19), cz - Math.floor(roomW * 0.16)); // over short box
    pendant(cx - Math.floor(roomW * 0.3), cz + Math.floor(roomW * 0.3));   // back-left quadrant
    const standing = (lx, lz) => {
      set(lx, floorY + 1, lz, MAT.lampPost);
      box(lx, floorY + 2, lz, lx, floorY + 3, lz, MAT.lampHot);
    };
    standing(x1 - 5, cz + 18);                                     // under shelf
    standing(pAx - 4, z1 - 4);                                     // behind pillar A (from doorway)
    standing(pBx + 4, pBz - 4);                                    // beside pillar B
    standing(x0 + 3, z1 - 3);                                      // back-left corner
    box(cx - 24, y1 - 6, z0, cx - 24, y1 - 5, z0, MAT.lampHot);    // front wall, high
    box(cx - 6, floorY + 8, z1, cx - 6, floorY + 9, z1, MAT.lampHot); // back wall, low
  } else {
    // Ceiling light strip (area light).
    const lHalf = Math.floor(roomW * 0.18);
    box(cx - lHalf, y1, cz - lHalf, cx + lHalf, y1, cz + lHalf, MAT.ceilLight);
  }

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
  const lamps = variant === 'lamps';
  lantern(Math.floor(N * 0.35), Math.floor(N * 0.25), lamps ? MAT.lampWarmDim : MAT.lampWarm);
  lantern(Math.floor(N * 0.68), Math.floor(N * 0.30), lamps ? MAT.lampCyanDim : MAT.lampCyan);
  lantern(Math.floor(N * 0.25), Math.floor(N * 0.65), lamps ? MAT.lampMagentaDim : MAT.lampMagenta);

  // A warm lamp inside the room, tucked in a corner behind the tall box (the
  // lamps variant puts one of its small bright standing lamps there instead).
  if (!lamps) box(x0 + 2, floorY + 1, z1 - 4, x0 + 4, floorY + 3, z1 - 2, MAT.lampWarm);

  // --- Brick occupancy map (built last so carved air is accounted for) -----
  const BG = N / BRICK;
  const bricks = new Uint32Array(BG * BG * BG);
  const brickMasks = new Uint32Array(BG * BG * BG * 16);
  for (let bz = 0; bz < BG; bz++)
    for (let by = 0; by < BG; by++)
      for (let bx = 0; bx < BG; bx++) {
        let occ = 0;
        const bi = bx + by * BG + bz * BG * BG;
        const maskBase = bi * 16;
        for (let z = bz * BRICK; z < (bz + 1) * BRICK; z++)
          for (let y = by * BRICK; y < (by + 1) * BRICK; y++)
            for (let x = bx * BRICK; x < (bx + 1) * BRICK; x++)
              if (vox[idx(x, y, z)] !== 0) {
                occ = 1;
                const lx = x - bx * BRICK;
                const ly = y - by * BRICK;
                const lz = z - bz * BRICK;
                const bit = lx + ly * BRICK + lz * BRICK * BRICK;
                brickMasks[maskBase + (bit >> 5)] |= (1 << (bit & 31)) >>> 0;
              }
        bricks[bi] = occ;
      }

  // --- Emissive face list (area lights for NEE / unified ReSTIR) -----------
  // Every exposed face of an emissive voxel becomes a sampleable area light:
  // word0 = x | y<<9 | z<<18 | face<<27 (face: 0..5 = +x,-x,+y,-y,+z,-z),
  // word1 = packed material. Requires N <= 512.
  const faceDirs = [[1, 0, 0], [-1, 0, 0], [0, 1, 0], [0, -1, 0], [0, 0, 1], [0, 0, -1]];
  const lightWords = [];
  const lightWeights = [];
  for (let z = 0; z < N; z++)
    for (let y = 0; y < N; y++)
      for (let x = 0; x < N; x++) {
        const m = vox[idx(x, y, z)];
        if (((m >>> 24) & 0xff) === 0 || m === 0) continue;
        const w = emissiveFaceWeight(m);
        for (let f = 0; f < 6; f++) {
          const [dx, dy, dz] = faceDirs[f];
          const nx = x + dx, ny = y + dy, nz = z + dz;
          if (!inb(nx, ny, nz) || vox[idx(nx, ny, nz)] === 0) {
            lightWords.push((x | (y << 9) | (z << 18) | (f << 27)) >>> 0, m);
            lightWeights.push(w);
          }
        }
      }
  const lights = new Uint32Array(lightWords.length ? lightWords : [0, 0]);
  const lightCount = lightWords.length / 2;
  const lightAlias = makeAliasTable(lightWeights);

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

  return { vox, bricks, brickMasks, spawn, worldM, lights, lightCount, lightAlias };
}
