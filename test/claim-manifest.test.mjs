import assert from 'node:assert/strict';
import test from 'node:test';
import { fileURLToPath } from 'node:url';
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
  loadCorrectnessManifest,
} from './correctness-manifest.mjs';

const manifestPath = fileURLToPath(new URL('./eval/claim-manifest.v1.json', import.meta.url));
const correctnessPath = fileURLToPath(new URL('./eval/correctness-manifest.v1.json', import.meta.url));

test('claim manifest loads with a stable identity and complete seed budgets', async () => {
  const loaded = await loadClaimManifest(manifestPath);
  assert.equal(claimIdentity(loaded).sha256.length, 64);
  assert.equal(loaded.data.evaluation.qualitySeeds.static.length, 16);
  assert.equal(loaded.data.evaluation.qualitySeeds.motion.length, 32);
});

test('claim settings reject a drifted operating bounce count', async () => {
  const { data: m } = await loadClaimManifest(manifestPath);
  const settings = {
    resolution: m.capture.resolution,
    renderScale: m.capture.renderScale,
    bounces: 2,
    denoise: m.capture.denoise,
    timing: true,
    timingWarmup: m.capture.timing.warmupFrames,
    refBounces: m.reference.bounces,
    refFrames: m.reference.targetFrames,
    wantFlip: true,
    launchArgs: m.runtime.launchArgs,
    playwrightVersion: m.runtime.playwrightVersion,
    browserVersion: m.runtime.chromiumVersion,
    flipEvaluatorVersion: m.runtime.flipEvaluatorVersion,
    scenarios: ['interior_static'],
    frameList: [32],
    configQueries: { ours: 'preset=ours' },
  };
  assert.match(claimSettingsProblems(m, settings).join('\n'), /operating bounces/);
});

test('reference cache comparison includes manifest and evaluation-pose identity', () => {
  const expected = { scenario: 'interior_move', evaluationFrame: 64, claimManifest: { sha256: 'abc' } };
  const actual = { ...expected, evaluationFrame: 96 };
  assert.deepEqual(referenceMetadataMismatches(actual, expected), ['evaluationFrame']);
});

test('reference convergence applies both energy and HDR-PSNR gates', () => {
  const thresholds = { maxChannelMeanRelativeDelta: 0.005, minHdrPsnrPeakDb: 30 };
  assert.equal(assessReferenceConvergence({ maxChannelMeanRelativeDelta: 0.004, hdrPsnrPeak: 31 }, thresholds).pass, true);
  assert.equal(assessReferenceConvergence({ maxChannelMeanRelativeDelta: 0.006, hdrPsnrPeak: 31 }, thresholds).pass, false);
  assert.equal(assessReferenceConvergence({ maxChannelMeanRelativeDelta: 0.004, hdrPsnrPeak: 29 }, thresholds).pass, false);
});

test('correctness manifest is bound to the frozen claim and unbiased configurations', async () => {
  const claim = await loadClaimManifest(manifestPath);
  const correctness = await loadCorrectnessManifest(correctnessPath);
  assert.deepEqual(
    correctnessClaimProblems(correctness.data, claim.data, claimIdentity(claim)),
    [],
  );
  assert.match(correctness.data.configurations.lin_unbiased, /dupmap=0/);
  assert.match(correctness.data.configurations.ours_unbiased, /rclamp=0/);
});

test('estimator correctness applies both energy and HDR-PSNR gates', () => {
  const thresholds = { maxChannelMeanRelativeDelta: 0.01, minHdrPsnrPeakDb: 30 };
  assert.equal(assessEstimatorCorrectness(
    { maxChannelMeanRelativeDelta: 0.009, hdrPsnrPeak: 31 }, thresholds,
  ).pass, true);
  assert.equal(assessEstimatorCorrectness(
    { maxChannelMeanRelativeDelta: 0.011, hdrPsnrPeak: 31 }, thresholds,
  ).pass, false);
  assert.equal(assessEstimatorCorrectness(
    { maxChannelMeanRelativeDelta: 0.009, hdrPsnrPeak: 29 }, thresholds,
  ).pass, false);
});
