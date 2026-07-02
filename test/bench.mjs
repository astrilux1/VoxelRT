// Benchmark harness: renders configurable pipeline presets headlessly
// (SwiftShader WebGPU) and scores them against converged path-traced
// references with PSNR, mirroring the evaluation protocol of Lin 2026.
//
// Usage:
//   node test/bench.mjs                  # full comparison table
//   node test/bench.mjs --refs           # (re)build reference images only
//   node test/bench.mjs --configs lin,ours --scenarios interior_move
//   node test/bench.mjs --shot 'preset=ours&px=8' --out /tmp/x.png
//
// References are cached under test/refs/ as raw RGB; delete to regenerate.

import { chromium } from 'playwright';
import http from 'node:http';
import { readFile, writeFile, mkdir } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import { extname, join, normalize } from 'node:path';

const ROOT = new URL('..', import.meta.url).pathname;
const REFDIR = join(ROOT, 'test', 'refs');
const args = process.argv.slice(2);
const getArg = (name, dflt) => {
  const i = args.indexOf(name);
  return i >= 0 ? args[i + 1] : dflt;
};

// --- Scenarios --------------------------------------------------------------
// interior: inside the Cornell-style room; illumination dominated by the
//   emissive ceiling strip, the corner lamp and sun shafts — the hard case.
// exterior: sun + sky + lantern GI over terrain from the spawn point.
// *_move: same poses with a deterministic ±0.6 m camera strafe; scores the
//   image *while the camera moves* (temporal reuse, disocclusion, ghosting).
const POSES = {
  interior: { px: 8.8, py: 4.7, pz: 7.0, yaw: -0.45, pitch: 0.02 },
  exterior: { px: 8.0, py: 4.9, pz: 4.6, yaw: 0.85, pitch: -0.10 },
};
const MOVE_AMP = 0.6;
const MOVE_FRAMES = 96;   // capture pose: sin((96-1)*0.07)*amp
const STATIC_FRAMES = 32; // capture after 32 frames of convergence

const SCENARIOS = {
  interior_static: { pose: POSES.interior, frames: STATIC_FRAMES },
  interior_move: { pose: POSES.interior, frames: MOVE_FRAMES, move: MOVE_AMP },
  exterior_static: { pose: POSES.exterior, frames: STATIC_FRAMES },
  exterior_move: { pose: POSES.exterior, frames: MOVE_FRAMES, move: MOVE_AMP },
};

// Pipeline configurations under test (see PRESETS in src/main.js).
const CONFIGS = {
  base: 'preset=base',
  gi: 'preset=gi',
  lin: 'preset=lin',
  ours: 'preset=ours',
};

const REF_FRAMES = parseInt(getArg('--ref-frames', '1600'), 10);
const VIEW = { width: 480, height: 270 };   // 0.5 scale -> 240x135 internal
const BASEQ = 'scale=0.5&bounces=2&nocanvas=1';

