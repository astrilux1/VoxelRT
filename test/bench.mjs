// Paper-style benchmark harness for the Lin 2026 ReSTIR PT Enhanced work.
//
// Default:
//   node test/bench.mjs --suite smoke
//
// Paper protocol shape:
//   node test/bench.mjs --suite ablation --resolution 1920x1080 --scale 1 --flip
//   node test/bench.mjs --suite convergence --resolution 1920x1080 --scale 1 --flip
//
// The harness captures linear HDR RGB before exposure/tonemap, caches converged
// base references, optionally scores with HDR-FLIP via flip-evaluator, and
// records WebGPU timestamp-query pass timings when the adapter supports them.

import { chromium } from 'playwright';
import http from 'node:http';
import { spawnSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { extname, join, normalize } from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  assessAdapter,
  chromiumLaunchOptions,
  detectNvidiaGpu,
  getArg,
  hasFlag,
} from './gpu-launch.mjs';

const ROOT = fileURLToPath(new URL('..', import.meta.url));
const EVALDIR = join(ROOT, 'test', 'eval');
const REFDIR = join(EVALDIR, 'refs');
const RUNDIR = join(EVALDIR, 'runs');
const args = process.argv.slice(2);

const flag = (name) => hasFlag(args, name);
const arg = (name, dflt) => getArg(args, name, dflt);
const parseList = (s) => String(s).split(',').map((x) => x.trim()).filter(Boolean);
const safe = (s) => String(s).replace(/[^a-zA-Z0-9_.-]+/g, '_');
const parseSize = (s) => {
  const m = /^(\d+)x(\d+)$/i.exec(String(s));
  if (!m) throw new Error(`bad --resolution ${s}, expected WIDTHxHEIGHT`);
  return { width: parseInt(m[1], 10), height: parseInt(m[2], 10) };
};

const suite = arg('--suite', 'smoke');
const view = parseSize(arg('--resolution', '1920x1080'));
const renderScale = arg('--scale', '1');
const bounces = arg('--bounces', '2');
const denoise = arg('--denoise', '0');
const timing = !flag('--no-timing');
const timingWarmup = parseInt(arg('--time-warmup', '4'), 10);
const refFrames = parseInt(arg('--ref-frames', suite === 'smoke' ? '16' : '1600'), 10);
// References use a much deeper bounce cut than the real-time operating point:
// this interior's Neumann series still gains +0.3% R from bounce 8 to 10, so a
// bounces=2 "reference" is ~15% dark and poisons every bias/FLIP number.
const refBounces = parseInt(arg('--ref-bounces', '12'), 10);
const framesDefault = parseInt(arg('--frames', suite === 'smoke' ? '8' : '64'), 10);
const repeats = parseInt(arg('--repeats', suite === 'smoke' ? '1' : '3'), 10);
const wantFlip = flag('--flip') || suite === 'paper';
const forceRefs = flag('--force-refs');
const refsOnly = flag('--refs');
const allowSoftwareGpu = flag('--allow-software-gpu');
const allowNonNvidiaGpu = flag('--allow-non-nvidia-gpu');
const explicitRequireNvidia = flag('--require-nvidia-gpu') || process.env.VOXELRT_REQUIRE_NVIDIA_GPU === '1';
const nvidiaGpus = detectNvidiaGpu();

function findPythonExecutable() {
  if (process.env.PYTHON) return process.env.PYTHON;
  if (process.platform === 'win32' && process.env.USERPROFILE) {
    const py = join(
      process.env.USERPROFILE,
      '.cache',
      'codex-runtimes',
      'codex-primary-runtime',
      'dependencies',
      'python',
      'python.exe',
    );
    if (existsSync(py)) return py;
  }
  return 'python';
}

const python = arg('--python', findPythonExecutable());

if (flag('--install-flip')) {
  const r = spawnSync(python, ['-m', 'pip', 'install', 'flip-evaluator'], { stdio: 'inherit' });
  if (r.status !== 0) process.exit(r.status ?? 1);
}

