# VoxelRT Project Plan

Working plan as of 2026-07-09. This document defines where the project is
going and the gates for getting there. `STATUS.md` records what has happened;
`RESEARCH_LOOP.md` defines how experiments are measured; and
`lin2026-restirptenhanced.pdf` is the reference evaluation protocol.

## 1. Project goal

Build a real-time WebGPU path tracer that uses the stable structure of a voxel
world to reuse global-illumination samples better than a faithful adaptation
of Lin, Kettunen, and Wyman's 2026 ReSTIR PT Enhanced method.

The primary research target is a measured **2-3x advantage** over the locked
`lin` baseline in at least one defensible form:

- 2-3x less GPU time at matched HDR-FLIP;
- 2-3x lower HDR-FLIP at matched cumulative GPU time; or
- 2-3x less cumulative GPU time to cross a fixed HDR-FLIP threshold.

The exact denominator must be stated with every result. Equal frame count is
not a performance claim when the configurations have different frame costs.
The published Lin 2026 absolute timings are context, not a denominator: this
project compares `ours` with `lin` inside the same WebGPU renderer, on the same
machine, scenes, paths, seeds, references, and measurement harness.

The central thesis is not simply "ReSTIR PT in voxels." It is:

> Stable voxel, face, and brick identities make world-space GI reuse cheap and
> persistent. That structure should improve initial sampling, camera-motion
> reuse, and disocclusion behavior beyond a screen-space ReSTIR adaptation.

The 2-3x target is a hypothesis until the final benchmark campaign proves it.

## 2. Definition of done

The project is complete when all of the following are true:

1. **Correctness is closed.** The unbiased variants of `lin` and `ours`
   converge to the same solution as `base` within a predeclared tolerance.
   Intentional dupmap bias is measured and reported separately.
2. **The comparison is reproducible.** A clean checkout can build the frozen
   references, run the claim suite at 1920x1080, and reproduce tables and
   convergence data with adapter and runtime provenance attached.
3. **The baseline is honest.** `lin` uses the paper's applicable defaults and
   has not been weakened to inflate the ratio. Renderer-wide optimizations are
   shared by `lin` and `ours`.
4. **The claimed win survives motion and scene diversity.** Final results
   include interior, exterior, and heterogeneous-emitter scenes, with static
   and deterministic camera-motion cases.
5. **A voxel-native contribution carries the result.** At least one promoted
   technique relies on stable voxel/brick/face structure rather than being a
   generic parameter sweep or WebGPU micro-optimization.
6. **The result is documented honestly.** The final report includes the
   exact claim, ablations, convergence curves, timing dispersion, bias,
   failure cases, memory cost, and techniques that were tested and killed.
7. **The renderer remains usable.** `npm test` is green, the interactive demo
   still works, and the promoted preset has a documented 1080p GPU frame cost.
   A 30 FPS GPU budget on the RTX 3080 is the practical floor; 60 FPS is a
   stretch target, not a substitute for the research claim.

## 3. Scope and non-goals

### In scope

- Diffuse voxel scenes in the existing 256^3 world at 1/16 m voxel scale.
- WebGPU/WGSL as the primary implementation and portability boundary.
- One-sample-per-pixel path tracing with temporal and spatial reservoir reuse.
- Static geometry plus deterministic camera motion for the claim campaign.
- Voxel-native sampling, visibility, cache, compression, and scheduling work.
- Algorithm-only evaluation first; denoised/presented quality as a separate
  secondary axis.

### Out of scope until the primary claim is resolved

- Porting the renderer to Falcor for an absolute-time comparison.
- Glossy/specular material support solely to mirror hybrid shift mapping.
- Dynamic brick transforms, voxel physics, and general scene streaming.
- A native CUDA/DXR backend or Tensor Core path. Matrix hardware is relevant
  only if a later neural-cache or reconstruction workload is dense enough to
  justify it; DDA traversal and visibility are not that workload.
- Counting renderer-wide traversal or pass-fusion wins in the `ours`/`lin`
  algorithm ratio. Those changes may improve the product, but both sides must
  receive them.

## 4. Current state

The project has a serious experimental foundation, but it has not yet proved
the headline result.

### Working foundation