// --- Static file server + browser -------------------------------------------
const MIME = { '.html': 'text/html', '.js': 'text/javascript', '.wgsl': 'text/plain' };
const server = http.createServer(async (req, res) => {
  try {
    let path = req.url.split('?')[0];
    if (path === '/') path = '/index.html';
    const file = normalize(join(ROOT, path));
    if (!file.startsWith(normalize(ROOT))) throw new Error('forbidden');
    const data = await readFile(file);
    res.writeHead(200, { 'content-type': MIME[extname(file)] || 'application/octet-stream' });
    res.end(data);
  } catch {
    res.writeHead(404); res.end('not found');
  }
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const port = server.address().port;

const browser = await chromium.launch({
  executablePath: process.env.CHROMIUM_PATH || '/opt/pw-browsers/chromium',
  args: [
    '--no-sandbox', '--enable-unsafe-webgpu', '--enable-features=Vulkan',
    '--use-webgpu-adapter=swiftshader', '--use-angle=swiftshader',
    '--disable-gpu-vsync', '--disable-frame-rate-limit',
  ],
});

async function render(query, frames) {
  const page = await browser.newPage({ viewport: VIEW });
  let pageError = null;
  page.on('pageerror', (e) => { pageError = e.message; });
  const t0 = Date.now();
  await page.goto(`http://127.0.0.1:${port}/?${BASEQ}&stopat=${frames}&${query}`);
  await page.waitForFunction(
    (n) => window.__voxelrt && (window.__voxelrt.error || window.__voxelrt.frames >= n),
    frames, { timeout: 1800000, polling: 250 });
  const state = await page.evaluate(() => window.__voxelrt);
  if (state.error || pageError) throw new Error(`renderer: ${state.error || pageError} (query=${query})`);
  const cap = await page.evaluate(() => window.__voxelrt.capture());
  await page.close();
  return {
    rgb: Buffer.from(cap.rgb, 'base64'),
    png: Buffer.from(cap.png.split(',')[1], 'base64'),
    w: cap.w, h: cap.h,
    msPerFrame: (Date.now() - t0) / frames,
  };
}

function psnr(a, b) {
  if (a.length !== b.length) throw new Error(`size mismatch ${a.length} vs ${b.length}`);
  let se = 0;
  for (let i = 0; i < a.length; i++) { const d = a[i] - b[i]; se += d * d; }
  const mse = se / a.length;
  return { mse, psnr: 10 * Math.log10(255 * 255 / Math.max(mse, 1e-9)) };
}

function poseQuery(p, extra = '') {
  return `px=${p.px}&py=${p.py}&pz=${p.pz}&yaw=${p.yaw}&pitch=${p.pitch}${extra}`;
}

// Reference: plain unbiased path tracer, denoiser off, unbounded accumulation,
// at the exact pose the scenario captures (including the strafe end pose).
async function reference(name, scen) {
  const file = join(REFDIR, `${name}.rgb`);
  const meta = join(REFDIR, `${name}.json`);
  if (existsSync(file)) return readFile(file);
  await mkdir(REFDIR, { recursive: true });
  const pose = { ...scen.pose };
  if (scen.move) pose.px += Math.sin((scen.frames - 1) * 0.07) * scen.move;
  console.log(`  building reference ${name} (${REF_FRAMES} frames)...`);
  const r = await render(
    `preset=base&denoise=0&maxhist=1000000&${poseQuery(pose)}`, REF_FRAMES);
  await writeFile(file, r.rgb);
  await writeFile(join(REFDIR, `${name}.png`), r.png);
  await writeFile(meta, JSON.stringify({ pose, frames: REF_FRAMES, w: r.w, h: r.h }));
  return r.rgb;
}

// --- One-off screenshot mode -------------------------------------------------
const shotQ = getArg('--shot', null);
if (shotQ) {
  const frames = parseInt(getArg('--frames', '32'), 10);
  const out = getArg('--out', join(ROOT, 'test', 'shot.png'));
  const r = await render(shotQ, frames);
  await writeFile(out, r.png);
  console.log(`${out} (${r.w}x${r.h}, ${r.msPerFrame.toFixed(0)} ms/frame incl. boot)`);
  await browser.close(); server.close();
  process.exit(0);
}

// --- Main --------------------------------------------------------------------
const scenarioNames = (getArg('--scenarios', Object.keys(SCENARIOS).join(',')))
  .split(',').filter(Boolean);
const configNames = (getArg('--configs', Object.keys(CONFIGS).join(',')))
  .split(',').filter(Boolean);
const repeats = parseInt(getArg('--repeats', '3'), 10);
const refsOnly = args.includes('--refs');

const results = [];
for (const sn of scenarioNames) {
  const scen = SCENARIOS[sn];
  if (!scen) throw new Error(`unknown scenario ${sn}`);
  const ref = await reference(sn, scen);
  if (refsOnly) continue;

  for (const cn of configNames) {
    const q = `${CONFIGS[cn]}&${poseQuery(scen.pose)}` +
      (scen.move ? `&benchmove=${scen.move}` : '');
    let mseSum = 0, ms = 0;
    for (let rep = 0; rep < repeats; rep++) {
      const r = await render(`${q}&fseed=${rep * 7717}`, scen.frames);
      const m = psnr(r.rgb, ref);
      mseSum += m.mse; ms += r.msPerFrame;
      if (rep === 0) {
        await writeFile(join(REFDIR, `${sn}_${cn}.png`), r.png);
      }
    }
    const mse = mseSum / repeats;
    const row = {
      scenario: sn, config: cn, mse: +mse.toFixed(2),
      psnr: +(10 * Math.log10(255 * 255 / mse)).toFixed(2),
      msPerFrame: +(ms / repeats).toFixed(0),
    };
    results.push(row);
    console.log(`${sn.padEnd(16)} ${cn.padEnd(6)} MSE ${String(row.mse).padStart(8)}  ` +
      `PSNR ${String(row.psnr).padStart(6)} dB  ~${row.msPerFrame} ms/f`);
  }
}

if (!refsOnly) {
  // Improvement factors vs the Lin-adapted configuration.
  console.log('\nMSE improvement factor (x lower is better):');
  for (const sn of scenarioNames) {
    const get = (c) => results.find((r) => r.scenario === sn && r.config === c);
    const lin = get('lin'), ours = get('ours'), base = get('base');
    if (lin && ours) {
      const vsLin = (lin.mse / ours.mse).toFixed(2);
      const vsBase = base ? (base.mse / ours.mse).toFixed(2) : '-';
      console.log(`  ${sn.padEnd(16)} ours vs lin: ${vsLin}x   ours vs base: ${vsBase}x`);
    }
  }
  await writeFile(join(REFDIR, 'results.json'), JSON.stringify(results, null, 2));
}

await browser.close();
server.close();
