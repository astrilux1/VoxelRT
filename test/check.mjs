// Non-GPU repository gate: `npm run check`.
//
// The real gates (bench:correctness, bench:references, the claim suite) need
// the locked RTX 3080 path, so they only run on the benchmark machine. This
// script is the cheap complement that can run on any checkout — CI included —
// and catches the drift that would otherwise wait for the next full campaign:
//
//   1. WGSL structural sanity (balanced delimiters, required entry points,
//      no merge-conflict markers, no stray compute entry in library files)
//   2. Host/shader contract: RF flag bits in src/main.js match common.wgsl,
//      every RF_* used in a shader is declared, preset flags resolve, the
//      host's uniform buffer size matches the WGSL Uniforms struct layout,
//      and the string-replacement anchors main.js depends on still exist
//   3. Benchmark configs in test/bench.mjs reference real presets and knobs
//   4. Claim + correctness manifests parse, validate, and agree
//   5. Checked-in evidence reports are bound to the current manifest hashes
//      (a drifted manifest silently orphans evidence; fail closed instead)
//
// This is a structural gate, not a compiler: it cannot type-check WGSL. Real
// shader compilation is still exercised by `npm test` on a GPU machine.

import { readFile } from 'node:fs/promises';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { canonicalBytes, claimIdentity, loadClaimManifest } from './claim-manifest.mjs';
import { correctnessClaimProblems, loadCorrectnessManifest } from './correctness-manifest.mjs';

const ROOT = fileURLToPath(new URL('..', import.meta.url));
const SHADERDIR = join(ROOT, 'src', 'shaders');
const EVALDIR = join(ROOT, 'test', 'eval');

const problems = [];
const fail = (msg) => problems.push(msg);

// Compute-entry shaders get `fn main` + @compute; libraries are concatenated
// ahead of them by main.js and must not carry their own entry point.
const COMPUTE_SHADERS = ['pathtrace.wgsl', 'reuse_temporal.wgsl', 'reuse_spatial.wgsl',
  'dupmap.wgsl', 'temporal.wgsl', 'atrous.wgsl'];
const LIBRARY_SHADERS = ['common.wgsl', 'restir.wgsl', 'voxel.wgsl'];
const RENDER_SHADERS = ['present.wgsl'];

