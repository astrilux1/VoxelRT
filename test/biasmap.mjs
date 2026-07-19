// Seed-averaged bias maps (docs/BIASMAP.md) — analysis half.
//
// Consumes the per-repeat linear-HDR dumps that `bench.mjs --repeats N`
// leaves in test/eval/runs/ plus the frozen reference, and produces the
// pre-registered artifacts:
//   (a) mean image over N seeds (PNG, tonemapped like the bench preview)
//   (b) signed relative residual map (mean - ref)/max(ref, eps) on a fixed
//       +-10% color scale (blue = dark bias, red = bright bias)
//   (c) stats: per-channel mean relative delta, RMS relative residual,
//       p99 |relative residual|, fraction of pixels beyond +-2%, and the
//       noise-consistency check: observed residual RMS vs the sqrt(N)
//       prediction from the per-pixel stddev across runs (BIASMAP.md §3.3:
//       observed <= 3x predicted or the harness/estimator is suspect).
//
// Usage:
//   node test/biasmap.mjs --scenario interior_static --config ours \
//        --frames 32 --repeats 64 [--ref <path>] [--out-stem name]
//
// Accumulation is float64 Welford (mean + M2) so 64 runs lose no precision.

import { readFileSync, writeFileSync, existsSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import zlib from 'node:zlib';
import { getArg } from './gpu-launch.mjs';

const ROOT = fileURLToPath(new URL('..', import.meta.url));
const EVALDIR = join(ROOT, 'test', 'eval');
const args = process.argv.slice(2);
const scenario = getArg(args, '--scenario', 'interior_static');
const config = getArg(args, '--config', 'ours');
const frames = parseInt(getArg(args, '--frames', '32'), 10);
const repeats = parseInt(getArg(args, '--repeats', '64'), 10);
const width = parseInt(getArg(args, '--width', '1920'), 10);
const height = parseInt(getArg(args, '--height', '1080'), 10);
const outStem = getArg(args, '--out-stem', `biasmap_${scenario}_${config}_${frames}f_x${repeats}`);
const refPath = getArg(
  args,
  '--ref',
  join(EVALDIR, 'refs', `${scenario}_${width}x${height}_s1_rb12_rf1600${scenario.endsWith('_move') ? `_at${frames}f` : ''}.rgbf32`),
);

const npix = width * height;
const nch = npix * 3;

function loadF32(path, what) {
  if (!existsSync(path)) {
    console.error(`missing ${what}: ${path}`);
    process.exit(2);
  }
  const buf = readFileSync(path);
  if (buf.length !== nch * 4) {
    console.error(`${what} has ${buf.length} bytes, expected ${nch * 4}`);
    process.exit(2);
  }
  return new Float32Array(buf.buffer, buf.byteOffset, nch);
}

// --- Welford accumulation over the N per-seed runs ---------------------------
const mean = new Float64Array(nch);
const m2 = new Float64Array(nch);
let loaded = 0;
for (let r = 0; r < repeats; r++) {
  const p = join(EVALDIR, 'runs', `${scenario}_${config}_${frames}f_r${r}_${width}x${height}.rgbf32`);
  if (!existsSync(p)) {
    console.error(`missing run ${r}: ${p}`);
    process.exit(2);
  }
  const x = loadF32(p, `run r${r}`);
  loaded++;
  for (let i = 0; i < nch; i++) {
    const d = x[i] - mean[i];
    mean[i] += d / loaded;
    m2[i] += d * (x[i] - mean[i]);
  }
  if (r % 16 === 15) console.error(`  accumulated ${loaded}/${repeats}`);
}
const ref = loadF32(refPath, 'reference');

// --- Stats -------------------------------------------------------------------
const EPS = 1e-4;
let refSum = [0, 0, 0];
let meanSum = [0, 0, 0];
let residSq = 0;
let predSq = 0;
const absResid = new Float64Array(npix); // per-pixel max-channel |relative residual|
for (let i = 0; i < nch; i++) {
  const c = i % 3;
  refSum[c] += ref[i];
  meanSum[c] += mean[i];
  const rel = (mean[i] - ref[i]) / Math.max(ref[i], EPS);
  residSq += rel * rel;
  const variance = loaded > 1 ? m2[i] / (loaded - 1) : 0;
  const predicted = Math.sqrt(variance / loaded) / Math.max(ref[i], EPS);
  predSq += predicted * predicted;
  const pi = (i / 3) | 0;
  const a = Math.abs(rel);
  if (a > absResid[pi]) absResid[pi] = a;
}
const channelDelta = [0, 1, 2].map((c) => (meanSum[c] - refSum[c]) / Math.max(refSum[c], EPS));
const rmsResid = Math.sqrt(residSq / nch);
const rmsPredicted = Math.sqrt(predSq / nch);
const sortedAbs = Float64Array.from(absResid).sort();
const p99 = sortedAbs[Math.floor(0.99 * (npix - 1))];
const beyond2pct = absResid.reduce((n, v) => n + (v > 0.02 ? 1 : 0), 0) / npix;
const noiseRatio = rmsResid / Math.max(rmsPredicted, 1e-12);

// --- Residual PNG (+-10% fixed scale) + mean PNG -----------------------------
function pngWrite(path, rgba) {
  // Minimal PNG encoder (RGBA8, no filter) to avoid new dependencies.
  const raw = Buffer.alloc((width * 4 + 1) * height);
  for (let y = 0; y < height; y++) {
    raw[y * (width * 4 + 1)] = 0;
    rgba.copy(raw, y * (width * 4 + 1) + 1, y * width * 4, (y + 1) * width * 4);
  }
  const idat = zlib.deflateSync(raw, { level: 6 });
  const chunks = [];
  const chunk = (type, data) => {
    const c = Buffer.alloc(8 + data.length + 4);
    c.writeUInt32BE(data.length, 0);
    c.write(type, 4);
    data.copy(c, 8);
    const crcBuf = Buffer.concat([Buffer.from(type), data]);
    c.writeUInt32BE(crc32(crcBuf), 8 + data.length);
    return c;
  };
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0);
  ihdr.writeUInt32BE(height, 4);
  ihdr[8] = 8; ihdr[9] = 6; // 8-bit RGBA
  chunks.push(Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]));
  chunks.push(chunk('IHDR', ihdr));
  chunks.push(chunk('IDAT', idat));
  chunks.push(chunk('IEND', Buffer.alloc(0)));
  writeFileSync(path, Buffer.concat(chunks));
}
let crcTable = null;
function crc32(buf) {
  if (!crcTable) {
    crcTable = new Int32Array(256);
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
      crcTable[n] = c;
    }
  }
  let c = ~0;
  for (const b of buf) c = crcTable[(c ^ b) & 0xff] ^ (c >>> 8);
  return ~c >>> 0;
}