- `base`, `gi`, `lin`, and `ours` presets render end-to-end.
- The Lin-applicable algorithm set is implemented: unified DI/GI reservoirs,
  paired Gaussian spatial reuse, duplication-map history control, and vector
  shading weights. The paper's footprint reconnection criterion is retained as
  a negative ablation, not a default: it requires the hybrid shift's later-
  vertex replay fallback, which this Lambertian x2-only representation lacks.
- Voxel-specific receiver reconstruction now recovers the exact primary voxel
  plane. This removed the former false-visibility quality gap.
- The null-reservoir conditional-mean error and fp16 long-accumulation drift
  were found and fixed. Float32 accumulation is used when supported.
- The harness captures linear HDR, computes HDR-FLIP, records per-pass WebGPU
  timestamp data, logs adapter/runtime provenance, and emits repeat-aware
  CSV/JSON summaries.
- Benchmarks use 1920x1080, six operating bounces, and twelve-bounce reference
  paths by default.
- Static and motion scenarios exist for the default interior/exterior scene;
  a heterogeneous-emitter `lamps` scene is also available.
- A versioned correctness manifest and fail-closed `bench:correctness` suite
  now bind estimator checks to the frozen claim references.

### Evidence so far

- The large historical energy deficit is obsolete; it came from multiple
  correctness and reference problems that have since been fixed.
- Voxel-exact receiver reconstruction was a large shared correctness/quality
  win and must remain enabled for every comparison.
- Short 32-frame, three-repeat smoke runs give `sigma=32` the only stable
  Tier-1 signal so far, but the gain is small and not claim-bearing.
- `adaptcand` and the current `lightgrid` have shown scene-dependent equal-frame
  improvements, but equal-time convergence evidence is still missing.
- `lightpower` has not helped in the tested scenes. `mixsigma`, `rclamp`, and
  the current mutation prototype have not earned promotion yet.
- The full six-scenario unclamped (`fclamp=0`) twelve-bounce reference set
  passes its 800-to-1600-frame convergence gate. The earlier clamped reference
  contract was invalidated because base and ReSTIR modes applied the clamp at
  different estimator stages.
- The permanent static correctness gate is green: all 15 rows pass with reuse
  estimators on the paper-consistent unbiased path (`treuse=0`, fresh plus
  spatial reuse, uncapped accumulation). Maximum channel-mean drift is 0.682%
  interior, 0.821% exterior, and 0.910% lamps; minimum HDR PSNR is 47.73 dB.
  The faithful real-time `lin` path keeps temporal reuse and `cCap=20`, and is
  correctly treated as biased rather than mislabeled as an unbiased estimator.
- Pre-spatial temporal-history isolation is promoted as an explicit motion-only
  `ours_motion` preset. It improves repeated 96-frame HDR-FLIP by 18.2% on
  exterior motion and 5.5% on lamps motion at about 0.3-1.1% median GPU cost;
  it remains off for static sequences because early static FLIP regresses.
- The footprint flag alone produced about 5.1% lamps-scene darkening. Removing
  it from the applicable preset reduces `lin_unbiased` to about 1.8% low; do
  not re-enable it without a real replay/later-reconnection fallback.

### Present blockers

1. The honest current `lin` versus `ours` equal-time ratio is not yet frozen.
2. The cumulative `ours` preset contains ideas that have not all passed an
   ablation gate.
3. The strongest voxel-native idea - persistent world-space GI reservoirs -
   is not implemented.

### What the paper changes about the research priority

ReSTIR PT Enhanced's headline speedup is cumulative, not one sampling trick:
Table 1 moves from 35.73 ms to 13.04 ms (2.74x) through code micro-optimization,
forced NEE reconnection, replay compaction, paired reuse, Russian roulette, and
DI/GI unification; the quality/robustness additions raise the final cost to
15.53 ms while retaining a 2.30x speedup. Most replay/divergence savings do not
exist in this analytic Lambertian WebGPU shift, so VoxelRT must not assume that
the paper's ratio transfers. The within-renderer `lin` denominator remains the
only defensible one.

The paper's conclusion says quality is bounded by initial sampling and that
high-frequency motion remains difficult, pointing toward Area ReSTIR. After
correctness closes, VoxelRT therefore prioritizes two structural extensions:
better voxel-aware initial proposals and persistent brick/face-keyed GI reuse
that survives disocclusion and camera return. Wider pairing remains a small
candidate, not the central thesis.

## 5. Measurement contract