// interior: Cornell-style room dominated by emissive voxel faces.
// exterior: spawn-point terrain with sun/sky/lantern GI.
const POSES = {
  interior: { px: 8.8, py: 4.7, pz: 7.0, yaw: -0.45, pitch: 0.02 },
  exterior: { px: 8.0, py: 4.9, pz: 4.6, yaw: 0.85, pitch: -0.10 },
};
const MOVE_AMP = 0.6;
const MOVE_FRAMES = 96;

const SCENARIOS = {
  interior_static: { pose: POSES.interior, frames: framesDefault },
  interior_move: { pose: POSES.interior, frames: Math.max(framesDefault, MOVE_FRAMES), move: MOVE_AMP },
  exterior_static: { pose: POSES.exterior, frames: framesDefault },
  exterior_move: { pose: POSES.exterior, frames: Math.max(framesDefault, MOVE_FRAMES), move: MOVE_AMP },
};

const BASE_CONFIGS = {
  base: 'preset=base',
  gi: 'preset=gi',
  lin: 'preset=lin',
  ours: 'preset=ours',
  ours_no_lightpower: 'preset=ours&lightpower=0',
  ours_no_dup: 'preset=ours&dupmap=0',
};

// Existing renderer flags mapped into the paper's ablation shape. The early
// rows are not meant to reproduce Falcor micro-optimization rows; they isolate
// the same algorithmic additions this voxel renderer exposes.
const ABLATIONS = {
  gi: 'preset=gi',
  unified: 'preset=gi&unified=1',
  paired: 'preset=gi&unified=1&paired=1',
  footprint: 'preset=gi&unified=1&paired=1&footprint=1',
  dupmap: 'preset=gi&unified=1&paired=1&footprint=1&dupmap=1',
  vector: 'preset=gi&unified=1&paired=1&footprint=1&dupmap=1&vector=1',
  lin: 'preset=lin',
  ours_no_lightpower: 'preset=ours&lightpower=0',
  ours_no_dup: 'preset=ours&dupmap=0',
  ours: 'preset=ours',
};

const CONVERGENCE = {
  base: 'preset=base',
  lin: 'preset=lin',
  ours_no_dup: 'preset=ours&dupmap=0',
  ours: 'preset=ours',
};

const defaultScenarios = suite === 'smoke' ? 'interior_static' : 'interior_static,interior_move,exterior_static,exterior_move';
const scenarioNames = parseList(arg('--scenarios', defaultScenarios));
const defaultConfigs = suite === 'ablation'
  ? Object.keys(ABLATIONS).join(',')
  : suite === 'convergence'
    ? Object.keys(CONVERGENCE).join(',')
    : 'base,lin,ours';
const configNames = parseList(arg('--configs', defaultConfigs));
const frameList = parseList(arg('--frame-list', suite === 'convergence' ? '1,2,4,8,16,32,64,96,128' : String(framesDefault)))
  .map((x) => parseInt(x, 10));

function configQuery(name) {
  if (suite === 'ablation' && ABLATIONS[name]) return ABLATIONS[name];
  if (suite === 'convergence' && CONVERGENCE[name]) return CONVERGENCE[name];
  return BASE_CONFIGS[name] || name;
}

function poseQuery(p) {
  return `px=${p.px}&py=${p.py}&pz=${p.pz}&yaw=${p.yaw}&pitch=${p.pitch}`;
}

function finalPose(scen) {
  const pose = { ...scen.pose };
  if (scen.move) pose.px += Math.sin((scen.frames - 1) * 0.07) * scen.move;
  return pose;
}

function buildQuery(query, frames) {
  const p = new URLSearchParams(query);
  if (!p.has('scale')) p.set('scale', renderScale);
  if (!p.has('bounces')) p.set('bounces', bounces);
  if (!p.has('denoise')) p.set('denoise', denoise);
  p.set('nocanvas', '1');
  p.set('stopat', String(frames));
  if (timing) {
    p.set('timing', '1');
    p.set('timewarmup', String(timingWarmup));
  }
  // Defaults from Lin 2026 Section 7: cCap=20, 3 neighbors, R=30, sigma=16.
  if (!p.has('ccap')) p.set('ccap', '20');
  if (!p.has('taps')) p.set('taps', '3');
  if (!p.has('radius')) p.set('radius', '30');
  if (!p.has('sigma')) p.set('sigma', '16');
  return p.toString();
}

