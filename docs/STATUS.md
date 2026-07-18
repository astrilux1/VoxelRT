# Status: ReSTIR pipeline implementation (handoff, 2026-07-09)

## 2026-07-01 campaign update (see docs/PLAN.md for the full plan)

- **2026-07-18 Process hardening (no research results in this change).**
  Implemented the process recommendations that came out of the post-world-GI
  review; no estimator, shader logic, or evidence changed. **(1)** New non-GPU
  fail-closed gate `npm run check` (`test/check.mjs`), run by CI on every push
  (`.github/workflows/check.yml`): WGSL structural sanity, RF flag-bit and
  uniform-layout agreement between `src/main.js` and `common.wgsl`, the
  string-replacement anchors main.js patches, bench/claim configuration
  resolution against real presets and knobs, claim+correctness manifest
  validation, and binding of the checked-in evidence reports to the current
  manifest SHA-256s (a manifest edit without re-running the GPU gates now
  fails here instead of silently orphaning evidence). **(2)** One-command
  entry points for the next queue items, runnable only on the benchmark
  machine: `npm run bench:baseline` (PLAN §7 item 6: locked
  `base`/`gi`/`lin`/`ours_unbiased`/`ours` convergence campaign over all six
  scenarios, 5 repeats) and `npm run bench:ladder` /
  `npm run bench:ladder:confdenoise` (item 7 rung-2 direction checks; adds
  frozen-shape `ours_mutate` — `dupmap=0&mutate=1`, measured against
  `ours_no_dup` — and `ours_confdenoise` configs to `bench.mjs`). **(3)**
  Pre-registration is now mandatory for flag-adding experiments:
  `docs/EXPERIMENT_TEMPLATE.md` (hypothesis, denominator, invariants,
  acceptance, kill condition — filled in before code, thresholds immutable
  after the first run), referenced from RESEARCH_LOOP.md §Loop.2; WORLDGI.md
  is the worked example. **(4)** Documentation debt paid: README's features,
  pipeline, URL-parameter, and testing sections now describe the actual
  unified-ReSTIR renderer and all bench commands; `VoxelBench/` is deleted
  (self-contained, nothing outside it referenced its code, its own rnd-log
  recorded its techniques failing review; PLAN §6.6/old handoff item 5 —
  history remains in git). **Next action is unchanged and now one command:**
  run `npm run bench:baseline` on the RTX 3080 machine, then drive the four
  parked flags through the ladder (`bench:ladder`), then shrink the default
  `ours` preset to promoted techniques only.
- **2026-07-18 `npm run check` mutation-tested; one real gap found and closed.**
  A fail-closed gate that has only ever been observed passing is untested, so
  the gate was run against 17 injected faults. 16 were caught. The miss: the
  host/shader flag contract compared only the *sorted bit values* of
  `src/main.js RF` and `common.wgsl RF_*`, so any **permutation** of the
  mapping passed — e.g. host `treuse: 4` against `RF_SPATIAL = 4u` leaves both
  sorted lists identical while every preset silently enables the wrong shader
  feature. That is the exact class of silent contract break the gate exists to
  catch, and it would corrupt a campaign run on top of it without failing a
  single check. `test/check.mjs` now compares the two sides **per flag**, via an
  explicit `HOST_TO_WGSL` alias map for the three flags whose names differ
  (`treuse`→`RF_TEMPORAL`, `sreuse`→`RF_SPATIAL`, `rclamp`→`RF_CLAMP`) and
  `RF_<UPPERCASE>` for the rest, and additionally fails when a flag exists on
  only one side. Re-verified: host-side, WGSL-side, and 3-cycle permutations
  plus one-sided renames are now caught, and three benign refactors
  (reformatting, comment rewording, whitespace realignment) still pass. No
  estimator, shader, or evidence change; `npm test`, `bench:preflight`, and
  `bench --suite smoke` are green on the 3080 and unaffected.
