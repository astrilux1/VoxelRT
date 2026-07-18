// Non-GPU analysis over test/eval/results.json: turns benchmark output into
// the paper-parity artifacts the 2-3x claim is judged on (Lin 2026 §7):
//
//   1. Equal-time convergence curves (paper Fig. 15b): HDR-FLIP versus
//      cumulative GPU milliseconds per scenario/config, remapped from the
//      frame-checkpoint ladder.
//   2. Equal-error speedups ("Nx faster at matched HDR-FLIP") and equal-time
//      error ratios ("Nx lower FLIP at matched GPU ms") for chosen config
//      pairs, interpolated in log-log space, reported per scenario plus the
//      cross-scenario range (the paper reports 2.08x-3.05x, not one number).
//   3. Additive cost table (paper Table 1): with --cost-table, per-config
//      total and per-pass GPU ms with deltas row over row, in the config
//      order the suite ran (meant for --suite ablation results).
//
// Usage:
//   node test/analyze.mjs                         # curves + ratios
//   node test/analyze.mjs --pairs ours:lin,ours:base
//   node test/analyze.mjs --cost-table            # Table-1 style report
//   node test/analyze.mjs --results path.json --out-stem name
//
// Frame checkpoints below the timing warmup carry no GPU timing; their
// cumulative time is estimated from the nearest timed checkpoint's ms/frame
// for the same scenario/config and flagged `estimated` in the output. This
// keeps per-frame cost differences between configs honest while not silently
// dropping the early points the convergence story needs.