The measurement contract is part of the implementation. It must be frozen
before optimizing against it.

### Locked baselines

- `base`: plain one-sample-per-pixel voxel path tracing.
- `gi`: minimal ReSTIR GI-style temporal/spatial reuse.
- `lin`: faithful translation of applicable Lin 2026 techniques.
- `ours_unbiased`: promoted VoxelRT techniques with dupmap and any other known
  bias source disabled.
- `ours`: the real-time configuration, which may include measured intentional
  bias if that improves short-time perceptual quality.

`lin` starts with the paper's applicable defaults: three spatial neighbors,
`sigma=16` corresponding to radius 30, and `cCap=20`. Any retuning must use a
predeclared sweep that is also available to `ours` and must be recorded.

### Claim matrix

All claim-bearing rows use:

- 1920x1080 at render scale 1;
- six-bounce real-time configurations;
- cached `base`, denoiser-off references with twelve bounces and a locked
  high-convergence frame budget (initial target: 1600 frames);
- `interior_static`, `interior_move`, `exterior_static`, `exterior_move`,
  `lamps_static`, and `lamps_move`;
- linear HDR before tonemapping;
- HDR-FLIP as the headline metric;
- HDR MSE/RMSE, relative error, channel means, and HDR PSNR for diagnosis;
- GPU timestamp-query time and per-pass time as cost; wall time is reported
  but is not the headline denominator;
- recorded adapter, browser/backend, OS, launch flags, feature set, and
  timestamp-query availability.

Final timing rows use at least five warmed repeats. Final quality rows use
independent sampling seeds: at least 16 for static captures and 32 for motion
captures. If a bias or correlation claim depends on the mean across runs,
scale the motion set toward 64 seeds. Report confidence intervals or bootstrap
intervals in addition to mean/median/dispersion.

### Correctness tolerances

Before the baseline campaign, add a dedicated correctness suite and freeze its
thresholds. The initial gate is:

- denoiser off and effectively uncapped temporal accumulation;
- `lin` and `ours` tested without dupmap or other intentional bias;
- estimator configurations at eight bounces against the twelve-bounce
  reference, so the remaining depth mismatch is small and shared;
- mean RGB energy within 1% of the reference on each claim scene;
- greater than 30 dB HDR PSNR against the corresponding converged baseline;
- no structured residual that grows with frame count or camera motion.

If reference variance prevents those thresholds, increase the reference and
seed budgets instead of weakening the gate after seeing a result.

### Fairness rules

- Correctness fixes shared by all estimators are not `ours` contributions.
- Claim references and unbiased rows use `fclamp=0`; the interactive firefly
  clamp is a product-quality option and cannot silently enter convergence data.
- Brick masks, traversal changes, generic pass fusion, and generic buffer
  compression are enabled for both `lin` and `ours` or reported separately.
- Biased and unbiased variants never share a result row or label.
- Denoiser changes are evaluated only on the presented-quality axis and never
  used to support an algorithm-only sampling claim.
- No conclusion comes from a sub-1080p run, a single seed, equal frames alone,
  wall clock alone, or a scene selected after results are known.

## 6. Roadmap and gates

| Phase | Purpose | Exit gate |
| --- | --- | --- |
| 0 | Freeze the experiment | Claim manifest, full references, and reproducible adapter-verified smoke run |
| 1 | Close estimator correctness | Unbiased `lin` and `ours` pass the permanent convergence/bias suite |
| 2 | Establish the real baseline | Honest `lin`/`ours` ablation and equal-time curves quantify the actual gap |
| 3 | Finish current experiments | Every existing flag is promoted, parked, or killed with evidence |
| 4 | Build the voxel-native contribution | World-space GI reuse passes correctness and improves static plus motion cases |
| 5 | Optimize the surviving renderer | Measured bottlenecks fall without compromising comparison fairness |
| 6 | Prove and publish the result | Frozen multi-scene campaign supports the exact claim and documents failures |

### Phase 0 - Freeze the experiment

1. Add a versioned claim manifest under `test/eval/` containing resolution,
   scale, scenario poses/motion, operating/reference bounce counts, reference
   frames, frame checkpoints, seeds, repeats, browser launch settings, and
   metric versions.
