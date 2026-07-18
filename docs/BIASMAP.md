# Seed-averaged bias maps (no new flag) — design and result

One-paragraph summary: adapt Lin 2026 Fig. 14's strongest verification tool —
averaging many independent-seed runs so noise cancels and *spatially
structured* bias remains — to quantify the bias of the default `ours` preset
and to verify that `ours_unbiased` is actually unbiased at the per-pixel
level, not just in channel means. Terminal state: **pre-registered, not yet
run** (waiting on the benchmark machine after the baseline campaign).

This is a measurement probe, not a flag experiment: it adds no shader code,
pass, buffer, or estimator term. It is pre-registered anyway because its
thresholds gate how the final claim may be worded, and thresholds chosen
after seeing the maps are worthless.

## 1. Hypothesis

The biased components of `ours` (duplication-map cCap reduction, reservoir
contribution clamp) produce spatially structured bias that channel-mean energy
gates cannot see, because channel means average over the image: a −5% dark
trail in a disocclusion region and a +5% bright region elsewhere can cancel to
zero. Conversely, `ours_unbiased`'s seed-averaged residual is consistent with
pure Monte Carlo noise reduced by √M. This is exactly the failure mode Lin
2026 exposes in their Fig. 14 (dark/bright trails under motion after 1024-run
averaging) and the correctness manifest's channel-mean gate is blind to it.

## 2. Denominator

Per-pixel relative residual of the **mean image over M = 64 independent-seed
runs** against the frozen 1600-frame reference, at a fixed 32-frame
checkpoint, 1920×1080, scale 1, operating bounces 6, denoise off, fclamp 0.
Configurations: `ours` and `ours_unbiased` (the pair whose difference *is* the
intentional bias). Scenarios: `interior_static` (static: isolates estimator
bias) and `exterior_move` (motion: where disocclusion-trail bias lives).
Seeds: `i * diagnosticSeedStride` for `i = 0..63` from the claim manifest's
stride, so the run is reproducible from the manifest alone. Equal frame count
is correct here — this measures bias, not performance.

## 3. Design and invariants

No estimator changes. Harness only:

1. `bench.mjs` runs with `--repeats 64` at the fixed checkpoint; each run
   already writes linear HDR `.rgbf32` per repeat.
2. A non-GPU averaging script streams the 64 HDR buffers, accumulates a mean
   image in float64, and emits: (a) mean image PNG, (b) signed relative
   residual map `(mean − ref) / max(ref, ε)` on a fixed ±10% color scale
   (fixed scale, as in the paper, so maps are comparable across configs),
   (c) summary stats: channel-mean relative delta, RMS relative residual,
   99th-percentile |relative residual|, and the fraction of pixels beyond
   ±2%.
3. Noise-consistency invariant for `ours_unbiased`: the per-pixel stddev
   across the 64 runs predicts the residual RMS of the mean as
   `stddev_pixel / √64`. Observed residual RMS beyond 3× that prediction
   indicates either hidden bias or a harness bug (e.g. seed not actually
   varying), and must be resolved before the maps are interpreted.
4. Determinism invariant: two runs with the same seed produce byte-identical
   HDR output (already relied on by the reference cache; spot-checked here
   with one duplicate-seed pair before the 64-run batch).

## 4. Cost budget

GPU: 64 runs × (32 frames × ~35 ms + ~2.5 s launch overhead) ≈ 4 min per
config/scenario; 2 configs × 2 scenarios ≈ **16 min** total on the RTX 3080.
Disk: 64 × 24.9 MB HDR per config/scenario would be 1.6 GB; the averaging
script streams and discards, keeping only the mean image and maps (~75 MB
total). No new GPU memory.

## 5. Acceptance criteria

1. Determinism: duplicate-seed pair is byte-identical
   (`cmp` on the two `.rgbf32` files).
2. `ours_unbiased` noise consistency: observed mean-image residual RMS ≤ 3×
   the √M prediction on both scenarios, and |channel-mean relative delta| ≤
   0.5% per channel (the frozen correctness tolerance).
3. `ours` bias classification, per scenario:
   - **small**: |channel-mean rel delta| ≤ 0.5% AND 99th-percentile
     |relative residual| ≤ 5% → claim wording may describe the default as
     "bias below X%" with the maps as evidence.
   - **material**: anything worse → the final claim must lead with the
     unbiased variant, and the biased default's numbers are reported
     separately (paper §7.4 does exactly this split).
   No threshold may be moved after the first 64-run batch.

## 6. Kill condition and budget

If criterion 2 fails after one day of investigation, the claim campaign is
**blocked** — a failed noise-consistency check means either `ours_unbiased`
is not unbiased or the harness's seeding is broken, and every downstream
number is suspect until the cause is named. This probe cannot be "killed" in
the promotion sense; it either produces maps or exposes a defect. Budget: one
benchmark-machine session (≤ 1 hour GPU) plus one day of analysis.

## 7. Results

Not yet run.

## 8. Terminal state and redirect

Pre-registered 2026-07-18. Runs after the baseline campaign (PLAN §7.6)
completes on the benchmark machine.
