# VoxelRT Research Loop

This project is trying to beat Lin, Kettunen, and Wyman 2026 by 2-3x on
defensible terms: lower error at equal GPU time, or equal error at lower GPU
time. Treat that as a measured research claim, not a visual impression.

## Loop

1. Lock three baselines before testing a new idea:
   - `base`: plain voxel path tracing.
   - `lin`: faithful Lin 2026 adaptation.
   - `ours`: cumulative VoxelRT extensions.
2. Add one hypothesis at a time behind a flag. Keep `lin` unchanged unless the
   change is a correctness fix shared by all estimators. Any experiment that
   adds a flag, pass, buffer, or estimator term is pre-registered first:
   copy `docs/EXPERIMENT_TEMPLATE.md`, fill in the hypothesis, denominator,
   invariants, acceptance criteria, and kill condition before writing code
   (`docs/WORLDGI.md` is the worked example).
3. Start with short 1920x1080 smoke runs for compile, adapter, and gross
   signal checks. Do not use lower resolutions for research conclusions.
4. Promote only surviving ideas to fixed-seed ablations, convergence curves,
   and repeated runs.
5. Declare wins only from equal-time or equal-error comparisons, with GPU
   pass timings and HDR error metrics recorded.

## Metrics

- Use linear HDR readback before tonemapping for quantitative comparisons.
- Prefer HDR-FLIP as the headline perceptual metric; keep HDR MSE/RMSE,
  relative error, and channel means for debugging bias.
- Report GPU timestamp-query pass times separately from wall time. Each run
  should include ms/frame, FPS, ms/megapixel, cumulative GPU cost for the frame
  budget, per-pass ms, per-pass percentages, warmup frames, and timed frames.
- For repeated runs, keep mean, median, stddev, coefficient of variation, min,
  p95, and max for wall ms/frame and GPU ms/frame.
- Record adapter/device context: vendor, architecture, fallback status,
  features, limits, browser version, launch flags, OS, Node version, and
  whether timestamp queries were available.
- Capture static and deterministic camera-motion scenarios.
- Keep references frozen and cached per pose/configuration.
- Separate algorithm-only results from denoised/presented results.

## Traps

- Reuse correlation can look like convergence. Check repeated seeds and motion.
- Duplication-map cCap reduction is intentionally biased; measure it separately.
- Visibility, reconnection, and G-buffer reconstruction errors usually show up
  as energy loss before they show up as obvious artifacts.
- Firefly/contribution clamps improve early images but can hide converged bias.
- Browser timing is useful but implementation-dependent; record adapter,
  browser, driver/backend, launch flags, and timestamp-query availability.

## Paper Parity (Lin 2026 §7)

The target paper's evaluation methodology is the standard this project's
evidence is held to. Mapping of their instruments to ours, including the two
places we deliberately differ and the one place we cannot follow:

| Lin 2026 instrument | VoxelRT equivalent | Status |
|---|---|---|
| Table 1: additive per-pass cost ladder, one technique per row | `npm run bench:ablation` + `npm run analyze:cost` | adopted |
| Fig. 8: matched-parameter fairness (σ = 8ρ/9π, equal mean sample distance) | σ=16 ↔ ρ=30 defaults baked into `buildQuery` | adopted |
| Fig. 9: decorrelation vs an alternative at weak/med/strong settings | dupmap vs `?mutate=` ladder rows | adopted |
| Fig. 13: equal-frame FLIP + ms reported together, per-scene speedup *range* | `analyze.mjs` pairwise tables report min–median–max per scenario and the cross-scenario range, never one number | adopted |
| Fig. 14: 1024-run seed-averaged per-pixel bias maps | `docs/BIASMAP.md`, 64 runs (noise floor √16 ≈ 4× higher than theirs; enough to expose >1% structured bias) | adapted |
| Fig. 15: MSE-vs-iteration and FLIP-vs-cumulative-time convergence, biased + unbiased variants overlaid | `npm run bench:baseline` + `npm run analyze` | adopted |
| §7.1 NSight hardware counters (warp occupancy, latency) verifying *where* speedups come from | **not available** — WebGPU in Chrome exposes no hardware counters; timestamp-query per-pass ms is the measurement ceiling. Recorded as a stated claim limitation, not papered over | impossible here |
| Scene diversity: 8+ scenes incl. glossy/caustic stress cases | 3 scenes × static/move; the renderer is diffuse-only (Lambertian + emissive), so glossy-dependent techniques (footprint reconnection) structurally cannot show their paper deltas — ladder verdicts on those rows carry this caveat | adapted |

## Making A 2-3x Claim

State the denominator precisely:

- "2-3x faster at matched HDR-FLIP."
- "2-3x lower HDR-FLIP at matched GPU milliseconds."
- "2-3x fewer accumulated frames/rays at matched error."

For each claim, publish the baseline and variant GPU ms, frame budget, reference
settings, metric values, and failure cases. A hero screenshot can illustrate a
result, but the claim lives or dies on the table and convergence curve.

## Sources

- ReSTIR PT Enhanced: https://research.nvidia.com/labs/rtr/publication/lin2026restirptenhanced/
- ReSTIR DI: https://benedikt-bitterli.me/restir/
- ReSTIR GI: https://research.nvidia.com/publication/2021-06_restir-gi-path-resampling-real-time-path-tracing
- GRIS/ReSTIR PT: https://research.nvidia.com/publication/2022-07_generalized-resampled-importance-sampling-foundations-restir
- ReSTIR PT source: https://github.com/DQLin/ReSTIR_PT
- NVIDIA FLIP: https://github.com/NVlabs/flip
- HDR-FLIP: https://research.nvidia.com/publication/2021-05_HDR-FLIP
- Chrome WebGPU timestamp-query notes: https://developer.chrome.com/blog/new-in-webgpu-121
- WebGPU timing caveats: https://webgpufundamentals.org/webgpu/lessons/webgpu-timing.html