2. Rebuild all six references at 1920x1080, scale 1, `base`, denoiser off,
   `fclamp=0`, uncapped history, twelve bounces, and 1600 frames. Preserve
   metadata beside every capture and reject caches whose query does not match
   the manifest.
3. Add an explicit reference-convergence check. Compare 800 versus 1600 frames
   and extend to 3200 where channel means or HDR error are still moving enough
   to affect the correctness tolerance.
4. Make the harness fail closed when the expected NVIDIA hardware adapter,
   float32 accumulation, timestamp queries, HDR-FLIP installation, or manifest
   settings are missing. Diagnostic overrides must mark outputs non-claimable.
5. Separate scratch outputs from durable evidence. Claim artifacts get stable
   names; one-off PNGs and obsolete low-resolution caches are not kept in the
   repository root.

Exit artifact: a single documented command can rebuild or validate references
and run an adapter-verified 1080p smoke suite from a clean checkout.

### Phase 1 - Close estimator correctness

1. Add a `correctness` benchmark suite rather than relying on handwritten
   query combinations. It must include `base`, `gi`, unified-only, `lin`
   without dupmap, and `ours` without every intentional bias mechanism.
2. Run the suite on interior, exterior, and lamps static scenes first, then on
   the deterministic motion captures.
3. Track energy by channel, HDR PSNR, relative-error images, and error versus
   frame count. A clean-looking image is insufficient.
4. Turn every correctness bug already discovered into a regression check:
   float32 reference accumulation, null outcomes in MIS partitions, bounce
   truncation, exact receiver reconstruction, and visibility-ray origin/aim
   consistency.
5. Keep the unbiased correctness path separate from the faithful real-time
   baseline: disable ReSTIR temporal reuse for the former, and retain cCap=20
   for `lin`. Rerun the suite after changes to sampling, MIS, visibility,
   reservoir packing, or cache reuse.
6. Track temporal-plus-spatial correlation and bias in real-time rows rather
   than turning a finite confidence cap into an unbiasedness claim.

Exit artifact: a checked-in correctness summary that shows unbiased convergence
and fails automatically when any locked tolerance regresses.

### Phase 2 - Establish the honest baseline

1. Freeze and document the exact `lin` translation, including which paper
   mechanisms are inapplicable in a Lambertian voxel renderer and therefore
   absent from both cost and quality comparisons.
2. Run the additive applicable sequence:
   `gi -> unified -> paired -> vector -> lin`, with per-pass GPU costs and
   HDR-FLIP at fixed cumulative GPU-time checkpoints. Report `footprint` as an
   inapplicable/negative translation ablation and `dupmap` as a separate biased
   robustness row rather than silently folding either into `lin_unbiased`.
3. Run `base`, `lin`, `ours_unbiased`, and the current `ours` through full
   convergence curves. Report equal-time and equal-error interpolations, not
   only raw checkpoints.
4. Measure the cost of each current `ours` extension over `lin`. Remove any
   default-on feature whose benefit cannot be isolated.
5. Pin the target operating points for subsequent phases. Record how much
   quality or cost remains to reach 2x and 3x; this gap determines whether
   incremental tuning can matter or a structural idea is required.

Exit artifact: the first defensible baseline report. It may show no win; its
purpose is to establish the denominator before further research.

### Phase 3 - Finish the experiments already in the tree

Use the same promotion ladder for each flag:

1. Shader/regression smoke at 1920x1080.
2. Three-seed, 32-frame direction check on at least two static scenes and one
   motion scene.
3. Equal-time convergence comparison through 128 frames with at least five
   timing repeats and independent quality seeds.
4. Full claim matrix only if the signal survives.

Default promotion bar: at least 5% lower HDR-FLIP at matched GPU time, or at
least 5% lower GPU time at matched HDR-FLIP, on two or more core scenarios;
no core scenario may regress by more than 2% without an explicit configuration
split and explanation. Small complementary wins may be kept only when a
cumulative ablation proves their combined value.

Evaluate in this order:

1. **Motion history isolation (promoted configuration split).** Keep
   `ours_motion` explicit and validate it at additional motion checkpoints;
   never silently enable it for short static sequences.
2. **All-tap `sigma=32`.** Promote the only repeat-stable Tier-1 signal to
   higher-frame static and motion curves. Compare against a fairly retuned
   `lin`, not only the current `ours` default.