const MIME = {
  '.html': 'text/html',
  '.js': 'text/javascript',
  '.wgsl': 'text/plain',
  '.png': 'image/png',
  '.pdf': 'application/pdf',
};

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
    res.writeHead(404);
    res.end('not found');
  }
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const port = server.address().port;

const launch = chromiumLaunchOptions({ benchmark: true });
const browser = await chromium.launch(launch);
const browserVersion = browser.version();
const page = await browser.newPage({ viewport: view, deviceScaleFactor: 1 });
let pageError = null;
let adapterDiagnostics = null;

page.on('pageerror', (e) => { pageError = e.message; });
page.on('console', (m) => {
  if (m.type() === 'error') console.log(`[page:error] ${m.text()}`);
});

function validateAdapter(adapterInfo) {
  if (adapterDiagnostics) return;
  const requireNvidia = explicitRequireNvidia || Boolean(nvidiaGpus?.length && !allowNonNvidiaGpu);
  const assessed = assessAdapter(adapterInfo, {
    requireHardware: !allowSoftwareGpu,
    requireNvidia,
    label: 'Chromium WebGPU adapter',
  });
  adapterDiagnostics = {
    adapterInfo,
    text: assessed.text,
    nvidiaGpus,
    launchArgs: launch.args,
    executablePath: launch.executablePath || null,
  };
  if (nvidiaGpus?.length) console.log(`Host NVIDIA GPU(s): ${nvidiaGpus.join('; ')}`);
  console.log(`Chromium WebGPU adapter: ${assessed.text}`);
  for (const warning of assessed.warnings) console.warn(`WARNING: ${warning}`);
}

function recordTimingContext(gpuTiming) {
  if (!adapterDiagnostics) return;
  adapterDiagnostics.timestampQuery = {
    requested: gpuTiming?.requested ?? false,
    supported: gpuTiming?.supported ?? false,
    warmupFrames: timingWarmup,
    error: gpuTiming?.error,
  };
}

async function render(query, frames, { hdr = true } = {}) {
  pageError = null;
  const url = `http://127.0.0.1:${port}/?${buildQuery(query, frames)}`;
  const t0 = Date.now();
  await page.goto(url);
  await page.waitForFunction(
    (n) => window.__voxelrt && (window.__voxelrt.error || window.__voxelrt.frames >= n),
    frames,
    { timeout: 1800000, polling: 250 },
  );
  const state = await page.evaluate(() => ({
    frames: window.__voxelrt.frames,
    error: window.__voxelrt.error,
    gpuTiming: window.__voxelrt.gpuTiming,
    adapterInfo: window.__voxelrt.adapterInfo,
  }));
  validateAdapter(state.adapterInfo);
  recordTimingContext(state.gpuTiming);
  if (state.error || pageError) {
    throw new Error(`renderer: ${state.error || pageError} (query=${query})`);
  }
  if (timing && state.gpuTiming?.supported) {
    const expected = Math.max(0, frames - timingWarmup);
    await page.waitForFunction(
      (n) => window.__voxelrt.gpuTiming && window.__voxelrt.gpuTiming.frames >= n,
      expected,
      { timeout: 300000, polling: 100 },
    ).catch(() => {});
  }
  const cap = await page.evaluate((wantHdr) => window.__voxelrt.capture({ hdr: wantHdr }), hdr);
  const stateAfterCapture = await page.evaluate(() => ({
    frames: window.__voxelrt.frames,
    gpuTiming: window.__voxelrt.gpuTiming,
    adapterInfo: window.__voxelrt.adapterInfo,
  }));
  return {
    rgb: Buffer.from(cap.rgb, 'base64'),
    png: Buffer.from(cap.png.split(',')[1], 'base64'),
    hdr: cap.hdr ? Buffer.from(cap.hdr, 'base64') : null,
    hdrFormat: cap.hdrFormat,
    w: cap.w,
    h: cap.h,
    wallMsPerFrame: (Date.now() - t0) / frames,
    gpuTiming: stateAfterCapture.gpuTiming,
    adapterInfo: stateAfterCapture.adapterInfo,
  };
}