function stripWgslComments(src) {
  return src.replace(/\/\*[\s\S]*?\*\//g, ' ').replace(/\/\/[^\n]*/g, '');
}

function checkDelimiters(name, src) {
  for (const [open, close] of [['{', '}'], ['(', ')'], ['[', ']']]) {
    let depth = 0;
    for (const ch of src) {
      if (ch === open) depth++;
      else if (ch === close && --depth < 0) break;
    }
    if (depth !== 0) fail(`${name}: unbalanced '${open}${close}' (depth ${depth} at EOF)`);
  }
}

const shaders = {};
for (const name of [...COMPUTE_SHADERS, ...LIBRARY_SHADERS, ...RENDER_SHADERS]) {
  const raw = await readFile(join(SHADERDIR, name), 'utf8');
  if (/^(<{7}|={7}|>{7})/m.test(raw)) fail(`${name}: merge-conflict marker`);
  const src = stripWgslComments(raw);
  checkDelimiters(name, src);
  shaders[name] = src;
}
for (const name of COMPUTE_SHADERS) {
  if (!/@compute/.test(shaders[name])) fail(`${name}: missing @compute`);
  if (!/fn\s+main\s*\(/.test(shaders[name])) fail(`${name}: missing entry point 'fn main'`);
}
for (const name of LIBRARY_SHADERS) {
  if (/@compute/.test(shaders[name])) fail(`${name}: library files must not declare @compute entries`);
}
if (!/@vertex/.test(shaders['present.wgsl']) || !/fn\s+vsMain\s*\(/.test(shaders['present.wgsl'])) {
  fail('present.wgsl: missing @vertex vsMain');
}
if (!/@fragment/.test(shaders['present.wgsl']) || !/fn\s+fsMain\s*\(/.test(shaders['present.wgsl'])) {
  fail('present.wgsl: missing @fragment fsMain');
}

// --- Host/shader flag contract ----------------------------------------------

const mainSrc = await readFile(join(ROOT, 'src', 'main.js'), 'utf8');
const common = shaders['common.wgsl'];

const rfBlock = /const RF = \{([\s\S]*?)\};/.exec(mainSrc)?.[1];
if (!rfBlock) fail('src/main.js: could not locate the `const RF = {...}` flag map');
const hostFlags = new Map();
for (const m of (rfBlock ?? '').matchAll(/(\w+)\s*:\s*(\d+)/g)) {
  hostFlags.set(m[1], Number(m[2]));
}
const wgslFlags = new Map();
for (const m of common.matchAll(/const\s+(RF_\w+)\s*:\s*u32\s*=\s*(\d+)u/g)) {
  wgslFlags.set(m[1], Number(m[2]));
}
if (hostFlags.size === 0) fail('src/main.js: RF flag map is empty');
if (wgslFlags.size === 0) fail('common.wgsl: no RF_* flag constants found');
const hostBits = [...hostFlags.values()].sort((a, b) => a - b);
const wgslBits = [...wgslFlags.values()].sort((a, b) => a - b);
for (const [where, bits] of [['src/main.js RF', hostBits], ['common.wgsl RF_*', wgslBits]]) {
  if (new Set(bits).size !== bits.length) fail(`${where}: duplicate flag bits`);
  for (const bit of bits) {
    if (bit <= 0 || (bit & (bit - 1)) !== 0) fail(`${where}: ${bit} is not a power of two`);
  }
}
// Compare the two sides *per flag*, not as bit sets: a permuted mapping
// (host `treuse: 4` against `RF_SPATIAL = 4u`) leaves both sorted bit lists
// identical while every preset silently enables the wrong shader feature.
// The host keys and the WGSL names differ for three flags, so spell those out
// and derive the rest as RF_<UPPERCASE>.
const HOST_TO_WGSL = { treuse: 'RF_TEMPORAL', sreuse: 'RF_SPATIAL', rclamp: 'RF_CLAMP' };
const wgslNameFor = (host) => HOST_TO_WGSL[host] ?? `RF_${host.toUpperCase()}`;
const mappedWgslNames = new Set();
for (const [host, bit] of hostFlags) {
  const wgslName = wgslNameFor(host);
  mappedWgslNames.add(wgslName);
  if (!wgslFlags.has(wgslName)) {
    fail(`src/main.js RF.${host} has no counterpart ${wgslName} in common.wgsl`);
  } else if (wgslFlags.get(wgslName) !== bit) {
    fail(`flag bit mismatch: main.js RF.${host} = ${bit} but ` +
      `common.wgsl ${wgslName} = ${wgslFlags.get(wgslName)}`);
  }
}
for (const name of wgslFlags.keys()) {
  if (!mappedWgslNames.has(name)) fail(`common.wgsl ${name} has no counterpart in the src/main.js RF map`);
}
// Every RF_* referenced by any shader must be declared in common.wgsl.
for (const [name, src] of Object.entries(shaders)) {
  for (const m of src.matchAll(/\bRF_[A-Z0-9_]+\b/g)) {
    if (!wgslFlags.has(m[0])) fail(`${name}: ${m[0]} is not declared in common.wgsl`);
  }
}

// Preset flag lists in main.js must resolve against the RF map.
const presetBlock = /const PRESETS = \{([\s\S]*?)\n\};/.exec(mainSrc)?.[1];
if (!presetBlock) fail('src/main.js: could not locate the PRESETS map');
const presets = new Set();
for (const m of (presetBlock ?? '').matchAll(/^\s*(\w+)\s*:\s*\[([\s\S]*?)\]/gm)) {
  presets.add(m[1]);
  for (const f of m[2].matchAll(/'(\w+)'/g)) {
    if (!hostFlags.has(f[1])) fail(`preset ${m[1]}: unknown flag '${f[1]}'`);
  }
}
if (presets.size === 0) fail('src/main.js: no presets parsed');

// String-replacement anchors: main.js patches these exact substrings before
// compiling; if a shader edit renames them the patch silently stops applying.
if (!shaders['pathtrace.wgsl'].includes('FCLAMP_VALUE')) {
  fail('pathtrace.wgsl: FCLAMP_VALUE placeholder is gone but main.js still substitutes it');
}
if (!shaders['temporal.wgsl'].includes('texture_storage_2d<rgba16float, write>')) {
  fail('temporal.wgsl: the rgba16float storage declaration main.js rewrites for fp32 accumulation is gone');
}

// Uniform buffer size: the host allocates a fixed byte count; recompute the
// WGSL struct size (mat4x4 = 64 B, vec4 = 16 B, both 16-aligned so packing is
// exact) and require agreement with every host-side literal.
const structBlock = /struct Uniforms \{([\s\S]*?)\n\};/.exec(common)?.[1];
if (!structBlock) fail('common.wgsl: could not locate struct Uniforms');
else {
  const mats = (structBlock.match(/:\s*mat4x4<f32>/g) || []).length;
  const vecs = (structBlock.match(/:\s*vec4</g) || []).length;
  const structSize = mats * 64 + vecs * 16;
  const hostSizes = [
    /label: 'uniforms', size: (\d+)/.exec(mainSrc)?.[1],
    /new ArrayBuffer\((\d+)\)/.exec(mainSrc)?.[1],
  ].map(Number);
  for (const size of hostSizes) {
    if (size !== structSize) {
      fail(`uniform size mismatch: WGSL Uniforms is ${structSize} B ` +
        `(${mats} mat4x4 + ${vecs} vec4), host allocates ${size} B`);
    }
  }
}

// --- Benchmark configuration queries ----------------------------------------
// Every preset=... query in bench.mjs must reference a real preset, and every
// query key must be a knob the renderer actually reads.
const benchSrc = await readFile(join(ROOT, 'test', 'bench.mjs'), 'utf8');
const knownParams = new Set([
  ...hostFlags.keys(),
  // tuning knobs + top-level URL params read in src/main.js
  'preset', 'fclamp', 'scale', 'bounces', 'denoise', 'scene', 'grid', 'temporal',
  'nocanvas', 'benchmove', 'stopat', 'timing', 'timewarmup',
  'px', 'py', 'pz', 'yaw', 'pitch',
  'taps', 'sigma', 'radius', 'ccap', 'capmin', 'dupalpha', 'fpc', 'rclampv',
  'maxhist', 'fseed', 'sigma2', 'candscale', 'confk', 'mutscale', 'gridcand', 'wgicap',
]);
for (const m of benchSrc.matchAll(/'(preset=[^']+)'/g)) {
  const q = new URLSearchParams(m[1]);
  if (!presets.has(q.get('preset'))) {
    fail(`bench.mjs config '${m[1]}': unknown preset '${q.get('preset')}'`);
  }
  for (const key of q.keys()) {
    if (!knownParams.has(key)) fail(`bench.mjs config '${m[1]}': unknown knob '${key}'`);
  }
}

// --- Manifests and evidence binding ------------------------------------------

let claim = null;
try {
  claim = await loadClaimManifest(join(EVALDIR, 'claim-manifest.v1.json'));
} catch (e) {
  fail(`claim manifest: ${e.message}`);
}
let corr = null;
try {
  corr = await loadCorrectnessManifest(join(EVALDIR, 'correctness-manifest.v1.json'));
} catch (e) {
  fail(`correctness manifest: ${e.message}`);
}
if (claim && corr) {
  for (const p of correctnessClaimProblems(corr.data, claim.data, claimIdentity(claim))) {
    fail(`correctness/claim manifest disagreement: ${p}`);
  }
  // Frozen claim configurations must themselves resolve against the renderer.
  for (const [name, query] of Object.entries(claim.data.configurations)) {
    const q = new URLSearchParams(query);
    if (!presets.has(q.get('preset'))) fail(`claim configuration ${name}: unknown preset '${q.get('preset')}'`);
    for (const key of q.keys()) {
      if (!knownParams.has(key)) fail(`claim configuration ${name}: unknown knob '${key}'`);
    }
  }
}

// Checked-in evidence must be bound to the *current* manifest bytes. A manifest
// edit that forgets to re-run the gates would otherwise leave stale evidence
// looking authoritative.
async function reportJson(name) {
  return JSON.parse(canonicalBytes(await readFile(join(EVALDIR, name))).toString('utf8'));
}
if (claim && corr) {
  try {
    const report = await reportJson('estimator-correctness.v1.json');
    if (report.claimManifest?.sha256 !== claim.sha256) {
      fail('estimator-correctness.v1.json is bound to a stale claim manifest; re-run bench:correctness');
    }
    if (report.correctnessManifest?.sha256 !== corr.sha256) {
      fail('estimator-correctness.v1.json is bound to a stale correctness manifest; re-run bench:correctness');
    }
    if (report.pass !== true) fail('estimator-correctness.v1.json: checked-in gate is failing');
  } catch (e) {
    fail(`estimator-correctness.v1.json: ${e.message}`);
  }
  try {
    const report = await reportJson('reference-convergence.v1.json');
    if (report.claimManifest?.sha256 !== claim.sha256) {
      fail('reference-convergence.v1.json is bound to a stale claim manifest; re-run bench:references');
    }
    if (report.pass !== true) fail('reference-convergence.v1.json: checked-in gate is failing');
  } catch (e) {
    fail(`reference-convergence.v1.json: ${e.message}`);
  }
}

// --- Result -------------------------------------------------------------------

if (problems.length) {
  console.error(`check FAILED (${problems.length} problem${problems.length === 1 ? '' : 's'}):`);
  for (const p of problems) console.error(`- ${p}`);
  process.exit(1);
}
console.log('check passed: shaders structurally sane, host/shader flag and uniform ' +
  'contracts agree, bench configs resolve, manifests valid, evidence bound to current hashes.');