3. **Adaptive candidates plus the current per-brick light grid.** Test each
   alone and together at equal time. Inspect where the grid proposals fail;
   do not increase candidate count to hide a bad proposal distribution.
4. **Confidence-driven denoising.** Evaluate only with denoising enabled,
   primarily under motion and disocclusion. Keep separate from sampling rows.
5. **Intra-face mutation.** Compare `ours&dupmap=0&mutate=1` against both the
   biased dupmap variant and unbiased no-dupmap variant. Its case must be lower
   motion correlation without converged energy loss.
6. **Power sampling, mixed sigma, clamps, and parameter sweeps.** Treat the
   current weak or negative evidence as a presumption against promotion.
   Reopen only with a stated failure hypothesis and a scene already present in
   the claim matrix.

Exit artifact: a compact keep/kill table in `STATUS.md`, a minimal promoted
`ours` preset, and no ambiguous experimental flags enabled by default.

### Phase 4 - Primary research bet: brick-resident world-space GI reuse

This phase gets the largest research budget because it attacks the core thesis
and the observed limitation of screen-space reuse: history disappears when a
surface leaves the image, reappears, or is newly exposed by camera motion.

#### 4A. Design and invariant

1. Define a stable receiver key from brick coordinate plus a small normal/face
   bucket. Keep the first prototype bounded - for example, one or a few GI
   reservoirs per active brick and face orientation - rather than allocating a
   reservoir for every possible voxel face.
2. Write the exact estimator and merge math before code. A cache miss or null
   entry must remain a zero-valued outcome in the MIS domain; the project has
   already demonstrated why dropping nulls creates energy inflation.
3. Budget memory and bandwidth for both history buffers. Record bytes per
   brick, total worst-case bytes, clears, update cost, and lookup cost.
4. Define invalidation now even though claim scenes use static geometry:
   version each brick so future voxel edits cannot silently reuse stale GI.

#### 4B. Minimal prototype

1. Add a single `worldgi` flag and one new pass or bounded update path. Avoid
   combining compression, neural scoring, or dynamic geometry in the first
   implementation.
2. Seed the cache from validated screen reservoirs and query it as an
   additional GI proposal at the current receiver. Preserve a screen-only
   control path with identical non-cache work.
3. Visualize cache coverage, age, confidence, hit rate, rejected proposals,
   and world-to-screen contribution so failures are diagnosable.
4. Prove the cache is inert when empty and unbiased when enabled without any
   intentional clamp or confidence cap.

#### 4C. Motion and proposal quality

1. Measure first-frame, disocclusion, camera-return, and steady-motion cases.
   The cache must demonstrate persistence that screen-space history cannot.
2. Tune spatial granularity, normal buckets, age/decay, and proposal count one
   dimension at a time. Use hit rate and accepted contribution, not only image
   error, to explain the result.
3. Compare world-space cache proposals with the existing light-grid proposal.
   Keep them conceptually separate: the light grid selects emitters for NEE;
   the GI cache reuses transported radiance at stable receivers.
4. Test cache invalidation with synthetic brick edits before claiming that the
   design can extend to dynamic voxel scenes.

Promotion gate: a correctness-clean world-space cache must improve matched-time
HDR-FLIP on at least one static and two motion/disocclusion scenarios, with a
clear cache-coverage explanation and no more than 2% regression elsewhere.
If it cannot beat the screen-space control after two materially different
granularity/update designs, stop and document the negative result rather than
turning it into an unbounded tuning project.

Exit artifact: the project's primary voxel-native ablation, including memory
cost, cache diagnostics, static/motion convergence curves, and failure cases.

### Phase 5 - Performance engineering after algorithm selection

Profile the promoted pipeline at the locked operating points. Optimize only
the measured dominant cost.

Priority order:

1. **Reservoir/cache bandwidth.** Pack stable fields, test 16-bit or shared
   encodings, and quantify quality loss before keeping compression.
2. **Pass and traffic reduction.** Fuse passes only where inputs, lifetime,
   and synchronization make the saved bandwidth measurable. Apply generic
   fusion to both `lin` and `ours`.
3. **Initial path sampling.** Improve divergence, Russian roulette, light
   proposals, or blue-noise candidate seeds only when pathtrace timings and
   proposal diagnostics identify them as limiting.