function averageTimings(gpuTiming) {
  if (!gpuTiming?.supported || !gpuTiming.frames) return null;
  const out = {};
  let total = 0;
  for (const [name, sum] of Object.entries(gpuTiming.sums || {})) {
    out[name] = sum / gpuTiming.frames;
    total += out[name];
  }
  out.total = total;
  out.frames = gpuTiming.frames;
  return out;
}

function performanceStats(frameCount, wallMsPerFrame, gpuMs) {
  const megapixels = (view.width * view.height) / 1e6;
  const stats = {
    frames: frameCount,
    pixels: view.width * view.height,
    megapixels,
    wallMsPerFrame,
    wallFps: 1000 / wallMsPerFrame,
    wallRunMs: wallMsPerFrame * frameCount,
  };
  if (!gpuMs?.total) return stats;

  stats.gpuMsPerFrame = gpuMs.total;
  stats.gpuFps = 1000 / gpuMs.total;
  stats.gpuMsPerMegapixel = gpuMs.total / megapixels;
  stats.gpuRunMs = gpuMs.total * frameCount;
  stats.timingFrames = gpuMs.frames;

  let dominantName = '';
  let dominantMs = 0;
  const passPercent = {};
  for (const [name, ms] of Object.entries(gpuMs)) {
    if (name === 'total' || name === 'frames') continue;
    passPercent[name] = (ms / gpuMs.total) * 100;
    if (ms > dominantMs) {
      dominantName = name;
      dominantMs = ms;
    }
  }
  stats.gpuPassPercent = passPercent;
  stats.dominantPass = dominantName;
  stats.dominantPassPercent = dominantName ? passPercent[dominantName] : 0;
  return stats;
}

function formatPerf(stats) {
  const wall = `wall ${stats.wallMsPerFrame.toFixed(1)} ms/f ${stats.wallFps.toFixed(1)} fps`;
  if (!stats.gpuMsPerFrame) return wall;
  const gpu = `GPU ${stats.gpuMsPerFrame.toFixed(2)} ms/f ${stats.gpuFps.toFixed(1)} fps`;
  const norm = `${stats.gpuMsPerMegapixel.toFixed(2)} ms/MP`;
  const cost = `cost ${(stats.gpuRunMs / 1000).toFixed(2)} GPU-s/${stats.frames}f`;
  const timed = stats.timingFrames ? `timed ${stats.timingFrames}f` : '';
  const pass = stats.dominantPass
    ? `${stats.dominantPass} ${stats.dominantPassPercent.toFixed(0)}%`
    : '';
  return [wall, gpu, norm, cost, timed, pass].filter(Boolean).join(' | ');
}

function quantile(values, q) {
  if (!values.length) return null;
  const sorted = [...values].sort((a, b) => a - b);
  const pos = (sorted.length - 1) * q;
  const lo = Math.floor(pos);
  const hi = Math.ceil(pos);
  if (lo === hi) return sorted[lo];
  return sorted[lo] + (sorted[hi] - sorted[lo]) * (pos - lo);
}

function sampleStats(values) {
  const xs = values.filter((v) => Number.isFinite(v));
  if (!xs.length) return null;
  const mean = xs.reduce((s, v) => s + v, 0) / xs.length;
  const variance = xs.length > 1
    ? xs.reduce((s, v) => s + (v - mean) ** 2, 0) / (xs.length - 1)
    : 0;
  const stddev = Math.sqrt(variance);
  return {
    n: xs.length,
    mean,
    median: quantile(xs, 0.5),
    stddev,
    cv: mean !== 0 ? stddev / mean : 0,
    min: Math.min(...xs),
    p95: quantile(xs, 0.95),
    max: Math.max(...xs),
  };
}

function summarizeGroup(scenario, config, frameCount, rows, avgMse, avgFlip) {
  const wall = sampleStats(rows.map((r) => r.perf?.wallMsPerFrame));
  const gpu = sampleStats(rows.map((r) => r.perf?.gpuMsPerFrame));
  const gpuMp = sampleStats(rows.map((r) => r.perf?.gpuMsPerMegapixel));
  const gpuCost = sampleStats(rows.map((r) => r.perf?.gpuRunMs));
  const metric = avgFlip != null
    ? { name: 'flip', mean: avgFlip }
    : { name: 'hdrMse', mean: avgMse };
  return {
    scenario,
    config,
    frames: frameCount,
    repeats: rows.length,
    metric,
    wallMsPerFrame: wall,
    gpuMsPerFrame: gpu,
    gpuMsPerMegapixel: gpuMp,
    gpuRunMs: gpuCost,
    timingFramesMean: sampleStats(rows.map((r) => r.perf?.timingFrames))?.mean ?? null,
    dominantPasses: rows.map((r) => ({
      repeat: r.repeat,
      pass: r.perf?.dominantPass,
      percent: r.perf?.dominantPassPercent,
    })),
  };
}