- **2026-07-18 Phase 4 primary bet KILLED WITH EVIDENCE: world-space GI reuse
  cache does not beat screen-space reuse in this renderer.** The thesis — that
  persistent brick/face GI reservoirs survive disocclusion/camera-return better
  than screen-space reuse — was tested across three materially different designs
  plus a dedicated camera-return probe, all at 1920×1080 on the RTX 3080, HDR-
  FLIP vs the frozen `_at96f`/static references (harnesses `test/worldgi-check.mjs`,
  `test/worldgi-return.mjs`, and `bench.mjs --suite convergence --frame-list 96`
  with a new `ours_worldgi` config). Results: **(1)** cache applied to every
  receiver (query in pathtrace) is catastrophic — FLIP ~4–5× worse
  (interior_move 0.115→0.590, exterior 0.121→0.485, lamps 0.167→0.668), from
  confidence runaway in the query→seed→query loop plus **brick-blocky** GI (one
  reservoir per cell read by all pixels in it; visible ~0.5 m blotches on
  interior walls). **(2)** confidence cap (`?wgicap=`) + a plane-exact 8×-cell
  key (50 MB) recovers only to ~2× worse — the blockiness is tangential, so a
  finer normal-axis key cannot fix it. **(3)** gating the query to
  `reuse_temporal`'s disocclusion `!found` branch removes the harm and ties the
  control (exterior_move 0.1210 vs 0.1208 at cap1) but is **neutral** — at 96f
  disocclusion is a thin image fraction. **(4)** a warm→look-away→return probe
  on interior_static shows FLIP delta −0.3%…+0.2% (noise) at +2/4/8/16 frames
  after return, with FLIP flat ~0.104 from +2 frames: this renderer's
  voxel-exact plane reuse + emissive NEE + disocclusion rescue already rebuild a
  re-exposed room in ~2 frames, so there is no persistence gap to exploit. The
  cache is implemented correctly (relRMSE improves ~1–2%), so this is a genuine
  hypothesis failure, not a bug. Code stays flag-gated, **off by default**, 50 MB
  buffer allocated only when `?worldgi=1` (32 B stub otherwise); kept as
  reproducible evidence per PLAN §8. **Redirect:** per PLAN §4C stop-and-document
  rule and the paper's own conclusion, the remaining quality bottleneck is
  **initial sampling**, not reuse persistence — next bet is voxel-aware initial
  proposals (extend the ReGIR-style `lightgrid`), not more reuse machinery. Full
  writeup and the three-design table are in `docs/WORLDGI.md`.
