import { createHash } from 'node:crypto';
import { readFile } from 'node:fs/promises';

function assert(condition, message) {
  if (!condition) throw new Error(`invalid claim manifest: ${message}`);
}

function same(a, b) {
  return JSON.stringify(a) === JSON.stringify(b);
}

// The manifest identity is a hash of its *content*, not of whatever bytes a
// particular checkout produced. Without this normalization a CRLF-converting
// clone (git core.autocrlf=true on Windows) yields a different digest for an
// identical manifest, which fails the gate closed and orphans every reference
// cache keyed by the old digest. LF is the canonical form; .gitattributes
// keeps the working tree matching it too.
export function canonicalBytes(raw) {
  return Buffer.from(raw.toString('utf8').replace(/\r\n/g, '\n'), 'utf8');
}

export async function loadClaimManifest(path) {
  const raw = canonicalBytes(await readFile(path));
  const data = JSON.parse(raw.toString('utf8'));
  validateClaimManifest(data);
  return {
    path,
    data,
    sha256: createHash('sha256').update(raw).digest('hex'),
  };
}

export function validateClaimManifest(m) {
  assert(m?.schemaVersion === 1, 'schemaVersion must be 1');
  assert(typeof m.id === 'string' && m.id.length > 0, 'id is required');
  assert(Number.isInteger(m.capture?.resolution?.width), 'capture.resolution.width must be an integer');
  assert(Number.isInteger(m.capture?.resolution?.height), 'capture.resolution.height must be an integer');
  assert(Number.isInteger(m.capture?.operatingBounces), 'capture.operatingBounces must be an integer');
  assert(m.capture?.fireflyClamp === 0, 'capture.fireflyClamp must be 0 for claim estimators');
  assert(m.capture?.hdrFormat === 'rgb-f32le-linear', 'capture.hdrFormat must be rgb-f32le-linear');
  assert(m.capture?.timing?.enabled === true, 'claim timing must be enabled');
  assert(m.capture?.timing?.minimumRepeats >= 5, 'timing.minimumRepeats must be at least 5');
  assert(Number.isInteger(m.reference?.targetFrames), 'reference.targetFrames must be an integer');
  assert(m.reference?.bounces > m.capture.operatingBounces, 'reference bounces must exceed operating bounces');
  assert(Array.isArray(m.reference?.convergence?.primaryFrames) &&
    m.reference.convergence.primaryFrames.length === 2,
  'reference.convergence.primaryFrames must contain two checkpoints');
  assert(m.reference.convergence.primaryFrames[1] === m.reference.targetFrames,
    'the upper primary convergence checkpoint must equal reference.targetFrames');
  assert(m.reference.convergence.extensionFrame > m.reference.targetFrames,
    'reference.convergence.extensionFrame must exceed targetFrames');
  assert(Object.keys(m.scenarios || {}).length === 6, 'exactly six claim scenarios are required');
  for (const [name, scenario] of Object.entries(m.scenarios)) {
    assert(scenario.pose && ['px', 'py', 'pz', 'yaw', 'pitch'].every((k) => Number.isFinite(scenario.pose[k])),
      `${name}.pose is incomplete`);
    assert(Number.isInteger(scenario.referenceFrame) && scenario.referenceFrame > 0,
      `${name}.referenceFrame must be positive`);
    if (name.endsWith('_move')) assert(scenario.motion?.amplitude > 0, `${name}.motion is required`);
  }
  assert(m.configurations?.base === 'preset=base&fclamp=0',
    'base configuration must be preset=base&fclamp=0');
  assert(typeof m.configurations?.ours_unbiased === 'string', 'ours_unbiased configuration is required');
  for (const [name, query] of Object.entries(m.configurations || {})) {
    assert(query.includes(`fclamp=${m.capture.fireflyClamp}`),
      `${name} must explicitly freeze fclamp=${m.capture.fireflyClamp}`);
  }
  assert(Array.isArray(m.evaluation?.qualitySeeds?.static) &&
    m.evaluation.qualitySeeds.static.length >= 16,
  'at least 16 static quality seeds are required');
  assert(Array.isArray(m.evaluation?.qualitySeeds?.motion) &&
    m.evaluation.qualitySeeds.motion.length >= 32,
  'at least 32 motion quality seeds are required');
  assert(new Set(m.evaluation.qualitySeeds.static).size === m.evaluation.qualitySeeds.static.length,
    'static quality seeds must be unique');
  assert(new Set(m.evaluation.qualitySeeds.motion).size === m.evaluation.qualitySeeds.motion.length,
    'motion quality seeds must be unique');
  assert(Array.isArray(m.runtime?.requiredFeatures) &&
    m.runtime.requiredFeatures.includes('float32-filterable') &&
    m.runtime.requiredFeatures.includes('timestamp-query'),
  'runtime.requiredFeatures must include float32-filterable and timestamp-query');
  return m;
}