async function score(refPath, testPath, w, h) {
  const script = join(ROOT, 'test', 'metrics.py');
  const cmd = [
    script,
    '--ref', refPath,
    '--test', testPath,
    '--width', String(w),
    '--height', String(h),
  ];
  if (wantFlip) cmd.push('--flip');
  const r = spawnSync(python, cmd, { encoding: 'utf8' });
  if (r.status !== 0) {
    throw new Error(`metrics failed: ${r.stderr || r.stdout}`);
  }
  return JSON.parse(r.stdout);
}

async function writeCapture(stem, capture, extraMeta = {}) {
  await mkdir(RUNDIR, { recursive: true });
  const f32 = join(RUNDIR, `${stem}.rgbf32`);
  const png = join(RUNDIR, `${stem}.png`);
  const meta = join(RUNDIR, `${stem}.json`);
  if (!capture.hdr) throw new Error('renderer did not return HDR capture');
  await writeFile(f32, capture.hdr);
  await writeFile(png, capture.png);
  await writeFile(meta, JSON.stringify({
    ...extraMeta,
    w: capture.w,
    h: capture.h,
    hdrFormat: capture.hdrFormat,
    wallMsPerFrame: capture.wallMsPerFrame,
    gpuTiming: capture.gpuTiming,
  }, null, 2));
  return { f32, png, meta };
}

async function reference(name, scen) {
  await mkdir(REFDIR, { recursive: true });
  const stem = `${safe(name)}_${view.width}x${view.height}_s${safe(renderScale)}_rb${refBounces}`;
  const f32 = join(REFDIR, `${stem}.rgbf32`);
  const png = join(REFDIR, `${stem}.png`);
  const metaPath = join(REFDIR, `${stem}.json`);
  const pose = finalPose(scen);
  const meta = {
    scenario: name,
    pose,
    frames: refFrames,
    width: view.width,
    height: view.height,
    scale: renderScale,
    bounces: refBounces,
    query: `preset=base&denoise=0&maxhist=1000000&bounces=${refBounces}&${poseQuery(pose)}`,
  };
  if (!forceRefs && existsSync(f32) && existsSync(metaPath)) {
    const old = JSON.parse(await readFile(metaPath, 'utf8'));
    if (old.width === meta.width && old.height === meta.height &&
        old.scale === meta.scale && old.bounces === meta.bounces &&
        old.frames === meta.frames && old.query === meta.query) {
      return { f32, png, meta: old };
    }
  }
  console.log(`  building HDR reference ${name} (${refFrames} frames, ${view.width}x${view.height})...`);
  const r = await render(meta.query, refFrames, { hdr: true });
  await writeFile(f32, r.hdr);
  await writeFile(png, r.png);
  await writeFile(metaPath, JSON.stringify({ ...meta, hdrFormat: r.hdrFormat }, null, 2));
  return { f32, png, meta };
}

const shotQ = arg('--shot', null);
if (shotQ) {
  const frames = parseInt(arg('--frames', '32'), 10);
  const out = arg('--out', join(ROOT, 'test', 'shot.png'));
  const r = await render(shotQ, frames, { hdr: true });
  await writeFile(out, r.png);
  const stem = safe(`shot_${Date.now()}`);
  const files = await writeCapture(stem, r, { query: shotQ, frames });
  console.log(`${out} (${r.w}x${r.h}, HDR ${files.f32})`);
  await browser.close();
  server.close();
  process.exit(0);
}