const residPng = Buffer.alloc(npix * 4);
const meanPng = Buffer.alloc(npix * 4);
for (let p = 0; p < npix; p++) {
  // Residual: signed mean-channel relative residual mapped to blue<->red.
  let rel = 0;
  for (let c = 0; c < 3; c++) {
    const i = p * 3 + c;
    rel += (mean[i] - ref[i]) / Math.max(ref[i], EPS);
  }
  rel /= 3;
  const t = Math.max(-1, Math.min(1, rel / 0.10)); // +-10% full scale
  residPng[p * 4] = t > 0 ? Math.round(255 * t) : 0;
  residPng[p * 4 + 1] = 0;
  residPng[p * 4 + 2] = t < 0 ? Math.round(-255 * t) : 0;
  residPng[p * 4 + 3] = 255;
  for (let c = 0; c < 3; c++) {
    const v = mean[p * 3 + c];
    meanPng[p * 4 + c] = Math.round(255 * Math.min(1, Math.sqrt(v / (1 + v)))); // simple tonemap
  }
  meanPng[p * 4 + 3] = 255;
}
pngWrite(join(EVALDIR, `${outStem}_residual.png`), residPng);
pngWrite(join(EVALDIR, `${outStem}_mean.png`), meanPng);

const report = {
  scenario, config, frames, repeats: loaded, reference: refPath,
  channelMeanRelativeDelta: channelDelta,
  maxChannelMeanRelativeDelta: Math.max(...channelDelta.map(Math.abs)),
  rmsRelativeResidual: rmsResid,
  p99AbsRelativeResidual: p99,
  fractionBeyond2pct: beyond2pct,
  noiseConsistency: { observedRms: rmsResid, predictedRms: rmsPredicted, ratio: noiseRatio, gate3x: noiseRatio <= 3 },
};
writeFileSync(join(EVALDIR, `${outStem}.json`), JSON.stringify(report, null, 2));
console.log(JSON.stringify(report, null, 2));
console.error(`wrote ${outStem}{.json,_residual.png,_mean.png} in test/eval/`);
