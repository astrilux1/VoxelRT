import { createHash } from 'node:crypto';
import { readFile } from 'node:fs/promises';
import { canonicalBytes } from './claim-manifest.mjs';

function assert(condition, message) {
  if (!condition) throw new Error(`invalid correctness manifest: ${message}`);
}

function same(a, b) {
  return JSON.stringify(a) === JSON.stringify(b);
}

export async function loadCorrectnessManifest(path) {
  const raw = canonicalBytes(await readFile(path));
  const data = JSON.parse(raw.toString('utf8'));
  validateCorrectnessManifest(data);
  return {
    path,
    data,
    sha256: createHash('sha256').update(raw).digest('hex'),
  };
}

export function validateCorrectnessManifest(m) {
  assert(m?.schemaVersion === 1, 'schemaVersion must be 1');
  assert(typeof m.id === 'string' && m.id.length > 0, 'id is required');
  assert(typeof m.claimManifest?.id === 'string', 'claimManifest.id is required');
  assert(m.claimManifest?.schemaVersion === 1, 'claimManifest.schemaVersion must be 1');
  assert(/^[0-9a-f]{64}$/.test(m.claimManifest?.sha256 || ''),
    'claimManifest.sha256 must be a SHA-256 digest');
  assert(Number.isInteger(m.capture?.resolution?.width), 'capture.resolution.width must be an integer');
  assert(Number.isInteger(m.capture?.resolution?.height), 'capture.resolution.height must be an integer');
  assert(Number.isInteger(m.capture?.bounces) && m.capture.bounces > 0,
    'capture.bounces must be positive');
  assert(Number.isInteger(m.capture?.frames) && m.capture.frames > 0,
    'capture.frames must be positive');
  assert(String(m.capture?.denoise) === '0', 'capture.denoise must be 0');
  assert(Number.isInteger(m.capture?.maxHistory) && m.capture.maxHistory >= m.capture.frames,
    'capture.maxHistory must cover the full correctness run');
  assert(Array.isArray(m.capture?.seeds) && m.capture.seeds.length > 0,
    'capture.seeds must be non-empty');
  assert(new Set(m.capture.seeds).size === m.capture.seeds.length,
    'capture.seeds must be unique');
  assert(Number.isInteger(m.reference?.frames) && m.reference.frames > m.capture.frames,
    'reference.frames must exceed capture.frames');
  assert(Number.isInteger(m.reference?.bounces) && m.reference.bounces > m.capture.bounces,
    'reference.bounces must exceed capture.bounces');
  assert(Array.isArray(m.scenarios) && m.scenarios.length === 3,
    'exactly three static correctness scenarios are required');
  assert(m.scenarios.every((name) => name.endsWith('_static')),
    'correctness scenarios must be static');

  const requiredConfigs = ['base', 'gi', 'unified', 'lin_unbiased', 'ours_unbiased'];
  assert(same(Object.keys(m.configurations || {}), requiredConfigs),
    `configurations must be exactly ${requiredConfigs.join(', ')}`);
  for (const [name, query] of Object.entries(m.configurations)) {
    assert(typeof query === 'string' && query.includes(`maxhist=${m.capture.maxHistory}`),
      `${name} must explicitly freeze maxhist=${m.capture.maxHistory}`);
    assert(query.includes('fclamp=0'), `${name} must disable the firefly clamp`);
    if (name !== 'base') {
      assert(query.includes('treuse=0'),
        `${name} must disable biased ReSTIR temporal reuse`);
    }
  }
  assert(m.configurations.lin_unbiased.includes('dupmap=0'),
    'lin_unbiased must disable duplication-map bias');
  assert(m.configurations.lin_unbiased.includes('footprint=0'),
    'lin_unbiased must disable the unsupported footprint rejection path');
  assert(m.configurations.ours_unbiased.includes('dupmap=0') &&
    m.configurations.ours_unbiased.includes('rclamp=0') &&
    m.configurations.ours_unbiased.includes('footprint=0'),
  'ours_unbiased must disable duplication-map, contribution-clamp, and footprint bias');
  assert(Number.isFinite(m.thresholds?.maxChannelMeanRelativeDelta) &&
    m.thresholds.maxChannelMeanRelativeDelta > 0,
  'thresholds.maxChannelMeanRelativeDelta must be positive');
  assert(Number.isFinite(m.thresholds?.minHdrPsnrPeakDb),
    'thresholds.minHdrPsnrPeakDb is required');
  return m;
}

export function correctnessIdentity(loaded) {
  return {
    id: loaded.data.id,
    schemaVersion: loaded.data.schemaVersion,
    sha256: loaded.sha256,
  };
}

export function correctnessClaimProblems(correctness, claim, claimIdentity) {
  const problems = [];
  const expect = (actual, expected, label) => {
    if (!same(actual, expected)) problems.push(
      `${label}: expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`,
    );
  };
  expect(correctness.claimManifest, claimIdentity, 'bound claim manifest');
  expect(correctness.capture.resolution, claim.capture.resolution, 'resolution');
  expect(String(correctness.capture.denoise), String(claim.capture.denoise), 'denoise');
  expect(0, claim.capture.fireflyClamp, 'firefly clamp');
  expect(correctness.reference.frames, claim.reference.targetFrames, 'reference frames');
  expect(correctness.reference.bounces, claim.reference.bounces, 'reference bounces');
  for (const scenario of correctness.scenarios) {
    if (!claim.scenarios[scenario]) problems.push(`scenario is not frozen by the claim manifest: ${scenario}`);
  }
  return problems;
}

export function assessEstimatorCorrectness(metrics, thresholds) {
  const channelDelta = Number(metrics.maxChannelMeanRelativeDelta);
  const psnr = Number(metrics.hdrPsnrPeak);
  const reasons = [];
  if (!Number.isFinite(channelDelta) || channelDelta > thresholds.maxChannelMeanRelativeDelta) {
    reasons.push(
      `max channel-mean delta ${channelDelta} exceeds ${thresholds.maxChannelMeanRelativeDelta}`,
    );
  }
  if (!Number.isFinite(psnr) || psnr < thresholds.minHdrPsnrPeakDb) {
    reasons.push(`HDR PSNR ${psnr} dB is below ${thresholds.minHdrPsnrPeakDb} dB`);
  }
  return {
    pass: reasons.length === 0,
    maxChannelMeanRelativeDelta: channelDelta,
    hdrPsnrPeakDb: psnr,
    reasons,
  };
}