const results = [];
const summaries = [];
for (const sn of scenarioNames) {
  const scen = SCENARIOS[sn];
  if (!scen) throw new Error(`unknown scenario ${sn}`);
  const ref = await reference(sn, scen);
  if (refsOnly) continue;

  for (const cn of configNames) {
    for (const frameCount of frameList) {
      let mseSum = 0;
      let flipSum = 0;
      let flipN = 0;
      let wallSum = 0;
      const timingSums = {};
      let timingN = 0;
      let timingFramesSum = 0;
      const groupRows = [];

      for (let rep = 0; rep < repeats; rep++) {
        const q = `${configQuery(cn)}&${poseQuery(scen.pose)}` +
          (scen.move ? `&benchmove=${scen.move}` : '') +
          `&fseed=${rep * 7717}`;
        const cap = await render(q, frameCount, { hdr: true });
        const avg = averageTimings(cap.gpuTiming);
        const perf = performanceStats(frameCount, cap.wallMsPerFrame, avg);
        const stem = safe(`${sn}_${cn}_${frameCount}f_r${rep}_${view.width}x${view.height}`);
        const files = await writeCapture(stem, cap, {
          scenario: sn,
          config: cn,
          query: q,
          frames: frameCount,
          repeat: rep,
          perf,
        });
        const m = await score(ref.f32, files.f32, cap.w, cap.h);
        mseSum += m.hdrMse;
        if (m.flip != null) {
          flipSum += m.flip;
          flipN++;
        }
        wallSum += cap.wallMsPerFrame;
        if (avg) {
          timingN++;
          timingFramesSum += avg.frames || 0;
          for (const [name, ms] of Object.entries(avg)) {
            if (name !== 'frames') timingSums[name] = (timingSums[name] || 0) + ms;
          }
        }

        const row = {
          scenario: sn,
          config: cn,
          frames: frameCount,
          repeat: rep,
          hdrMse: m.hdrMse,
          hdrRmse: m.hdrRmse,
          hdrPsnrPeak: m.hdrPsnrPeak,
          meanAbsRelative: m.meanAbsRelative,
          flip: m.flip,
          flipAvailable: m.flipAvailable,
          flipError: m.flipError,
          wallMsPerFrame: cap.wallMsPerFrame,
          gpuMs: avg,
          perf,
          png: files.png,
          hdr: files.f32,
        };
        results.push(row);
        groupRows.push(row);
        const metric = m.flip != null ? `FLIP ${m.flip.toFixed(4)}` : `HDR-MSE ${m.hdrMse.toExponential(3)}`;
        console.log(`${sn.padEnd(16)} ${cn.padEnd(18)} ${String(frameCount).padStart(4)}f r${rep}  ${metric} | ${formatPerf(perf)}`);
      }

      const avgMse = mseSum / repeats;
      const avgFlip = flipN ? flipSum / flipN : null;
      const avgWall = wallSum / repeats;
      const avgTimings = timingN
        ? Object.fromEntries(Object.entries(timingSums).map(([k, v]) => [k, v / timingN]))
        : null;
      if (avgTimings) avgTimings.frames = timingFramesSum / timingN;
      const avgPerf = performanceStats(frameCount, avgWall, avgTimings);
      const summaryMetric = avgFlip != null ? `FLIP ${avgFlip.toFixed(4)}` : `HDR-MSE ${avgMse.toExponential(3)}`;
      const summary = summarizeGroup(sn, cn, frameCount, groupRows, avgMse, avgFlip);
      summaries.push(summary);
      const jitter = summary.gpuMsPerFrame && summary.gpuMsPerFrame.n > 1
        ? ` | GPU median ${summary.gpuMsPerFrame.median.toFixed(2)} ms cv ${(summary.gpuMsPerFrame.cv * 100).toFixed(1)}% p95 ${summary.gpuMsPerFrame.p95.toFixed(2)}`
        : '';
      console.log(`  avg ${sn}/${cn}/${frameCount}f: ${summaryMetric} | ${formatPerf(avgPerf)}`);
      if (jitter) console.log(`      repeats ${summary.repeats}${jitter}`);
    }
  }
}