4. **Voxel traversal.** Extend occupancy/macro skipping only when traversal
   remains dominant after reservoir work. Keep runtime branches out of the
   inner DDA loop unless profiling proves their value.
5. **WebGPU subgroups.** Use only behind feature detection and only after a
   representative adapter shows a repeatable end-to-end win.

Stop any micro-optimization that saves less than 3% end-to-end GPU time after
repeat variance, unless it removes substantial memory or simplifies the code.

Exit artifact: a cost breakdown before/after each retained optimization, with
the algorithm ratio and absolute 1080p frame budget both reported.

### Phase 6 - Final proof, writeup, and cleanup

1. Freeze code, presets, claim manifest, references, browser/backend version,
   and hardware configuration before the final campaign.
2. Run the full seed/repeat matrix without retuning after seeing results.
3. Produce:
   - additive technique ablation table;
   - HDR-FLIP versus cumulative GPU-time curves;
   - matched-error and matched-time ratios with intervals;
   - per-pass GPU cost and memory tables;
   - biased versus unbiased convergence plots;
   - static versus motion breakdown;
   - representative images and residual/error visualizations;
   - failure cases and killed hypotheses.
4. State the strongest claim the data supports. If the result is 1.4x, report
   1.4x; do not preserve a 2-3x title after the evidence rejects it.
5. Update `README.md` to match the final renderer pipeline and add a dedicated
   results document mapping each retained technique to the corresponding Lin
   section or to the voxel-native contribution.
6. Remove obsolete low-resolution caches, loose root screenshots, stale
   fallback code, and `VoxelBench` after confirming that no final workflow or
   evidence depends on them.

Exit artifact: a reproducible result package and a clean renderer repository,
whether the central hypothesis succeeds or fails.

## 7. Immediate execution queue

These are the next actions, in order:

1. [x] Finish and verify the benchmark-default work (`bounces=6`) without
   disturbing the pre-existing uncommitted status/harness changes.
2. [x] Add the versioned claim manifest, strict preflight, motion-pose cache
   keying, and reference-convergence validation.
3. [x] Run `npm.cmd run bench:references` to build and validate high-convergence
   unclamped twelve-bounce references for all six scenarios.
4. [x] Add the permanent estimator-correctness suite and checked-in report.
5. [x] Correct the unbiased contract to disable ReSTIR temporal reuse, pass all
   15/15 correctness rows, and retain cCap=20 only in the faithful real-time
   baseline.
6. [ ] Run the locked `base`/`gi`/`lin`/`ours_unbiased`/`ours` baseline campaign.
7. [ ] Promote or kill `sigma=32`, `adaptcand+lightgrid`, `confdenoise`, and
   `mutate` using the Phase 3 ladder.
8. [ ] Reduce the default `ours` preset to promoted techniques only.
9. [x] World-space GI cache: implemented, tested, and **killed with evidence**
   (`docs/WORLDGI.md`, STATUS 2026-07-18). Three designs (everywhere/uncapped →
   catastrophic-blocky; capped + plane-exact key → ~2× worse; disocclusion-gated
   → neutral) plus a camera-return probe all failed to beat the screen-only
   control. Root cause: screen reuse here already recovers disocclusions in ~2
   frames. Code kept flag-gated and off by default as evidence. Redirect: the
   bottleneck is initial sampling, not reuse persistence (see below).
10. [ ] Voxel-aware initial proposals (the redirected primary bet): extend the
   ReGIR-style `lightgrid` toward better initial candidate distributions;
   measure against a fairly retuned `lin`. This is where the paper says quality
   is bounded and where the world-cache result says the remaining gap lives.

Do not start final optimization or broaden into dynamic voxel systems until
the baseline and correctness gates make experiment results trustworthy.

## 8. Decision log and document ownership

- `PLAN.md` changes only when the goal, scope, phase order, or gates change.
- `STATUS.md` is the chronological handoff: current evidence, exact commands,
  promoted ideas, killed ideas, and next action.
- `RESEARCH_LOOP.md` owns measurement rules shared by every experiment.
- `test/eval/claim-manifest.*` will own claim-bearing machine parameters.
- Generated benchmark files are evidence only when their metadata matches the
  frozen manifest; otherwise they are scratch results.

Every experiment ends in one of four states: **promoted**, **parked with a
specific missing test**, **killed with evidence**, or **invalid because a gate
failed**. "Interesting" is not a durable project state.