- **2026-07-17 Phase 4 primary bet started: world-space GI reuse cache
  (`?worldgi=1`) prototype landed.** This is the voxel-native contribution
  blocker #3 named as unimplemented. Design/invariant written first in
  `docs/WORLDGI.md` (PLAN §4A.2): receiver key = brick coord × 6-way face
  bucket; one persistent 32 B reservoir per cell; (256/8)³×6×32 ≈ 6.29 MB,
  resolution-independent, created once, zero-init = empty/inert. Because unified
  samples are area-measure with Jacobian ≡ 1, a cached `SK_POINT` (x2, n2, Lo)
  is re-evaluable at any receiver via `evalF` — the cache is the existing
  GRIS merge with a persistent world-cell neighbour instead of a screen pixel.
  `pathtrace.wgsl` queries the receiver's cell and confidence-weight-merges it
  into the fresh initial reservoir (same math as `reuse_temporal.wgsl`);
  `reuse_spatial.wgsl` seeds the cell with the shaded reservoir (last-writer-
  wins, non-null only so a failed pixel can't erase persistent history). Not in
  any preset; off in the unbiased gate (like `treuse=0`). **Acceptance §7.1–2
  pass** (`node test/worldgi-check.mjs`, 640×360, preset=ours): renderer is
  deterministic (worldgi=0 twice → byte-identical HDR), worldgi=1 vs worldgi=0
  at frame 1 is **byte-identical** (empty cell skips the merge, consumes no
  rand()), and at 32 static frames the cache is active (74.6% of channels
  change). Observed direction at 32f is ~3.7% darker mean — the expected
  chained-reuse correlation drift (cf. temporal's 1–2%), a real-time-axis
  observation, not a claim. **Still to do (PLAN §4B.4/§4C):** converged
  `ours_unbiased&worldgi=1&treuse=0` vs `base` bias check (§7.3), and the
  actual thesis test — disocclusion / camera-return / first-frame HDR-FLIP at
  matched GPU time vs a screen-only control (§7.4). No atomic reservoir
  stream-combine yet (last-writer-wins); brick-version invalidation is
  specified but inert for static claim scenes.
- **2026-07-09 Phase 1 is green under the paper-consistent unbiased contract.**
  Lin 2026 explicitly recommends disabling temporal reuse for noise-free
  unbiased accumulation. The earlier `cCap=20` correctness target contradicted
  that contract: temporal-plus-spatial feedback stayed 1.0-2.2% dark, and even
  a new pre-spatial-history isolation experiment still converged 1.29% low for
  `gi` on `lamps_static` at 1600 frames. The versioned correctness manifest now
  freezes `treuse=0` for reuse estimators while retaining fresh plus spatial
  reuse, `maxhist=1000000`, and `fclamp=0`; the faithful real-time `lin` preset
  remains unchanged at `cCap=20`. `npm.cmd run bench:correctness` completed in
  335.2 s and passed all 15/15 1920x1080 rows. Maximum channel-mean drift was
  0.682% interior, 0.821% exterior, and 0.910% lamps; minimum HDR PSNR was
  47.73 dB. The fail-closed report is `test/eval/estimator-correctness.v1.json`.
- **Pre-spatial temporal history is promoted as the explicit `ours_motion`
  preset.** `RF_HISTISOLATE` still uses spatial reuse for current-frame
  shading, but writes the pre-spatial temporal reservoir as next-frame history,
  preventing recursive propagation of spatially copied lineages. At the
  96-frame moving endpoint, 1920x1080, bounces=6, denoiser off, three seeds,
  frozen 1600-frame/twelve-bounce references, mean HDR-FLIP improved versus
  `ours`: exterior 0.1209 -> 0.0989 (-18.2%) at median GPU 22.56 -> 22.62 ms,
  lamps 0.1666 -> 0.1575 (-5.5%) at 33.31 -> 33.62 ms, and interior
  0.1150 -> 0.1116 (-3.0%) at 33.96 -> 33.99 ms. It is not default-on:
  `lamps_static` regressed at 16/32 frames and only caught up near 96 frames.
  Sample-independent stochastic history renewal was also tested from 6.25% to
  100%; it did not beat pure lineage isolation across motion scenes, so its
  code and configs were removed.
- **2026-07-09 Phase-0 manifest and reference gate implemented.**
  `test/eval/claim-manifest.v1.json` now freezes the 1920x1080 claim matrix:
  six scenarios, operating/reference bounce counts, 800/1600/3200 reference
  checkpoints, frame checkpoints, 16 static and 32 motion seeds, Chromium
  launch settings, RTX 3080 requirements, fp32 accumulation, timestamp queries,
  HDR-FLIP 1.7, and unbiased `fclamp=0` reference/claim estimators.
  `node test/bench.mjs --claim ...` fails closed when those
  settings or runtime versions drift; reference metadata is bound to the
  manifest SHA-256 and rejects mismatched queries or HDR byte lengths.
  `npm.cmd run bench:preflight` passed on the live RTX 3080 / Ampere adapter with
  Chromium 149.0.7827.55, Playwright 1.61.1, `rgba32float` accumulation,
  `timestamp-query`, `float32-filterable`, and HDR-FLIP installed.
- **The full reference-convergence gate passed at 1600 frames.**
  `npm.cmd run bench:references` completed all six 1920x1080, twelve-bounce
  unclamped 800->1600 comparisons in 274.9 s on the locked RTX 3080 path. Max
  channel-mean drift / HDR PSNR were: interior static 0.201% / 48.90 dB,
  interior motion 0.135% / 49.55 dB, exterior static 0.105% / 48.35 dB,
  exterior motion 0.484% / 44.49 dB, lamps static 0.263% / 50.09 dB, and lamps
  motion 0.099% / 53.48 dB. All pass the frozen <=0.5% and >=30 dB thresholds,
  so no 3200-frame
  extension was needed. The claimable report is checked in as
  `test/eval/reference-convergence.v1.json`; full HDR/PNG reference caches stay
  local under `test/eval/refs/`. Metrics also report RGB means, per-channel mean
  drift, and relative HDR RMSE.
- **The original clamped reference contract was invalid and has been replaced.**
  The shader's default `fclamp=48` clamps total radiance in `base` but clamps
  stored continuation radiance earlier in ReSTIR modes. It was not frozen in
  the first manifest, so modes were biased differently: on `interior_static`,
  disabling it moved unified energy from 3.116% low to 0.890% low even against
  the old clamped reference. Claim manifest `e734c1030fd1...` now freezes
  `fclamp=0` for every estimator and reference. Old `a093a47b302c...` caches and
  reports are diagnostic only and cannot match the new cache key.
- **The first temporal-on correctness run exposed a contract error.**
  `npm.cmd run bench:correctness` runs `base`, `gi`, unified-only,
  `lin_unbiased`, and `ours_unbiased` for 800 frames at 1920x1080 / eight
  bounces against the frozen 1600-frame / twelve-bounce references. It writes
  `test/eval/estimator-correctness.v1.json` and enforces <=1% maximum
  per-channel mean drift plus >=30 dB HDR PSNR. Current maximum energy deficits
  for those five rows are, respectively: interior 0.413/1.053/1.004/1.040/
  1.207%; exterior 0.269/1.003/1.354/1.386/1.778%; lamps 0.248/1.796/1.669/
  1.789/2.189%. All PSNR rows exceeded 47 dB, but only the three `base` rows
  passed. These rows remain diagnostic evidence for the biased real-time path;
  the paper-consistent temporal-off gate above supersedes them for correctness.
- **The remaining bias is the chained temporal-plus-spatial reuse path.**
  On `lamps_static` at 800 frames, initial-only GI was 0.288% low,
  temporal-only 0.344%, and spatial-only 0.639%; combining both was 1.796%
  low. Unified initial/temporal/spatial-only were likewise 0.279/0.315/0.837%
  low. With paired reuse, cCap=1 reduced the deficit to 0.982% and cCap=5 to
  1.316%, versus 1.774% at the paper-default cCap=20; this identifies
  history correlation/accounting but is not permission to retune `lin`.
- **The paper's footprint rule is not applicable without hybrid replay.**
  On `lamps_static`, paired reuse was 1.774% low, enabling the current
  footprint rejection made it 5.122% low, and vector shading changed nothing
  (5.127%). Excluding rejected samples from pooled domains improved it only to
  4.583%. Section 4 selects a later replay reconnection when a footprint fails;
  this x2-only Lambertian representation has no such fallback. `footprint` is
  now off in `lin`/`ours`, frozen off in unbiased checks, and retained only as
  a negative ablation. A confidence-only `constmis` experiment was also killed
  after worsening the lamps deficit to 5.276%; its code was removed.
- **Paper review changes the post-correctness priority.** Table 1's 2.74x fast
  row (35.73 -> 13.04 ms) stacks micro-op, forced NEE, replay compaction,
  paired reuse, Russian roulette, and unification; the final quality row is
  15.53 ms / 2.30x. Most replay/divergence wins do not transfer to this analytic
  WebGPU shift. The paper also names initial sampling and high-frequency motion
  as open limits and points toward Area ReSTIR. After the bias gate closes,
  prioritize voxel-aware initial proposals and brick/face-persistent GI reuse;
  `sigma=32` remains a small parked candidate, not the central research bet.
- **Motion references are keyed to the evaluated frame checkpoint.**
  The old harness derived every moving reference from a fixed 96-frame endpoint,
  so a 32-, 64-, or 128-frame moving result could be scored against the wrong
  camera pose. Moving reference stems now include `_at<frame>f`, and the final
  pose is recomputed for each checkpoint. A 320x180 diagnostic confirmed that
  frame 2 and frame 3 build and use distinct cached references; this was a
  workflow check only, not research evidence.
- **2026-07-09 continuation: benchmark default aligned to bounces=6.**
  `test/bench.mjs` now defaults `--bounces` to 6, matching the documented
  post-truncation operating point. The old bounces=2 default could silently
  reintroduce the shallow-bounce truncation floor into new smoke runs even
  though references are built with `--ref-bounces 12`.
- **Tier-1 smoke checks at 1920x1080 found only a weak sigma signal.**
  These are direction-finding runs only (64-frame references, 32-frame
  variants, denoiser off, HDR-FLIP, RTX 3080 timestamp-query timings), not
  claim-bearing results. On the heterogeneous `lamps_static` scene,
  `lightpower` did not help: at 32f x 3 repeats, `ours_no_lightpower`
  averaged FLIP 0.2841 vs `ours` 0.2862, with similar median GPU time.
  `lightgrid` also failed the equal-time smoke bar: `lamps_static` 32f x 2
  was worse (0.2888 vs `ours` 0.2864) and `interior_static` was flat
  (0.2330 vs 0.2330) while costing more. `adaptcand` has a small quality
  signal but not enough at this budget: `lamps_static` 0.2848 vs 0.2864
  with about 5% more GPU time, `interior_static` 0.2324 vs 0.2330 with
  about 4% more GPU time. The only repeat-stable direction worth promoting
  is all-tap widened pairing around `sigma=32`: at 32f x 3,
  `interior_static` improved 0.2330 -> 0.2327 with roughly equal GPU time,
  and `lamps_static` improved 0.2862 -> 0.2847 with slightly lower measured
  GPU time. This candidate is now parked until the permanent correctness gate
  passes; do not optimize a biased reservoir chain. Keep `lightpower`/
  `lightgrid` out of the promoted set unless a different scene or longer-run
  evidence overturns this smoke.
- **fp16 accumulation drift found and fixed.** All pre-fix "converged"
  references and bias measurements were contaminated: accumulation lived in
  rgba16float with the history count in fp16 alpha (integers >2048 not
  representable; running-mean increments fall below the fp16 ulp near ~1000
  frames). Measured effect: base@800f vs base@1600f differed by +3–5% in
  channel means with identical seeds. Accumulation is now rgba32float when
  `float32-filterable` is available (it is, on the RTX 3080) and the HDR
  readback decodes both layouts. **The earlier "8–18% darker" bias numbers
  (240×135, fp16 accum) are unreliable; re-measured post-fix below.**
- **fullv fix looks confirmed at equal frame count**: ours vs base at 800
  frames (both fp16 at the time) agreed to ±3% per channel — the historical
  8–18% deficit is gone. Converged fp32 confirmation ladder in flight.
- Five features implemented in parallel on branches `feature/mixsigma`,
  `feature/adaptcand`, `feature/confdenoise`, `feature/lightgrid`,
  `feature/mutate` (flag bits 8192..131072, params5/params6 slots — see
  common.wgsl), all merged into `research/voxel-restir-2x`. Keep/kill by
  equal-time FLIP per docs/RESEARCH_LOOP.md.
- **Conditional-mean energy inflation found and fixed (the real Phase-1
  bug).** Null reservoirs (failed initial sampling, null history/neighbors)
  were dropped from the reuse MIS instead of counting as zero-valued
  outcomes with confidence, so reservoir chains converged to
  E[w | sample found] = E[w]/(1-q): **+165% in `gi`** (its only candidate is
  the bounce path, which often lands on dark surfaces), +4% in `lin`/`ours`.
  Diagnosed by converged bisection: inflation was a step function of any
  confidence accumulation (identical at cCap=1/5/20, clean at 0), a
  never-adopt variant was clean, and zero-jitter changed nothing. Fix:
  pathtrace always writes c=1 (even for null outcomes); the temporal merge
  keeps null history in the partition and writes pooled-confidence nulls;
  the spatial pass separates applicable *domains* (validated neighbors plus
  canonical, null or not) from non-null *candidates*. The later footprint
  study above supersedes the old assumption that a footprint-rejected sample
  can be counted as a zero domain in this no-replay specialization.
  Post-fix: converged `gi` temporal-only went from +165% to −0.3%.
- **Reference bounce truncation found and fixed.** `base&bounces=2`
  references are ~15% dark in this interior (converged means: b3 +8.5% R
  over the b2 ref, b4 +12.7, b6 +16.3, b8 +17.3, b10 +17.6 — the series is
  still moving at b10). Worse, NEE-unified estimators capture one extra
  emissive path segment per bounce count than the hit-based baseline, so
  unified-vs-base comparisons at equal `bounces` carried a truncation
  mismatch masquerading as estimator bias (the post-null-fix "+3.4% R"
  in `gi&unified` decomposed into exactly this: an inline-NEE probe
  matched the reservoir path bit-for-bias, spatially uniform +12%
  relative excess, and base(b3) overshot it). References now default to
  `--ref-bounces 12` (cached under `_rb12` stems); estimator-correctness
  checks run configs at bounces=8 against it (residual depth mismatch
  <0.2%); real-time rows keep their operating point and share a
  truncation floor that cancels in ours-vs-lin ratios. The old
  interior/exterior `_b2` reference caches are invalid — delete them.
- **Benchmark operating point moved to bounces=6** (bounces=2 left a ~15%
  truncation floor that compressed all FLIP differences; at b6 the floor
  is ~1.3% and Russian roulette keeps the cost moderate).
- **Voxel-exact receiver reconstruction landed (big win for everyone).**
  Reuse passes reconstructed x1 as centerRay × jittered-depth — off the
  true surface at edges, wobbling per frame. Bisection showed RF_FULLV's
  canonical visibility retest false-firing on it was the ENTIRE
  lin-vs-ours quality gap. `receiverPoint()` (restir.wgsl) intersects the
  reconstruction ray with the voxel grid plane recovered by rounding the
  dominant-axis coordinate: exact, frame-stable. Interior static 64f/b6
  FLIP: lin 0.1825→0.1321, ours 0.2044→0.1349, ours+adaptcand+lightgrid
  0.1952→0.1248 (now beats lin at equal frames; equal-time pending the
  convergence curves).
- Equal-frame keep/kill so far (interior, b6, 64f): `adaptcand` and
  `lightgrid` each help (~3%, more together); `rclamp` is a no-op (clamp
  never fires at these budgets); `lightpower` is a no-op in THIS scene
  (homogeneous strip — heterogeneous-lights scene variant in progress on
  `feature/lamps-scene`); `mixsigma` slightly negative in static so far;
  `mutate`-for-dupmap neutral in static (its case is motion artifacts +
  zero converged bias, confirmed to 4 decimals).

Working notes for whoever picks this up next. Read alongside
`docs/lin2026-restirptenhanced.pdf` (Lin, Kettunen, Wyman — "ReSTIR PT
Enhanced", the golden reference for this work) and `docs/RESEARCH_LOOP.md`
for the measurement discipline behind the 2-3x claim.

## Goal

Take the Lin 2026 technique set, implement it in this renderer's WGSL
shaders, then improve on it — target is **≥2–3× better than their results**
(error at equal cost / cost at equal error), measured with an evaluation
protocol equivalent to theirs, not asserted.

## What is implemented and working

The renderer's sampling core was rebuilt as a **unified ReSTIR reservoir
pipeline**. `npm test` is green; the full pipeline renders correctly
end-to-end (default preset `ours`).

### Frame pipeline (all flag-gated)

```
pathtrace.wgsl       primary hit, G-buffer, initial reservoir candidates:
                     sun-cone NEE + emissive light-list NEE (RIS over 4
                     faces) + BSDF bounce path — one unified reservoir
                     per pixel (paper §6.1)
reuse_temporal.wgsl  reprojected reservoir merge; duplication-map adaptive
                     cCap (§5); 3×3 disocclusion history rescue (§6.4 analog)
reuse_spatial.wgsl   paired Gaussian spatial reuse via self-inverting
                     pairing textures (§3); footprint reconnection criterion
                     (§4); confidence-weighted balance-heuristic MIS;
                     vector-valued shading weights (§6.3) with per-candidate
                     visibility; writes shaded radiance + next frame's history
dupmap.wgsl          9×9 sample-duplication map from reservoir seeds (§5)
temporal.wgsl        (pre-existing) accumulation, history cap now uniform-driven
atrous.wgsl/present  unchanged
```

### Key design decisions

- **Area-measure unified domain.** Every surface is Lambertian, so a
  reservoir sample is either a surface point `x2` carrying
  direction-independent outgoing radiance `Lo`, or a distant direction
  (sun/sky). The reconnection shift is a pure re-evaluation of the receiver's
  geometry term — **Jacobian ≡ 1, no random replay** — which is the voxel
  specialization of the paper's hybrid shift. Reservoirs pack to 32 B/pixel
  (paper compresses 88→64 B, §6.2.1).
- **Pairing textures** (`src/pairing.js`): the paper's 2×2-shuffle
  construction (Eq. 3), sizes 254/230/210, per-frame offset/mirror/swap
  transform in-shader (`pairedPartner()` in `reuse_spatial.wgsl`).
- **Emissive face light list** (`src/scene.js`): every exposed face of an
  emissive voxel is a sampleable area light (word-packed; `lightCount` in
  uniforms). This is the paper's light-tile RIS analog and makes direct
  emissive lighting (interior scene) dramatically cleaner than
  path-hit-only sampling. `ours` now adds an alias table over emitted
  luminance so bright voxel faces are sampled in proportion to power rather
  than uniformly.
- **Brick occupancy masks** (`src/scene.js` + `voxel.wgsl`): each 8³ brick now
  has a 512-bit occupancy mask, letting DDA skip full voxel-buffer reads for
  empty cells inside occupied bricks. This is a renderer-wide optimization and
  is intentionally not part of the `lin`/`ours` technique comparison.
- **Estimator split**: with unification ON, emission is never picked up on
  bounce hits (pure NEE); with it OFF, the baseline estimator is preserved
  exactly. Both converge to the same integral — this is what the bias check
  below exploits.

### Configuration matrix

`?preset=base|gi|lin|ours` (see `PRESETS` in `src/main.js`) with individual
flag overrides (`?paired=0` etc.) and tuning knobs (`?taps= sigma= ccap=
capmin= dupalpha= fpc= rclampv= maxhist=`). `lin` is the faithful adaptation
of the paper's technique set; `ours` adds (so far): voxel-exact same-plane
neighbor validation, disocclusion rescue, per-candidate/canonical visibility
at shading, contribution clamping, and emissive-face power sampling.
Deterministic bench hooks: `?benchmove=`
(frame-indexed strafe), `?stopat=`, `?fseed=`, raw RGB capture
(`__voxelrt.capture().rgb`).

## RF_CONFDENOISE implemented (feature/confdenoise, 2026-07-01)

Reservoir-confidence-driven denoiser control (`?confdenoise=1`, knob
`?confk=`, default 1). A per-pixel quality signal
`q = clamp(c/(2*cCap),0,1) * (1 - dupScore)` is computed in
`reuse_spatial.wgsl` from the post-spatial reservoir confidence and the
previous frame's duplication map, and carried in the radiance texture's
alpha channel (previously a constant 1.0 nothing read — no new targets).
`temporal.wgsl` caps history at `maxhist * mix(0.125, 1, q)`;
`atrous.wgsl` scales its luminance sigma by `1 + confk*(1-q)` on top of the
existing history relaxation. Both are additionally gated on a new host
"denoise active" uniform bit (params0.z bit 1), so with `denoise=0` the
output is **bit-identical** flag on vs off (verified: 8-frame 1080p HDR
readbacks byte-equal). Presented-path axis only; measure per
RESEARCH_LOOP.md on denoised results at low frame budgets under motion.

## RF_MUTATE (`?mutate=1`, params5.w = `?mutscale=`, default 0.5)

Intra-face MCMC decorrelation (PLAN.md Tier-2 idea 8), implemented in
`reuse_temporal.wgsl` after the temporal merge. With probability equal to the
pixel's duplication score (the dupmap pass now also runs when only `mutate`
is set), a merged SK_POINT sample whose stored radiance exactly matches the
f16-quantized face-constant Le of its emissive voxel face is mutated by one
Metropolis-Hastings step: symmetric uniform jitter (half-width `mutscale` ×
face size, reflected at the face edges) in the face plane, target =
`evalTarget` × one-DDA-ray visibility, proposal re-encoded through the
reservoir packing before the ratio is evaluated, `W *= p̂(y)/p̂(y')` on
accept, confidence untouched. Intended as the unbiased replacement for the
dupmap's cCap darkening: measure `ours&dupmap=0&mutate=1` vs `ours`.
Smoke (32f, 1080p, denoise=0, `ours&dupmap=0`): HDR channel means match
on-vs-off to <2e-5 relative (exterior 0.431858/0.468142/0.434026 vs
0.431852/0.468144/0.434026; interior 1.318801/0.939388/0.684663 vs
1.318795/0.939389/0.684666) while 38–63% of pixels differ — the kernel fires
and the MH invariant holds. Converged bias check still pending.

## Energy loss in `ours`: candidate fix landed, needs converged confirmation

Bias validation (long-converged `ours` vs long-converged `base` at the same
interior pose, denoiser off, uncapped accumulation, 800 frames, 240×135):
**`ours` converges ~8–18% darker** (mean RGB 183/132/102 vs 200/152/124,
PSNR between converged images only 18.3 dB). The image is *clean* (reuse
works, noise is far lower than base at equal frames) but energy is lost.

2026-07-02 update: the low-res bisection isolated the extra darkening to
`RF_FULLV` canonical visibility revalidation. The visibility rays were started
at the receiver's offset surface point but aimed using the unoffset
`x1 -> target` vector, which can falsely self-occlude point samples. The fix in
`pathtrace.wgsl` and `reuse_spatial.wgsl` now aims emissive/canonical point
visibility rays from the actual offset ray origin. At 1920×1080, 4- and
16-frame smoke ladders no longer show a separate `fullv` energy penalty:
`lin` vs `lin&fullv=1` and `ours` vs `ours&fullv=0` are essentially tied in
HDR MSE/mean. This is not yet a final convergence proof; confirm with a
higher-frame 1920×1080 run before declaring the bias issue fully closed.
Unranked suspects, in rough order of my confidence:

1. **Contribution clamp default too low** (`rclampv=24` in demodulated
   luminance) — near the emissive ceiling strip, true contributions exceed
   this routinely. Test: `?rclamp=0`.
2. **Duplication-map cCap reduction** (§5 is *expected* to bias darker —
   paper reports 3.25% in Kitchen; a static converged view maximizes
   duplication, so the measured bias here overstates the animated case).
   Test: `?dupmap=0`.
3. **Balance-heuristic MIS with visibility-free targets** — classic ReSTIR
   darkening in shadowed/occluded regions; should be small but verify by
   disabling spatial reuse (`?sreuse=0`).
4. **x1 reconstruction mismatch**: G-buffer depth is measured along the
   *jittered* primary ray but reuse passes reconstruct x1 from the
   *unjittered* pixel-center ray → borderline visibility flips at voxel
   edges. Fix: store x1 (or the jitter) in the G-buffer.
5. Firefly-clamp semantics differ between modes (`Lo` clamp at 48 vs
   baseline's post-sum clamp at 48) — should be minor.

A flag-bisection script exists in concept (run each of
`ours / ours&rclamp=0 / ours&dupmap=0 / ours&rclamp=0&dupmap=0 / lin / gi`
converged, compare channel means against `base`) — this was about to run
when work was paused. The unbiased target: converged `ours` (minus dupmap,
which is legitimately biased) should match `base` to >30 dB.

## Pivot: use the paper's evaluation pipeline (do not reinvent)

The original low-res harness (`test/bench.mjs` at 240×135) was useful for
early correctness work but is the wrong instrument for the 2–3× claim:
resolution is far below the paper's 1920×1080, wall-clock boot/render timing
distorts cost ratios, and PSNR-on-tonemapped-8-bit is not their metric. Keep
short hardware runs at 1920×1080 for regression/bias checks; do the headline
evaluation the way the paper does:

1. **Metric: HDR ꟻLIP** (Andersson et al. 2021), which is what Lin 2026
   reports throughout. NVIDIA's reference implementation
   (github.com/NVlabs/flip) is a small Python/C++ tool; wire it to compare
   HDR (pre-tonemap) dumps. Add a linear-radiance readback path next to the
   existing 8-bit capture.
2. **Resolution/hardware**: render at 1920×1080 on a real GPU through a
   WebGPU browser. The harness should treat local GPU execution as the only
   benchmark path.
3. **References**: converged accumulation (the harness's
   `preset=base&denoise=0&maxhist=1e6` path) per pose, frozen and cached —
   equivalent to their offline references.
4. **Protocol** (mirrors their §7 + Table 1):
   - Ablation table adding one technique per row over the `gi` baseline:
     +unified DI/GI, +paired reuse, +footprint thresholds, +dupmap,
     +vector shading, then the `ours` extensions — each row reports
     per-pass GPU time (use WebGPU `timestamp-query`, not wall clock) and
     FLIP at fixed frame budget.
   - Convergence curves: FLIP vs cumulative time (their Fig. 15) for
     `base`/`lin`/`ours`, static and under the deterministic strafe.
   - Camera animation with fixed seeds, N independent runs averaged (their
     1024-run methodology, scaled down to ~16–64 runs).
5. **Their Falcor codebase** (github.com/NVIDIAGameWorks/Falcor + the
   ReSTIR PT release) is the ultimate apples-to-apples harness, but porting
   this voxel renderer into Falcor is not worth it; replicating the
   *protocol* (FLIP + per-pass timings + ablation rows + convergence plots)
   inside this repo's harness is the pragmatic equivalent and defensible.

The benchmark run line and CSV/JSON now include wall ms/f, wall FPS, GPU ms/f,
GPU FPS, GPU ms/megapixel, cumulative GPU cost for the frame budget, timed
frame count, dominant pass and pass percentages. `results.json` also includes
runtime/adapter context, and `summary.csv` records repeat-aware mean, median,
stddev, CV, min, p95, and max for wall/GPU cost fields.

## Suggested order of work

1. Fix the energy loss (bisection above; fix, then re-run the bias check —
   target >30 dB agreement of converged `ours&dupmap=0` vs `base`).
2. Add/verify HDR (pre-tonemap) readback + ꟻLIP scoring + `timestamp-query`
   per-pass timings to the harness; default benchmark resolution 1920×1080 on
   hardware, with short 1920×1080 smoke runs for quick checks.
3. Build the ablation table & convergence curves (protocol above). Tune
   `lin` honestly (σ=16 / R=30 / cCap=20 per paper defaults at 1080p).
4. Push `ours` past 2–3×: the biggest untapped, voxel-specific levers —
   - light-list power sampling (implemented behind `RF_LIGHTPOWER`/`?lightpower=`;
     measure `ours_no_lightpower` vs `ours`);
   - same-plane reuse with *widened* pairing σ (plane-exact validation
     removes the usual blur/bias penalty of long-range reuse);
   - per-brick 512-bit occupancy masks to cut DDA memory traffic (implemented;
     helps every config, improves absolute ms);
   - reservoir-confidence-driven accumulation/à-trous control (feed `c` and
     dup score into filter strength instead of raw history length);
   - RIS candidate budgets that adapt to light count and receiver/light
     distance, keeping equal-time comparisons honest.
5. Update README (pipeline description is now stale), decide VoxelBench's
   fate (recommendation: delete — its own rnd-log documents its techniques
   failing visual review; nothing in `pipeline/` is load-bearing here), and
   write up results with the technique→section mapping table.

## File map (this change)

- `src/shaders/restir.wgsl` — reservoir packing, target/integrand eval,
  footprint criterion (new)
- `src/shaders/pathtrace.wgsl` — rewritten: baseline branch + initial
  candidates/RIS
- `src/shaders/reuse_temporal.wgsl`, `reuse_spatial.wgsl`, `dupmap.wgsl` — new
- `src/shaders/common.wgsl` — uniforms extended (params2–4), flag bits
- `src/pairing.js` — pairing texture construction (new)
- `src/scene.js` — emissive face light list, power-sampling alias table, and
  per-brick occupancy masks
- `docs/RESEARCH_LOOP.md` — research/evaluation checklist for defensible
  equal-time/equal-error claims
- `src/main.js` — pass orchestration, presets/flags, bench hooks
- `test/bench.mjs` — headless harness: cached HDR references, 1920×1080
  default smoke/benchmark path, adapter diagnostics, optional HDR-FLIP,
  per-pass GPU timings when `timestamp-query` is available, richer
  performance/cost stats, repeat summaries, and `--shot` one-off renders
- `test/gpu-launch.mjs` — shared Chromium launch/GPU adapter diagnostics
