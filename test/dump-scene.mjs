// Dump the browser scene to a binary fixture for the native traversal bench
// (docs/S64.md acceptance criterion 1: image identity on fixture256).
//
// Usage:
//   node test/dump-scene.mjs [--scene default|lamps] [--grid 256] [--out path]
//
// Imports the real generateScene() from src/scene.js (pure JS, no browser
// APIs), so the fixture is byte-for-byte the scene the browser renders with
// ?scene=<variant>&grid=<N>.
//
// ── File format: test/eval/fixture256.bin ──────────────────────────────────
//
//   offset  size          contents
//   ------  ------------  ---------------------------------------------------
//   0       4             magic: ASCII "VXF1" (bytes 0x56 0x58 0x46 0x31)
//   4       4             u32 LE  grid size N (256)
//   8       4             u32 LE  sceneFlags: 0 = default, 1 = lamps
//   12      4             u32 LE  reserved (0)
//   16      N*N*N * 4     voxel payload: N^3 little-endian u32 values in
//                         x + y*N + z*N*N order — identical indexing to
//                         voxelAt() in src/shaders/voxel.wgsl.
//
// Each voxel u32 is 0 for air, otherwise a packed material:
//   bits 0..7   red   (linear-ish byte)
//   bits 8..15  green
//   bits 16..23 blue
//   bits 24..31 emissive intensity e (0 = non-emissive)
//
// Total file size for N=256: 16 + 256^3*4 = 67,108,880 bytes.
// The dump is deterministic: two runs produce byte-identical files.

import { createHash } from 'node:crypto';
import { mkdirSync, writeFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { generateScene } from '../src/scene.js';

const argv = process.argv.slice(2);
const argVal = (name, dflt) => {
  const i = argv.indexOf(name);
  return i >= 0 && i + 1 < argv.length ? argv[i + 1] : dflt;
};

const variant = argVal('--scene', 'default');
const SCENE_FLAGS = { default: 0, lamps: 1 };
if (!(variant in SCENE_FLAGS)) {
  console.error(`unknown --scene "${variant}" (expected: ${Object.keys(SCENE_FLAGS).join(', ')})`);
  process.exit(1);
}
const N = parseInt(argVal('--grid', '256'), 10);
const here = dirname(fileURLToPath(import.meta.url));
const outPath = resolve(here, argVal('--out', 'eval/fixture256.bin'));

const { vox } = generateScene(N, variant);

// Header (all fields little-endian).
const header = Buffer.alloc(16);
header.write('VXF1', 0, 'ascii');
header.writeUInt32LE(N, 4);
header.writeUInt32LE(SCENE_FLAGS[variant], 8);
header.writeUInt32LE(0, 12);

// Payload: N^3 u32 LE in x + y*N + z*N*N order. generateScene() already
// stores vox in exactly this order; serialize explicitly little-endian so
// the dump is host-endianness-independent.
const payload = Buffer.alloc(vox.length * 4);
for (let i = 0; i < vox.length; i++) payload.writeUInt32LE(vox[i], i * 4);

mkdirSync(dirname(outPath), { recursive: true });
writeFileSync(outPath, Buffer.concat([header, payload]));

// Summary.
let solid = 0, emissive = 0;
for (let i = 0; i < vox.length; i++) {
  const m = vox[i];
  if (m !== 0) {
    solid++;
    if (((m >>> 24) & 0xff) !== 0) emissive++;
  }
}
const sha = createHash('sha256').update(payload).digest('hex');

console.log(`scene     : ${variant} (sceneFlags=${SCENE_FLAGS[variant]})`);
console.log(`grid      : ${N}^3 (${vox.length} voxels)`);
console.log(`solid     : ${solid} (${(100 * solid / vox.length).toFixed(2)}% occupancy)`);
console.log(`emissive  : ${emissive}`);
console.log(`payload   : ${payload.length} bytes, sha256 ${sha}`);
console.log(`file      : ${outPath} (${16 + payload.length} bytes)`);