export function claimIdentity(loaded) {
  return {
    id: loaded.data.id,
    schemaVersion: loaded.data.schemaVersion,
    sha256: loaded.sha256,
  };
}

export function claimSettingsProblems(m, settings, profile = {}) {
  const problems = [];
  const expect = (actual, expected, label) => {
    if (!same(actual, expected)) problems.push(`${label}: expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`);
  };
  const expectedBounces = profile.bounces ?? m.capture.operatingBounces;
  const allowedScenarios = new Set(profile.scenarios ?? Object.keys(m.scenarios));
  const allowedFrames = new Set(profile.frameCheckpoints ?? m.evaluation.frameCheckpoints);
  const allowedConfigurations = profile.configurations ?? m.configurations;
  expect(settings.resolution, m.capture.resolution, 'resolution');
  expect(String(settings.renderScale), String(m.capture.renderScale), 'render scale');
  expect(Number(settings.bounces), expectedBounces, 'operating bounces');
  expect(String(settings.denoise), String(m.capture.denoise), 'denoise');
  expect(settings.timing, m.capture.timing.enabled, 'timing enabled');
  expect(settings.timingWarmup, m.capture.timing.warmupFrames, 'timing warmup');
  expect(settings.refBounces, m.reference.bounces, 'reference bounces');
  expect(settings.refFrames, m.reference.targetFrames, 'reference frames');
  expect(settings.wantFlip, true, 'HDR-FLIP requested');
  expect(settings.launchArgs, m.runtime.launchArgs, 'Chromium launch args');
  expect(settings.playwrightVersion, m.runtime.playwrightVersion, 'Playwright version');
  expect(settings.browserVersion, m.runtime.chromiumVersion, 'Chromium version');
  expect(settings.flipEvaluatorVersion, m.runtime.flipEvaluatorVersion, 'flip-evaluator version');
  for (const scenario of settings.scenarios) {
    if (!allowedScenarios.has(scenario)) problems.push(`scenario is not frozen in the active profile: ${scenario}`);
  }
  for (const frame of settings.frameList) {
    if (!allowedFrames.has(frame)) problems.push(`frame checkpoint is not frozen: ${frame}`);
  }
  for (const [name, query] of Object.entries(settings.configQueries)) {
    if (!allowedConfigurations[name]) problems.push(`configuration is not frozen in the active profile: ${name}`);
    else expect(query, allowedConfigurations[name], `configuration ${name}`);
  }
  if (profile.requireExact) {
    expect(settings.scenarios, profile.scenarios, 'profile scenarios');
    expect(settings.frameList, profile.frameCheckpoints, 'profile frame checkpoints');
    expect(Object.keys(settings.configQueries), Object.keys(profile.configurations),
      'profile configurations');
  }
  return problems;
}

export function referenceMetadataMismatches(actual, expected) {
  const mismatches = [];
  for (const [key, value] of Object.entries(expected)) {
    if (!same(actual?.[key], value)) mismatches.push(key);
  }
  return mismatches;
}

export function assessReferenceConvergence(metrics, thresholds) {
  const channelDelta = Number(metrics.maxChannelMeanRelativeDelta);
  const psnr = Number(metrics.hdrPsnrPeak);
  const reasons = [];
  if (!Number.isFinite(channelDelta) || channelDelta > thresholds.maxChannelMeanRelativeDelta) {
    reasons.push(`max channel-mean delta ${channelDelta} exceeds ${thresholds.maxChannelMeanRelativeDelta}`);
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