await mkdir(EVALDIR, { recursive: true });
const settings = {
  suite,
  resolution: view,
  renderScale,
  bounces,
  denoise,
  timing,
  timingWarmup,
  refFrames,
  repeats,
  scenarios: scenarioNames,
  configs: configNames,
  frameList,
  wantFlip,
  gpu: adapterDiagnostics,
  runtime: {
    node: process.version,
    platform: process.platform,
    arch: process.arch,
    browserVersion,
    argv: process.argv.slice(2),
  },
};
await writeFile(join(EVALDIR, 'results.json'), JSON.stringify({ settings, results, summaries }, null, 2));
const csv = [
  'scenario,config,frames,repeat,flip,hdrMse,hdrRmse,hdrPsnrPeak,meanAbsRelative,pixels,megapixels,wallMsPerFrame,wallFps,wallRunMs,gpuMsPerFrame,gpuFps,gpuMsPerMegapixel,gpuRunMs,timingFrames,dominantPass,dominantPassPercent,pathtraceMs,reuseTemporalMs,reuseSpatialMs,dupmapMs,temporalMs,atrousMs,pathtracePct,reuseTemporalPct,reuseSpatialPct,dupmapPct,temporalPct,atrousPct',
  ...results.map((r) => {
    const g = r.gpuMs || {};
    const p = r.perf || {};
    const pct = p.gpuPassPercent || {};
    const atrous = Object.entries(g)
      .filter(([k]) => k.startsWith('atrous'))
      .reduce((s, [, v]) => s + v, 0);
    return [
      r.scenario,
      r.config,
      r.frames,
      r.repeat,
      r.flip ?? '',
      r.hdrMse,
      r.hdrRmse,
      r.hdrPsnrPeak,
      r.meanAbsRelative,
      p.pixels ?? '',
      p.megapixels ?? '',
      r.wallMsPerFrame,
      p.wallFps ?? '',
      p.wallRunMs ?? '',
      p.gpuMsPerFrame ?? g.total ?? '',
      p.gpuFps ?? '',
      p.gpuMsPerMegapixel ?? '',
      p.gpuRunMs ?? '',
      p.timingFrames ?? g.frames ?? '',
      p.dominantPass ?? '',
      p.dominantPassPercent ?? '',
      g.pathtrace ?? '',
      g.reuse_temporal ?? '',
      g.reuse_spatial ?? '',
      g.dupmap ?? '',
      g.temporal ?? '',
      atrous || '',
      pct.pathtrace ?? '',
      pct.reuse_temporal ?? '',
      pct.reuse_spatial ?? '',
      pct.dupmap ?? '',
      pct.temporal ?? '',
      pct.atrous ?? '',
    ].join(',');
  }),
].join('\n');
await writeFile(join(EVALDIR, 'results.csv'), `${csv}\n`);

const statFields = (prefix) => [
  `${prefix}N`,
  `${prefix}Mean`,
  `${prefix}Median`,
  `${prefix}Stddev`,
  `${prefix}Cv`,
  `${prefix}Min`,
  `${prefix}P95`,
  `${prefix}Max`,
];
const statValues = (s) => s
  ? [s.n, s.mean, s.median, s.stddev, s.cv, s.min, s.p95, s.max]
  : ['', '', '', '', '', '', '', ''];
const summaryCsv = [
  [
    'scenario',
    'config',
    'frames',
    'repeats',
    'metric',
    'metricMean',
    ...statFields('wallMsPerFrame'),
    ...statFields('gpuMsPerFrame'),
    ...statFields('gpuMsPerMegapixel'),
    ...statFields('gpuRunMs'),
    'timingFramesMean',
  ].join(','),
  ...summaries.map((s) => [
    s.scenario,
    s.config,
    s.frames,
    s.repeats,
    s.metric.name,
    s.metric.mean,
    ...statValues(s.wallMsPerFrame),
    ...statValues(s.gpuMsPerFrame),
    ...statValues(s.gpuMsPerMegapixel),
    ...statValues(s.gpuRunMs),
    s.timingFramesMean ?? '',
  ].join(',')),
].join('\n');
await writeFile(join(EVALDIR, 'summary.csv'), `${summaryCsv}\n`);

await browser.close();
server.close();

if (wantFlip && results.some((r) => !r.flipAvailable)) {
  console.log('\nHDR-FLIP was requested but not available for at least one row.');
  console.log(`Install it with: ${python} -m pip install flip-evaluator`);
}
console.log(`\nWrote ${join(EVALDIR, 'results.json')}, ${join(EVALDIR, 'results.csv')} and ${join(EVALDIR, 'summary.csv')}`);
