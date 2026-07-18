// Self-inverting Gaussian pairing textures for paired spatial reuse
// (Lin 2026 §3). Pixels are filled with link indices so every index appears
// exactly twice side by side, then shuffled with n_sigma alternating tiled
// 2x2 permutations; pixels sharing an index become mutual reuse partners
// whose coordinate deltas follow ~N(0, sigma). Deltas are wrapped to the
// texture size so the texture tiles, and packed as two i16s per texel.

// Deterministic PRNG so renders are reproducible run to run.
function mulberry32(seed) {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

// Repeat count for a target standard deviation (Lin 2026 Eq. 3).
function shuffleCount(sigma) {
  return Math.max(1, Math.floor(
    (sigma * sigma) / 2 + 1.46 / sigma + 1.76 / (sigma * sigma) +
    0.656 / (sigma ** 3) + 0.5));
}

export function makePairingTexture(S, sigma, seed) {
  const rand = mulberry32(seed);
  const link = new Int32Array(S * S);
  for (let y = 0; y < S; y++)
    for (let x = 0; x < S; x++) link[y * S + x] = (x >> 1) + (S >> 1) * y;

  const n = shuffleCount(sigma);
  const cell = new Int32Array(4);
  for (let it = 0; it < n; it++) {
    const o = it & 1;   // every other pass offset diagonally by one (wrapping)
    for (let by = 0; by < S; by += 2) {
      for (let bx = 0; bx < S; bx += 2) {
        const xs = [(bx + o) % S, (bx + o + 1) % S];
        const ys = [(by + o) % S, (by + o + 1) % S];
        cell[0] = ys[0] * S + xs[0]; cell[1] = ys[0] * S + xs[1];
        cell[2] = ys[1] * S + xs[0]; cell[3] = ys[1] * S + xs[1];
        // Fisher-Yates over the 4 cells.
        for (let i = 3; i > 0; i--) {
          const j = (rand() * (i + 1)) | 0;
          const t = link[cell[i]]; link[cell[i]] = link[cell[j]]; link[cell[j]] = t;
        }
      }
    }
  }

  // Pair up matching link indices and store wrapped coordinate deltas.
  const first = new Int32Array((S * S) >> 1).fill(-1);
  const out = new Uint32Array(S * S);
  const wrap = (d) => {
    let w = d % S;
    if (w > S / 2) w -= S;
    if (w < -S / 2) w += S;
    return w;
  };
  for (let i = 0; i < S * S; i++) {
    const l = link[i];
    if (first[l] < 0) { first[l] = i; continue; }
    const a = first[l], b = i;
    const dx = wrap((b % S) - (a % S));
    const dy = wrap(((b / S) | 0) - ((a / S) | 0));
    out[a] = (((dy & 0xffff) << 16) | (dx & 0xffff)) >>> 0;
    out[b] = (((-dy & 0xffff) << 16) | (-dx & 0xffff)) >>> 0;
  }
  return out;
}

// The three texture sizes are near-coprime so tiling periods don't align
// (Lin 2026 footnote 3); must match pairSize()/pairOff() in reuse_spatial.wgsl.
export const PAIRING_SIZES = [254, 230, 210];

// `sigmas` holds one σ per PAIRING_SIZES entry so a single tap can be built
// wider than the others (RF_MIXSIGMA). A σ whose Gaussian tail exceeds ±S/2
// simply folds back through the delta wrap above — the construction tolerates
// this (pairs stay mutual and self-inverting; only the far tail tiles).
export function makePairingBuffer(sigmas) {
  const total = PAIRING_SIZES.reduce((a, s) => a + s * s, 0);
  const buf = new Uint32Array(total);
  let off = 0;
  PAIRING_SIZES.forEach((S, i) => {
    buf.set(makePairingTexture(S, sigmas[i], 0xc0ffee + i * 7919), off);
    off += S * S;
  });
  return buf;
}
