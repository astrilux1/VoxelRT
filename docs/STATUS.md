# Status: ReSTIR pipeline implementation (handoff, 2026-07-02)

## 2026-07-01 campaign update (see docs/PLAN.md for the full plan)

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
  the spatial pass separates *domains* (all validated neighbors + canonical,
  null or not — they normalize the balance heuristic and pool into output
  confidence) from *candidates* (non-null, footprint-passing only).
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