import { readFile, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { getArg, hasFlag } from './gpu-launch.mjs';

const ROOT = fileURLToPath(new URL('..', import.meta.url));
const EVALDIR = join(ROOT, 'test', 'eval');
const args = process.argv.slice(2);
const resultsPath = getArg(args, '--results', join(EVALDIR, 'results.json'));
const outStem = getArg(args, '--out-stem', 'analysis');
const metricName = getArg(args, '--metric', 'flip');
const costTable = hasFlag(args, '--cost-table');
const pairSpec = getArg(args, '--pairs', 'ours:lin,ours:base,lin:base');

const data = JSON.parse(await readFile(resultsPath, 'utf8'));
const { settings, results, summaries } = data;
if (!Array.isArray(summaries) || !summaries.length) {
  throw new Error(`${resultsPath} has no summaries; run bench first`);
}

const metricOf = (s) => (metricName === 'flip'
  ? (s.metric?.name === 'flip' ? s.metric.mean : null)
  : (s.metric?.name === 'hdrMse' ? s.metric.mean : null));

// --- Curves: cumulative GPU time per (scenario, config) ---------------------

const curves = new Map(); // scenario -> config -> [{frames, metric, msPerFrame, cumGpuMs, estimated}]
for (const s of summaries) {
  const metric = metricOf(s);
  if (metric == null) continue;
  if (!curves.has(s.scenario)) curves.set(s.scenario, new Map());
  const byConfig = curves.get(s.scenario);
  if (!byConfig.has(s.config)) byConfig.set(s.config, []);
  byConfig.get(s.config).push({
    frames: s.frames,
    metric,
    msPerFrame: s.gpuMsPerFrame?.mean ?? null,
    gpuCv: s.gpuMsPerFrame?.cv ?? null,
    repeats: s.repeats,
  });
}
for (const byConfig of curves.values()) {
  for (const points of byConfig.values()) {
    points.sort((a, b) => a.frames - b.frames);
    // Untimed checkpoints borrow ms/frame from the nearest timed one.
    for (const p of points) {
      if (p.msPerFrame != null) { p.estimated = false; continue; }
      const timed = points.filter((q) => q.msPerFrame != null);
      if (!timed.length) continue;
      const nearest = timed.reduce((best, q) =>
        Math.abs(q.frames - p.frames) < Math.abs(best.frames - p.frames) ? q : best);
      p.msPerFrame = nearest.msPerFrame;
      p.estimated = true;
    }
    for (const p of points) {
      p.cumGpuMs = p.msPerFrame != null ? p.msPerFrame * p.frames : null;
    }
  }
}

// --- Interpolation in log-log space -----------------------------------------
// Convergence is near power-law, so piecewise-linear in (log t, log e) is the
// faithful interpolant. Points outside the sampled range return null rather
// than extrapolating: a claim must not rest on extrapolated tails.

function logInterp(points, xKey, yKey, x) {
  const ps = points
    .filter((p) => p[xKey] != null && p[yKey] != null && p[xKey] > 0 && p[yKey] > 0)
    .sort((a, b) => a[xKey] - b[xKey]);
  if (ps.length < 2 || x < ps[0][xKey] || x > ps[ps.length - 1][xKey]) return null;
  for (let i = 1; i < ps.length; i++) {
    if (x <= ps[i][xKey]) {
      const [a, b] = [ps[i - 1], ps[i]];
      const t = (Math.log(x) - Math.log(a[xKey])) / (Math.log(b[xKey]) - Math.log(a[xKey]));
      return Math.exp(Math.log(a[yKey]) + t * (Math.log(b[yKey]) - Math.log(a[yKey])));
    }
  }
  return null;
}

// Metric should decrease with time; warn where the ladder disagrees so a
// non-converging config cannot silently produce nonsense ratios.
const monotonicityWarnings = [];
for (const [scenario, byConfig] of curves) {
  for (const [config, points] of byConfig) {
    const ordered = points.filter((p) => p.cumGpuMs != null);
    for (let i = 1; i < ordered.length; i++) {
      if (ordered[i].metric > ordered[i - 1].metric * 1.02) {
        monotonicityWarnings.push(
          `${scenario}/${config}: ${metricName} rises ${ordered[i - 1].metric.toFixed(4)} -> ` +
          `${ordered[i].metric.toFixed(4)} between ${ordered[i - 1].frames}f and ${ordered[i].frames}f`);
      }
    }
  }
}

// --- Pairwise equal-time / equal-error comparisons --------------------------

const pairs = pairSpec.split(',').map((p) => p.trim()).filter(Boolean).map((p) => {
  const [test, ref] = p.split(':');
  return { test, ref };
});

const GRID_STEPS = 9;
function logGrid(lo, hi, n = GRID_STEPS) {
  const out = [];
  for (let i = 0; i < n; i++) out.push(Math.exp(Math.log(lo) + (i / (n - 1)) * (Math.log(hi) - Math.log(lo))));
  return out;
}

function comparePair(scenario, testPoints, refPoints) {
  const usable = (ps) => ps.filter((p) => p.cumGpuMs != null && p.metric > 0);
  const t = usable(testPoints);
  const r = usable(refPoints);
  if (t.length < 2 || r.length < 2) return null;
  const range = (ps, key) => [Math.min(...ps.map((p) => p[key])), Math.max(...ps.map((p) => p[key]))];

  // Equal error: at shared metric levels, how much less time does test need?
  const [tLoE, tHiE] = range(t, 'metric');
  const [rLoE, rHiE] = range(r, 'metric');
  const eLo = Math.max(tLoE, rLoE);
  const eHi = Math.min(tHiE, rHiE);
  const equalError = [];
  if (eHi > eLo) {
    for (const e of logGrid(eLo, eHi)) {
      const tTime = logInterp(t, 'metric', 'cumGpuMs', e);
      const rTime = logInterp(r, 'metric', 'cumGpuMs', e);
      if (tTime != null && rTime != null) {
        equalError.push({ [metricName]: e, testMs: tTime, refMs: rTime, speedup: rTime / tTime });
      }
    }
  }

  // Equal time: at shared cumulative-GPU-ms levels, how much lower is the error?
  const [tLoT, tHiT] = range(t, 'cumGpuMs');
  const [rLoT, rHiT] = range(r, 'cumGpuMs');
  const tLo = Math.max(tLoT, rLoT);
  const tHi = Math.min(tHiT, rHiT);
  const equalTime = [];
  if (tHi > tLo) {
    for (const ms of logGrid(tLo, tHi)) {
      const tErr = logInterp(t, 'cumGpuMs', 'metric', ms);
      const rErr = logInterp(r, 'cumGpuMs', 'metric', ms);
      if (tErr != null && rErr != null) {
        equalTime.push({ cumGpuMs: ms, testMetric: tErr, refMetric: rErr, errorRatio: rErr / tErr });
      }
    }
  }
  return { equalError, equalTime };
}

const ratioStats = (xs) => (xs.length ? {
  n: xs.length,
  min: Math.min(...xs),
  median: [...xs].sort((a, b) => a - b)[Math.floor((xs.length - 1) / 2)],
  max: Math.max(...xs),
} : null);

const comparisons = [];
for (const { test, ref } of pairs) {
  const perScenario = [];
  for (const [scenario, byConfig] of curves) {
    if (!byConfig.has(test) || !byConfig.has(ref)) continue;
    const cmp = comparePair(scenario, byConfig.get(test), byConfig.get(ref));
    if (!cmp) continue;
    perScenario.push({
      scenario,
      equalError: cmp.equalError,
      equalTime: cmp.equalTime,
      equalErrorSpeedup: ratioStats(cmp.equalError.map((x) => x.speedup)),
      equalTimeErrorRatio: ratioStats(cmp.equalTime.map((x) => x.errorRatio)),
    });
  }
  if (!perScenario.length) continue;
  comparisons.push({
    test,
    ref,
    scenarios: perScenario,
    // Cross-scenario range of the per-scenario median speedups - the honest
    // "2.08x-3.05x"-style claim shape.
    overall: {
      equalErrorSpeedup: ratioStats(perScenario.map((s) => s.equalErrorSpeedup?.median).filter((x) => x != null)),
      equalTimeErrorRatio: ratioStats(perScenario.map((s) => s.equalTimeErrorRatio?.median).filter((x) => x != null)),
    },
  });
}

// --- Additive cost table (paper Table 1 shape) ------------------------------

let costRows = null;
if (costTable) {
  const configOrder = settings?.configs ?? [...new Set(results.map((r) => r.config))];
  costRows = [];
  let prev = null;
  for (const config of configOrder) {
    const rows = results.filter((r) => r.config === config && r.perf?.gpuMsPerFrame != null);
    if (!rows.length) continue;
    const total = rows.reduce((s, r) => s + r.perf.gpuMsPerFrame, 0) / rows.length;
    const passes = {};
    for (const r of rows) {
      for (const [name, pct] of Object.entries(r.perf.gpuPassPercent ?? {})) {
        passes[name] = (passes[name] ?? 0) + (pct / 100) * r.perf.gpuMsPerFrame / rows.length;
      }
    }
    costRows.push({
      config,
      scenarios: [...new Set(rows.map((r) => r.scenario))],
      rows: rows.length,
      totalGpuMsPerFrame: total,
      deltaVsPrevious: prev == null ? null : total - prev,
      passes,
    });
    prev = total;
  }
}

// --- Report -----------------------------------------------------------------

const fmt = (x, d = 3) => (x == null ? 'n/a' : x.toFixed(d));
const lines = [];
lines.push(`# Benchmark analysis (${metricName}, ${new Date(
  (await import('node:fs')).statSync(resultsPath).mtimeMs).toISOString()})`);
lines.push('');
lines.push(`Source: ${resultsPath} | suite ${settings?.suite} | ` +
  `${settings?.resolution?.width}x${settings?.resolution?.height} scale ${settings?.renderScale} | ` +
  `bounces ${settings?.bounces} | repeats ${settings?.repeats}`);
lines.push('');

if (monotonicityWarnings.length) {
  lines.push('## Non-monotone convergence warnings');
  lines.push('');
  for (const w of monotonicityWarnings) lines.push(`- ${w}`);
  lines.push('');
}

lines.push('## Equal-time convergence curves (cumulative GPU ms)');
lines.push('');
for (const [scenario, byConfig] of curves) {
  lines.push(`### ${scenario}`);
  lines.push('');
  lines.push(`| config | ` + [...byConfig.values()][0].map((p) => `${p.frames}f`).join(' | ') + ' |');
  lines.push('|---|' + [...byConfig.values()][0].map(() => '---').join('|') + '|');
  for (const [config, points] of byConfig) {
    lines.push(`| ${config} | ` + points.map((p) =>
      `${fmt(p.metric, 4)} @ ${p.cumGpuMs == null ? '?' : (p.cumGpuMs / 1000).toFixed(2)}s${p.estimated ? '*' : ''}`,
    ).join(' | ') + ' |');
  }
  lines.push('');
}
lines.push('`*` cumulative time estimated from the nearest timed checkpoint (frames <= timing warmup).');
lines.push('');

lines.push('## Pairwise claims');
lines.push('');
for (const c of comparisons) {
  lines.push(`### ${c.test} vs ${c.ref}`);
  lines.push('');
  lines.push(`| scenario | equal-${metricName} speedup (min / median / max) | equal-time ${metricName} ratio (min / median / max) |`);
  lines.push('|---|---|---|');
  for (const s of c.scenarios) {
    const se = s.equalErrorSpeedup;
    const st = s.equalTimeErrorRatio;
    lines.push(`| ${s.scenario} | ${se ? `${fmt(se.min, 2)}x / ${fmt(se.median, 2)}x / ${fmt(se.max, 2)}x` : 'n/a'} | ` +
      `${st ? `${fmt(st.min, 2)}x / ${fmt(st.median, 2)}x / ${fmt(st.max, 2)}x` : 'n/a'} |`);
  }
  const oe = c.overall.equalErrorSpeedup;
  const ot = c.overall.equalTimeErrorRatio;
  lines.push(`| **overall range** | ${oe ? `**${fmt(oe.min, 2)}x - ${fmt(oe.max, 2)}x**` : 'n/a'} | ` +
    `${ot ? `**${fmt(ot.min, 2)}x - ${fmt(ot.max, 2)}x**` : 'n/a'} |`);
  lines.push('');
}
lines.push(`Speedup > 1 means \`test\` needs less GPU time for the same ${metricName}; ` +
  `error ratio > 1 means \`test\` reaches lower ${metricName} in the same GPU time. ` +
  'Ratios are interpolated log-log within the sampled range only - no extrapolation.');
lines.push('');

if (costRows) {
  lines.push('## Additive cost table (GPU ms/frame, averaged over scenarios and repeats)');
  lines.push('');
  const passNames = [...new Set(costRows.flatMap((r) => Object.keys(r.passes)))];
  lines.push(`| config | total | delta | ` + passNames.join(' | ') + ' |');
  lines.push('|---|---|---|' + passNames.map(() => '---').join('|') + '|');
  for (const r of costRows) {
    lines.push(`| ${r.config} | ${fmt(r.totalGpuMsPerFrame, 2)} | ` +
      `${r.deltaVsPrevious == null ? '-' : (r.deltaVsPrevious >= 0 ? '+' : '') + fmt(r.deltaVsPrevious, 2)} | ` +
      passNames.map((n) => fmt(r.passes[n], 2)).join(' | ') + ' |');
  }
  lines.push('');
}

const report = {
  source: resultsPath,
  metric: metricName,
  settings: {
    suite: settings?.suite,
    resolution: settings?.resolution,
    renderScale: settings?.renderScale,
    bounces: settings?.bounces,
    repeats: settings?.repeats,
    gpu: settings?.gpu,
  },
  monotonicityWarnings,
  curves: Object.fromEntries([...curves].map(([scenario, byConfig]) =>
    [scenario, Object.fromEntries(byConfig)])),
  comparisons,
  costTable: costRows,
};

const mdPath = join(EVALDIR, `${outStem}.md`);
const jsonPath = join(EVALDIR, `${outStem}.json`);
await writeFile(mdPath, lines.join('\n'));
await writeFile(jsonPath, JSON.stringify(report, null, 2));
console.log(lines.join('\n'));
console.error(`\nWrote ${mdPath} and ${jsonPath}`);
