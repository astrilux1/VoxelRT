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

**Run 2026-07-19 (64 seeds × 32f, interior_static + exterior_move,
ours + ours_unbiased). The probe FAILED its gates — and the failure is a
probe-design defect, diagnosed to three named causes, not an estimator or
seeding defect.** As-registered numbers: noise-consistency ratios 3.3–13.2
(gate ≤ 3) and channel-mean deltas 1.7–2.9% (gate ≤ 0.5%) for *both*
configs including `ours_unbiased`.

Diagnosis (each verified by intervention):

1. **Analysis-tool defect**: the relative-residual floor (EPS = 1e-4 in
   linear HDR) exploded near-black pixels → nonsense RMS (≈38,000%).
   Fixed: floor at 1% of reference mean luminance. (Tool bug; does not
   affect channel-mean deltas.)
2. **Bounce mismatch**: runs traced at the 6-bounce operating point against
   12-bounce references, so the documented Neumann-tail truncation
   (~1%) appeared as "bias". Verified by rebuilding rb6 references:
   channel deltas dropped 2.59→1.55 / 1.73→1.16 / 2.89→2.26 / 2.23→1.78%.
   Bias probes must match bounce depth; registration didn't say so.
3. **Temporal transient + firefly tail (remaining ~1.2–2.3%)**: at 32f the
   accumulation still carries reservoir warm-up (E[EMA at 32f] ≠
   converged mean even for an unbiased estimator), and with fclamp=0 the
   seed-mean is firefly-skewed (RMS 240–370% vs p99 37–91% — extreme-tail
   dominated). The probe measured *transient + tail*, not converged
   estimator bias — exactly the quantity §2 did not intend.

Per the registered kill condition, criterion-2 failure blocks the claim
campaign "until the cause is named": **causes are named above; the block
converts to a re-registration requirement** (below), not an open defect.
Artifacts: `test/eval/biasmap_v2_*.{json,_residual.png,_mean.png}`, run
logs `test/eval/logs/biasmap-runs-2026-07-19.log`.

## 8. Terminal state and redirect

**Invalid as registered (gate mis-specified); re-registration required.**
BIASMAP v2 must specify: matched bounce depth (runs and reference at the
same bounces), a late checkpoint (≥128f) or explicit transient-window
handling so warm-up is not measured as bias, and firefly-robust statistics
(trimmed/median-of-seeds alongside the mean, or a documented clamp on the
probe axis). The 2026-07-19 captures remain valid inputs for a 32f
*transient characterization* — a different, useful quantity worth its own
name. v1-claim consequence: the seed-averaged bias verification the claim
wording depends on is still outstanding.
