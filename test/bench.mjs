// Paper-style benchmark harness for the Lin 2026 ReSTIR PT Enhanced work.
//
// Default:
//   node test/bench.mjs --suite smoke
//
// Paper protocol shape:
//   node test/bench.mjs --suite ablation --resolution 1920x1080 --scale 1 --flip
//   node test/bench.mjs --suite convergence --resolution 1920x1080 --scale 1 --flip
//   npm.cmd run bench:references         # strict claim preflight + reference convergence
//   npm.cmd run bench:correctness        # strict unbiased estimator convergence gate
//   node test/bench.mjs --claim ...      # fail closed on manifest/runtime drift
//
// The harness captures linear HDR RGB before exposure/tonemap, caches converged
// base references, optionally scores with HDR-FLIP via flip-evaluator, and
// records WebGPU timestamp-query pass timings when the adapter supports them.

import { chromium } from 'playwright';
import http from 'node:http';
import { spawnSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { mkdir, readFile, stat, writeFile } from 'node:fs/promises';
import { extname, join, normalize, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  assessAdapter,
  chromiumLaunchOptions,
  detectNvidiaGpu,
  getArg,
  hasFlag,
} from './gpu-launch.mjs';
import {
  assessReferenceConvergence,
  claimIdentity,
  claimSettingsProblems,
  loadClaimManifest,
  referenceMetadataMismatches,
} from './claim-manifest.mjs';
import {
  assessEstimatorCorrectness,
  correctnessClaimProblems,
  correctnessIdentity,
  loadCorrectnessManifest,
} from './correctness-manifest.mjs';

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

const claimPreflightOnly = flag('--claim-preflight');
const claimMode = flag('--claim') || claimPreflightOnly;
const validateRefs = flag('--validate-refs');
const manifestPath = resolve(arg('--manifest', join(EVALDIR, 'claim-manifest.v1.json')));
const loadedManifest = await loadClaimManifest(manifestPath);
const manifest = loadedManifest.data;
const manifestIdentity = claimIdentity(loadedManifest);
const suite = arg('--suite', 'smoke');
const correctnessMode = suite === 'correctness';
const correctnessManifestPath = resolve(arg(
  '--correctness-manifest',
  join(EVALDIR, 'correctness-manifest.v1.json'),
));
const loadedCorrectness = correctnessMode
  ? await loadCorrectnessManifest(correctnessManifestPath)
  : null;
const correctness = loadedCorrectness?.data ?? null;
if (correctnessMode) {
  if (!claimMode) throw new Error('the correctness suite requires --claim');
  if (validateRefs || flag('--refs')) {
    throw new Error('the correctness suite cannot be combined with reference-only modes');
  }
  const problems = correctnessClaimProblems(correctness, manifest, manifestIdentity);
  if (problems.length) throw new Error(`correctness manifest mismatch:\n- ${problems.join('\n- ')}`);
}
const frozenConfigurations = correctnessMode ? correctness.configurations : manifest.configurations;
const claimResolution = `${manifest.capture.resolution.width}x${manifest.capture.resolution.height}`;
const view = parseSize(arg('--resolution', claimMode ? claimResolution : '1920x1080'));
const renderScale = arg('--scale', claimMode ? manifest.capture.renderScale : '1');
const bounces = arg('--bounces', claimMode
  ? String(correctnessMode ? correctness.capture.bounces : manifest.capture.operatingBounces)
  : '6');
const denoise = arg('--denoise', claimMode ? manifest.capture.denoise : '0');
const timing = !flag('--no-timing');
const timingWarmup = parseInt(arg('--time-warmup', String(manifest.capture.timing.warmupFrames)), 10);
const refFrames = parseInt(arg('--ref-frames', claimMode
  ? String(manifest.reference.targetFrames)
  : suite === 'smoke' ? '16' : '1600'), 10);
// References use a much deeper bounce cut than the real-time operating point:
// this interior's Neumann series still gains +0.3% R from bounce 8 to 10. A
// shallow bounce-count reference poisons every bias/FLIP number.
const refBounces = parseInt(arg('--ref-bounces', String(manifest.reference.bounces)), 10);
const framesDefault = parseInt(arg('--frames', correctnessMode
  ? String(correctness.capture.frames)
  : suite === 'smoke' ? '8' : '64'), 10);
const repeats = parseInt(arg('--repeats', suite === 'smoke' ? '1' : '3'), 10);
const wantFlip = claimMode || flag('--flip') || suite === 'paper';
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

function pythonRuntimeVersions() {
  const code = [
    'import importlib.metadata as m, json, sys',
    'print(json.dumps({"python": sys.version.split()[0], "numpy": m.version("numpy"), "flipEvaluator": m.version("flip-evaluator")}))',
  ].join('; ');
  const r = spawnSync(python, ['-c', code], { encoding: 'utf8' });
  if (r.status !== 0) {
    if (claimMode) throw new Error(`claim preflight could not inspect Python metrics: ${r.stderr || r.stdout}`);
    return { error: String(r.stderr || r.stdout).trim() };
  }
  return JSON.parse(r.stdout);
}

if (flag('--install-flip')) {
  const r = spawnSync(python, ['-m', 'pip', 'install', 'flip-evaluator'], { stdio: 'inherit' });
  if (r.status !== 0) process.exit(r.status ?? 1);
}

const playwrightPackage = JSON.parse(await readFile(join(ROOT, 'node_modules', 'playwright', 'package.json'), 'utf8'));
const metricRuntime = (claimMode || wantFlip) ? pythonRuntimeVersions() : null;

// interior: Cornell-style room dominated by emissive voxel faces.
// exterior: spawn-point terrain with sun/sky/lantern GI.
// lamps: `?scene=lamps` room variant with heterogeneous emitters (tiny bright
//        warm lamps, medium colored panels, large dim cool strips, occluders)
//        so light-selection techniques have signal; pose sees all classes.
// A scenario may pin a scene variant; `scene` is appended to every query for
// that scenario (test runs *and* its cached reference build).
const SCENARIOS = Object.fromEntries(Object.entries(manifest.scenarios).map(([name, scen]) => [name, {
  pose: scen.pose,
  scene: scen.scene,
  move: scen.motion?.amplitude,
  motionStep: scen.motion?.angularStep,
  referenceFrame: scen.referenceFrame,
}]));

const BASE_CONFIGS = {
  base: 'preset=base&fclamp=0',
  gi: 'preset=gi&fclamp=0',
  gi_unbiased: 'preset=gi&maxhist=1000000&fclamp=0',
  lin: 'preset=lin&fclamp=0',
  lin_unbiased: 'preset=lin&dupmap=0&footprint=0&maxhist=1000000&fclamp=0',
  ours: 'preset=ours&fclamp=0',
  ours_motion: 'preset=ours_motion&maxhist=64&fclamp=0',
  ours_unbiased: 'preset=ours&dupmap=0&rclamp=0&fclamp=0',
  ours_no_lightpower: 'preset=ours&lightpower=0&fclamp=0',
  ours_no_dup: 'preset=ours&dupmap=0&fclamp=0',
  ours_adaptcand: 'preset=ours&adaptcand=1&fclamp=0',
  ours_lightgrid: 'preset=ours&lightgrid=1&fclamp=0',
  ours_adapt_lightgrid: 'preset=ours&adaptcand=1&lightgrid=1&fclamp=0',
  ours_sigma24: 'preset=ours&sigma=24&fclamp=0',
  ours_sigma32: 'preset=ours&sigma=32&fclamp=0',
  ours_sigma48: 'preset=ours&sigma=48&fclamp=0',
  ours_mixsigma24: 'preset=ours&mixsigma=1&sigma2=24&fclamp=0',
  ours_mixsigma32: 'preset=ours&mixsigma=1&sigma2=32&fclamp=0',
  ours_mixsigma48: 'preset=ours&mixsigma=1&sigma2=48&fclamp=0',
  ours_mixsigma: 'preset=ours&mixsigma=1&fclamp=0',
  gi_histisolate: 'preset=gi&histisolate=1&maxhist=1000000&fclamp=0',
  unified_histisolate: 'preset=gi&unified=1&histisolate=1&maxhist=1000000&fclamp=0',
  lin_histisolate: 'preset=lin&dupmap=0&footprint=0&histisolate=1&maxhist=1000000&fclamp=0',
  ours_histisolate: 'preset=ours&dupmap=0&rclamp=0&footprint=0&histisolate=1&maxhist=1000000&fclamp=0',
};

// Existing renderer flags mapped into the paper's ablation shape. The early
// rows are not meant to reproduce Falcor micro-optimization rows; they isolate
// the same algorithmic additions this voxel renderer exposes.
const ABLATIONS = {
  gi: 'preset=gi&fclamp=0',
  unified: 'preset=gi&unified=1&fclamp=0',
  paired: 'preset=gi&unified=1&paired=1&fclamp=0',
  footprint: 'preset=gi&unified=1&paired=1&footprint=1&fclamp=0',
  dupmap: 'preset=gi&unified=1&paired=1&footprint=1&dupmap=1&fclamp=0',
  vector: 'preset=gi&unified=1&paired=1&footprint=1&dupmap=1&vector=1&fclamp=0',
  lin: 'preset=lin&fclamp=0',
  ours_no_lightpower: 'preset=ours&lightpower=0&fclamp=0',
  ours_no_dup: 'preset=ours&dupmap=0&fclamp=0',
  ours: 'preset=ours&fclamp=0',
};

const CONVERGENCE = {
  base: 'preset=base&fclamp=0',
  lin: 'preset=lin&fclamp=0',
  ours_unbiased: 'preset=ours&dupmap=0&rclamp=0&fclamp=0',
  ours: 'preset=ours&fclamp=0',
  // World-space GI cache (docs/WORLDGI.md §7.4 thesis test): identical to
  // `ours` plus the persistent brick/face reuse cache, so `ours` is the exact
  // screen-only control that does the same non-cache work.
  ours_worldgi: 'preset=ours&worldgi=1&fclamp=0',
  ours_worldgi_cap1: 'preset=ours&worldgi=1&wgicap=1&fclamp=0',
  ours_worldgi_cap4: 'preset=ours&worldgi=1&wgicap=4&fclamp=0',
};

const defaultScenarios = correctnessMode
  ? correctness.scenarios.join(',')
  : claimMode
    ? Object.keys(SCENARIOS).join(',')
    : suite === 'smoke' ? 'interior_static' : 'interior_static,interior_move,exterior_static,exterior_move';
const scenarioNames = parseList(arg('--scenarios', defaultScenarios));
const defaultConfigs = correctnessMode
  ? Object.keys(correctness.configurations).join(',')
  : suite === 'ablation'
  ? Object.keys(ABLATIONS).join(',')
  : suite === 'convergence'
    ? Object.keys(CONVERGENCE).join(',')
    : 'base,lin,ours';
const configNames = parseList(arg('--configs', defaultConfigs));
const frameList = parseList(arg(
  '--frame-list',
  suite === 'convergence' ? '1,2,4,8,16,32,64,96,128' : String(framesDefault),
))
  .map((x) => parseInt(x, 10));

function configQuery(name) {
  let query;
  if (correctnessMode && correctness.configurations[name]) query = correctness.configurations[name];
  else if (suite === 'ablation' && ABLATIONS[name]) query = ABLATIONS[name];
  else if (suite === 'convergence' && CONVERGENCE[name]) query = CONVERGENCE[name];
  else query = BASE_CONFIGS[name] || name;
  if (claimMode && frozenConfigurations[name] !== query) {
    throw new Error(`claim configuration ${name} is not frozen or drifted (resolved ${query})`);
  }
  return query;
}

function poseQuery(p) {
  return `px=${p.px}&py=${p.py}&pz=${p.pz}&yaw=${p.yaw}&pitch=${p.pitch}`;
}

// Scene-variant fragment for a scenario ('' for the default scene, so all
// pre-existing scenario queries — and their cached references — are unchanged).
function sceneQuery(scen) {
  return scen.scene ? `&scene=${scen.scene}` : '';
}

function finalPose(scen, evaluationFrame = scen.referenceFrame) {
  const pose = { ...scen.pose };
  if (scen.move) pose.px += Math.sin((evaluationFrame - 1) * scen.motionStep) * scen.move;
  return pose;
}

function buildQuery(query, frames) {
  const p = new URLSearchParams(query);
  if (!p.has('scale')) p.set('scale', renderScale);
  if (!p.has('bounces')) p.set('bounces', bounces);
  if (!p.has('denoise')) p.set('denoise', denoise);
  if (claimMode && !p.has('fclamp')) {
    p.set('fclamp', String(manifest.capture.fireflyClamp));
  }
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
if (claimMode) {
  const configQueries = Object.fromEntries(configNames.map((name) => [name, configQuery(name)]));
  const settingsProfile = correctnessMode ? {
    bounces: correctness.capture.bounces,
    scenarios: correctness.scenarios,
    frameCheckpoints: [correctness.capture.frames],
    configurations: correctness.configurations,
    requireExact: true,
  } : undefined;
  const problems = claimSettingsProblems(manifest, {
    resolution: view,
    renderScale,
    bounces,
    denoise,
    timing,
    timingWarmup,
    refBounces,
    refFrames,
    wantFlip,
    launchArgs: launch.args,
    playwrightVersion: playwrightPackage.version,
    browserVersion,
    flipEvaluatorVersion: metricRuntime?.flipEvaluator,
    scenarios: scenarioNames,
    frameList,
    configQueries,
  }, settingsProfile);
  if (problems.length) {
    await browser.close();
    server.close();
    throw new Error(`claim manifest mismatch:\n- ${problems.join('\n- ')}`);
  }
}
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
  if (claimMode) {
    const features = new Set(adapterInfo?.features || []);
    const hostMatch = (nvidiaGpus || []).includes(manifest.runtime.hostGpu);
    if (!hostMatch) throw new Error(`claim requires host GPU ${manifest.runtime.hostGpu}`);
    if (String(adapterInfo?.vendor).toLowerCase() !== manifest.runtime.adapterVendor.toLowerCase()) {
      throw new Error(`claim requires adapter vendor ${manifest.runtime.adapterVendor}, got ${adapterInfo?.vendor}`);
    }
    if (String(adapterInfo?.architecture).toLowerCase() !== manifest.runtime.adapterArchitecture.toLowerCase()) {
      throw new Error(`claim requires adapter architecture ${manifest.runtime.adapterArchitecture}, got ${adapterInfo?.architecture}`);
    }
    for (const feature of manifest.runtime.requiredFeatures) {
      if (!features.has(feature)) throw new Error(`claim requires WebGPU feature ${feature}`);
    }
    if (adapterInfo?.accumulationFormat !== manifest.runtime.accumulationFormat) {
      throw new Error(`claim requires ${manifest.runtime.accumulationFormat} accumulation, got ${adapterInfo?.accumulationFormat}`);
    }
  }
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
  if (claimMode && (!gpuTiming?.requested || !gpuTiming?.supported)) {
    throw new Error(`claim requires working timestamp queries: ${gpuTiming?.error || 'unsupported'}`);
  }
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
  if (claimMode && hdr && cap.hdrFormat !== manifest.capture.hdrFormat) {
    throw new Error(`claim requires HDR format ${manifest.capture.hdrFormat}, got ${cap.hdrFormat}`);
  }
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
  const metrics = JSON.parse(r.stdout);
  if (claimMode && !metrics.flipAvailable) {
    throw new Error(`claim requires HDR-FLIP: ${metrics.flipError || 'flip-evaluator unavailable'}`);
  }
  return metrics;
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

function referenceRuntime() {
  return {
    node: process.version,
    platform: process.platform,
    arch: process.arch,
    browserVersion,
    playwrightVersion: playwrightPackage.version,
    metrics: metricRuntime,
    gpu: adapterDiagnostics,
  };
}

async function reference(name, scen, referenceFrames = refFrames, evaluationFrame = scen.referenceFrame) {
  await mkdir(REFDIR, { recursive: true });
  const motionStem = scen.move ? `_at${evaluationFrame}f` : '';
  const claimStem = claimMode ? `_cm${manifest.schemaVersion}_${loadedManifest.sha256.slice(0, 12)}` : '';
  const stem = `${safe(name)}_${view.width}x${view.height}_s${safe(renderScale)}` +
    `_rb${refBounces}_rf${referenceFrames}${motionStem}${claimStem}`;
  const f32 = join(REFDIR, `${stem}.rgbf32`);
  const png = join(REFDIR, `${stem}.png`);
  const metaPath = join(REFDIR, `${stem}.json`);
  const pose = finalPose(scen, evaluationFrame);
  const maxHistory = manifest.reference.maxHistory;
  const seed = manifest.reference.seed;
  const query = `preset=base&denoise=0&maxhist=${maxHistory}&bounces=${refBounces}` +
    `&fclamp=${manifest.capture.fireflyClamp}` +
    `&scale=${renderScale}&fseed=${seed}&${poseQuery(pose)}${sceneQuery(scen)}`;
  const meta = {
    scenario: name,
    evaluationFrame,
    pose,
    frames: referenceFrames,
    width: view.width,
    height: view.height,
    scale: renderScale,
    bounces: refBounces,
    seed,
    query,
    hdrFormat: manifest.capture.hdrFormat,
    ...(claimMode ? { claimManifest: manifestIdentity } : {}),
  };
  if (!forceRefs && existsSync(f32) && existsSync(png) && existsSync(metaPath)) {
    try {
      const old = JSON.parse(await readFile(metaPath, 'utf8'));
      const mismatches = referenceMetadataMismatches(old, meta);
      const expectedBytes = view.width * view.height * 3 * 4;
      const bytes = (await stat(f32)).size;
      if (bytes !== expectedBytes) mismatches.push(`HDR byte length ${bytes} != ${expectedBytes}`);
      if (!mismatches.length) return { f32, png, meta: old };
      console.warn(`  rejecting cached reference ${stem}: ${mismatches.join(', ')}`);
    } catch (error) {
      console.warn(`  rejecting unreadable cached reference ${stem}: ${error.message}`);
    }
  }
  console.log(`  building HDR reference ${name} at evaluation frame ${evaluationFrame} ` +
    `(${referenceFrames} reference frames, ${view.width}x${view.height})...`);
  const r = await render(meta.query, referenceFrames, { hdr: true });
  if (r.hdrFormat !== manifest.capture.hdrFormat) {
    throw new Error(`reference HDR format ${r.hdrFormat} does not match ${manifest.capture.hdrFormat}`);
  }
  await writeFile(f32, r.hdr);
  await writeFile(png, r.png);
  await writeFile(metaPath, JSON.stringify({ ...meta, runtime: referenceRuntime() }, null, 2));
  return { f32, png, meta };
}

async function validateReferenceSet() {
  const configured = claimMode
    ? [...manifest.reference.convergence.primaryFrames, manifest.reference.convergence.extensionFrame]
    : parseList(arg('--ref-checkpoints', [
      ...manifest.reference.convergence.primaryFrames,
      manifest.reference.convergence.extensionFrame,
    ].join(','))).map((x) => parseInt(x, 10));
  if (configured.length !== 3 || configured.some((x) => !Number.isInteger(x) || x <= 0)) {
    throw new Error('--ref-checkpoints must contain LOW,HIGH,EXTENSION positive frame counts');
  }
  const [lowFrames, highFrames, extensionFrames] = configured;
  const thresholds = manifest.reference.convergence;
  const rows = [];
  for (const name of scenarioNames) {
    const scen = SCENARIOS[name];
    if (!scen) throw new Error(`unknown scenario ${name}`);
    const evaluationFrame = scen.referenceFrame;
    const low = await reference(name, scen, lowFrames, evaluationFrame);
    const high = await reference(name, scen, highFrames, evaluationFrame);
    const primaryMetrics = await score(low.f32, high.f32, view.width, view.height);
    const primary = assessReferenceConvergence(primaryMetrics, thresholds);
    const row = {
      scenario: name,
      evaluationFrame,
      primary: { lowFrames, highFrames, metrics: primaryMetrics, assessment: primary },
      pass: primary.pass,
      status: primary.pass ? 'converged' : 'not-converged',
    };
    if (!primary.pass) {
      const extension = await reference(name, scen, extensionFrames, evaluationFrame);
      const extensionMetrics = await score(high.f32, extension.f32, view.width, view.height);
      const extensionAssessment = assessReferenceConvergence(extensionMetrics, thresholds);
      row.extension = {
        lowFrames: highFrames,
        highFrames: extensionFrames,
        metrics: extensionMetrics,
        assessment: extensionAssessment,
      };
      row.status = extensionAssessment.pass ? 'manifest-update-required' : 'not-converged';
    }
    rows.push(row);
    const pct = (primary.maxChannelMeanRelativeDelta * 100).toFixed(3);
    console.log(`reference ${name}: ${row.status} | max mean delta ${pct}% | HDR PSNR ${primary.hdrPsnrPeakDb.toFixed(2)} dB`);
  }
  const report = {
    claimManifest: manifestIdentity,
    claimable: claimMode,
    resolution: view,
    thresholds: {
      maxChannelMeanRelativeDelta: thresholds.maxChannelMeanRelativeDelta,
      minHdrPsnrPeakDb: thresholds.minHdrPsnrPeakDb,
    },
    checkpoints: configured,
    pass: rows.every((row) => row.pass),
    rows,
    runtime: referenceRuntime(),
  };
  await mkdir(EVALDIR, { recursive: true });
  const reportName = claimMode
    ? `reference-convergence.v${manifest.schemaVersion}.json`
    : 'reference-convergence.json';
  await writeFile(join(EVALDIR, reportName), JSON.stringify(report, null, 2));
  return report;
}

const shotQ = arg('--shot', null);
if (shotQ) {
  if (claimMode) throw new Error('--shot outputs are diagnostic and cannot run with --claim');
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

if (claimMode) {
  const probeScenario = SCENARIOS[scenarioNames[0]];
  if (!probeScenario) throw new Error(`unknown scenario ${scenarioNames[0]}`);
  console.log(`Claim preflight: ${manifest.id} ${loadedManifest.sha256.slice(0, 12)}`);
  await render(`preset=base&${poseQuery(probeScenario.pose)}${sceneQuery(probeScenario)}`,
    Math.max(timingWarmup + 1, 5), { hdr: true });
  if (claimPreflightOnly) {
    console.log('Claim preflight passed: manifest, metrics, browser, adapter, fp32 accumulation, HDR capture, and timestamps.');
    await browser.close();
    server.close();
    process.exit(0);
  }
}

const results = [];
const summaries = [];
let referenceValidation = null;
if (validateRefs) {
  referenceValidation = await validateReferenceSet();
} else if (refsOnly) {
  for (const sn of scenarioNames) {
    const scen = SCENARIOS[sn];
    if (!scen) throw new Error(`unknown scenario ${sn}`);
    await reference(sn, scen, refFrames, scen.referenceFrame);
  }
} else {
  const references = new Map();
  for (const sn of scenarioNames) {
    const scen = SCENARIOS[sn];
    if (!scen) throw new Error(`unknown scenario ${sn}`);
    const seeds = correctnessMode
      ? correctness.capture.seeds
      : claimMode
        ? manifest.evaluation.qualitySeeds[scen.move ? 'motion' : 'static']
      : Array.from({ length: repeats }, (_, i) => i * manifest.evaluation.diagnosticSeedStride);

    for (const cn of configNames) {
      for (const frameCount of frameList) {
        const evaluationFrame = scen.move ? frameCount : scen.referenceFrame;
        const refKey = `${sn}:${evaluationFrame}:${refFrames}`;
        if (!references.has(refKey)) {
          references.set(refKey, await reference(sn, scen, refFrames, evaluationFrame));
        }
        const ref = references.get(refKey);
      let mseSum = 0;
      let flipSum = 0;
      let flipN = 0;
      let wallSum = 0;
      const timingSums = {};
      let timingN = 0;
      let timingFramesSum = 0;
      const groupRows = [];

      for (const [rep, seed] of seeds.entries()) {
        const q = `${configQuery(cn)}&${poseQuery(scen.pose)}` +
          sceneQuery(scen) +
          (scen.move ? `&benchmove=${scen.move}` : '') +
          `&fseed=${seed}`;
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
          seed,
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
          seed,
          hdrMse: m.hdrMse,
          hdrRmse: m.hdrRmse,
          hdrPsnrPeak: m.hdrPsnrPeak,
          hdrRelativeRmse: m.hdrRelativeRmse,
          meanAbsRelative: m.meanAbsRelative,
          referenceMeanRgb: m.referenceMeanRgb,
          testMeanRgb: m.testMeanRgb,
          channelMeanRelativeDelta: m.channelMeanRelativeDelta,
          maxChannelMeanRelativeDelta: m.maxChannelMeanRelativeDelta,
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

      const avgMse = mseSum / groupRows.length;
      const avgFlip = flipN ? flipSum / flipN : null;
      const avgWall = wallSum / groupRows.length;
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
}

let correctnessReport = null;
if (correctnessMode) {
  const expectedRows = correctness.scenarios.length *
    Object.keys(correctness.configurations).length * correctness.capture.seeds.length;
  if (results.length !== expectedRows) {
    throw new Error(`correctness suite produced ${results.length} rows; expected ${expectedRows}`);
  }
  const rows = results.map((row) => {
    const metrics = {
      hdrMse: row.hdrMse,
      hdrRmse: row.hdrRmse,
      hdrRelativeRmse: row.hdrRelativeRmse,
      hdrPsnrPeak: row.hdrPsnrPeak,
      meanAbsRelative: row.meanAbsRelative,
      referenceMeanRgb: row.referenceMeanRgb,
      testMeanRgb: row.testMeanRgb,
      channelMeanRelativeDelta: row.channelMeanRelativeDelta,
      maxChannelMeanRelativeDelta: row.maxChannelMeanRelativeDelta,
      flip: row.flip,
    };
    return {
      scenario: row.scenario,
      config: row.config,
      query: configQuery(row.config),
      frames: row.frames,
      bounces: Number(bounces),
      seed: row.seed,
      metrics,
      assessment: assessEstimatorCorrectness(metrics, correctness.thresholds),
    };
  });
  correctnessReport = {
    claimManifest: manifestIdentity,
    correctnessManifest: correctnessIdentity(loadedCorrectness),
    claimable: claimMode,
    resolution: view,
    reference: correctness.reference,
    thresholds: correctness.thresholds,
    pass: rows.every((row) => row.assessment.pass),
    rows,
    runtime: referenceRuntime(),
  };
}

await mkdir(EVALDIR, { recursive: true });
if (correctnessReport) {
  await writeFile(
    join(EVALDIR, `estimator-correctness.v${correctness.schemaVersion}.json`),
    JSON.stringify(correctnessReport, null, 2),
  );
  const passing = correctnessReport.rows.filter((row) => row.assessment.pass).length;
  console.log(`\nEstimator correctness: ${passing}/${correctnessReport.rows.length} rows passed.`);
}
const settings = {
  suite,
  resolution: view,
  renderScale,
  bounces,
  denoise,
  timing,
  timingWarmup,
  refFrames,
  refBounces,
  repeats,
  repeatPolicy: claimMode ? {
    staticQualitySeeds: manifest.evaluation.qualitySeeds.static,
    motionQualitySeeds: manifest.evaluation.qualitySeeds.motion,
    minimumTimingRepeats: manifest.capture.timing.minimumRepeats,
  } : {
    diagnosticRepeats: repeats,
    seedStride: manifest.evaluation.diagnosticSeedStride,
  },
  scenarios: scenarioNames,
  configs: configNames,
  frameList,
  wantFlip,
  claim: {
    enabled: claimMode,
    claimable: claimMode && (!referenceValidation || referenceValidation.pass),
    manifestPath,
    manifest: manifestIdentity,
  },
  referenceValidation,
  correctness: correctnessMode ? {
    manifestPath: correctnessManifestPath,
    manifest: correctnessIdentity(loadedCorrectness),
    report: `estimator-correctness.v${correctness.schemaVersion}.json`,
    pass: correctnessReport.pass,
  } : null,
  gpu: adapterDiagnostics,
  runtime: {
    node: process.version,
    platform: process.platform,
    arch: process.arch,
    browserVersion,
    playwrightVersion: playwrightPackage.version,
    metrics: metricRuntime,
    argv: process.argv.slice(2),
  },
};
await writeFile(join(EVALDIR, 'results.json'), JSON.stringify({ settings, results, summaries }, null, 2));
const csv = [
  'scenario,config,frames,repeat,seed,flip,hdrMse,hdrRmse,hdrRelativeRmse,hdrPsnrPeak,meanAbsRelative,maxChannelMeanRelativeDelta,referenceMeanR,referenceMeanG,referenceMeanB,testMeanR,testMeanG,testMeanB,pixels,megapixels,wallMsPerFrame,wallFps,wallRunMs,gpuMsPerFrame,gpuFps,gpuMsPerMegapixel,gpuRunMs,timingFrames,dominantPass,dominantPassPercent,pathtraceMs,reuseTemporalMs,reuseSpatialMs,dupmapMs,temporalMs,atrousMs,pathtracePct,reuseTemporalPct,reuseSpatialPct,dupmapPct,temporalPct,atrousPct',
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
      r.seed,
      r.flip ?? '',
      r.hdrMse,
      r.hdrRmse,
      r.hdrRelativeRmse,
      r.hdrPsnrPeak,
      r.meanAbsRelative,
      r.maxChannelMeanRelativeDelta,
      ...(r.referenceMeanRgb || ['', '', '']),
      ...(r.testMeanRgb || ['', '', '']),
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
if (referenceValidation && !referenceValidation.pass) {
  const reportName = claimMode
    ? `reference-convergence.v${manifest.schemaVersion}.json`
    : 'reference-convergence.json';
  console.error(`\nReference convergence failed. See test/eval/${reportName}.`);
  process.exitCode = 1;
}
if (correctnessReport && !correctnessReport.pass) {
  const reportName = `estimator-correctness.v${correctness.schemaVersion}.json`;
  console.error(`\nEstimator correctness failed. See test/eval/${reportName}.`);
  process.exitCode = 1;
}
console.log(`\nWrote ${join(EVALDIR, 'results.json')}, ${join(EVALDIR, 'results.csv')} and ${join(EVALDIR, 'summary.csv')}`);
