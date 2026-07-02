# Plan: Beating Lin 2026 by 2–3× in VoxelRT

Working plan as of 2026-07-01. Companion to `STATUS.md` (state of the
implementation), `RESEARCH_LOOP.md` (measurement discipline), and
`lin2026-restirptenhanced.pdf` (the golden reference).

## 1. Where we are

- The unified ReSTIR reservoir pipeline is implemented and rendering
  end-to-end. All Lin 2026 techniques that survive translation to a
  Lambertian voxel world are in and flag-gated: paired Gaussian spatial
  reuse (§3), footprint reconnection criterion (§4), duplication-map
  adaptive cCap (§5), unified DI+GI reservoirs (§6.1), vector shading
  weights (§6.3), plus a disocclusion-rescue analog of §6.4.
- Three of the paper's techniques **do not apply here and are already free**:
  random replay (our reconnection shift has Jacobian ≡ 1 — no replay,
  so §6.2.2 stream compaction and §6.2.3 forced NEE reconnection are
  moot), and the hybrid-shift machinery itself. This is the structural
  advantage the 2–3× claim should be built on.
- The harness is largely ready: linear HDR readback, cached converged
  references, HDR-FLIP via `flip-evaluator`, WebGPU `timestamp-query`
  per-pass timings, smoke/ablation/convergence/paper suites, repeats
  with dispersion stats.
- **One open correctness item**: converged `ours` measured ~8–18% darker
  than converged `base`. The leading cause (offset-origin/unoffset-aim
  mismatch in `RF_FULLV` visibility rays) was fixed and smoke-tested,
  but the converged 1080p confirmation has not been run. Nothing above
  the bias-closure bar in Phase 1 should be trusted until this is done.

## 2. The claim, stated precisely

We cannot run their Falcor build on our scenes, so the defensible claim is
**within-harness**: `ours` vs `lin`, where `lin` is the honest, tuned
adaptation of their technique set (σ=16, 3 taps, cCap=20, their dupmap
defaults), measured with their protocol (HDR-FLIP at 1920×1080, GPU
timestamp pass times, fixed-seed repeats, static + deterministic motion).
Acceptable denominators (pick whichever is achieved, report all):

- ≥2–3× lower HDR-FLIP at matched GPU ms/frame; or
- ≥2–3× lower GPU ms/frame at matched HDR-FLIP; or
- ≥2–3× fewer frames to reach a fixed FLIP target.

Rules that keep it honest:

- Renderer-wide optimizations (brick occupancy masks, traversal changes,
  pass fusion of non-ReSTIR stages) must be ON for both `lin` and `ours`
  or excluded from the claim. They improve absolute ms, not the ratio.
- `dupmap` is intentionally biased (paper: 3.25% in Kitchen). Bias-bearing
  configs are reported separately from unbiased ones, like their Fig. 14/15.
- Every promoted idea gets an ablation row (technique added over the
  previous row) with per-pass GPU ms and FLIP at fixed budget — Table 1's
  shape, plus convergence curves (their Fig. 15).

## 3. Phase 0 — Re-verify state (half a day)

1. `npm test` green; `--suite smoke` at 1920×1080 renders and captures HDR.
2. Confirm `timestamp-query` is available on this machine's adapter and
   FLIP scoring runs (`--install-flip` if needed).
3. Freeze reference poses: `interior_static`, `interior_move`,
   `exterior_static`, `exterior_move` references built at 1600 frames and
   cached under `test/eval/refs`.

## 4. Phase 1 — Close the energy-loss bias (gate for everything else)

Target: converged `ours&dupmap=0` matches converged `base` to **>30 dB**
(denoiser off, uncapped history, same pose, 1920×1080).

1. Long-converged confirmation of the `fullv` ray-origin fix (the smoke
   ladders looked clean; this is the real test).
2. If residual darkening remains, run the flag bisection already sketched:
   `ours / ours&rclamp=0 / ours&dupmap=0 / ours&rclamp=0&dupmap=0 / lin / gi`,
   converged, channel means vs `base`. Suspects in order:
   - `rclampv=24` contribution clamp too low near the emissive ceiling
     (test `rclamp=0`; if guilty, raise default or make it adaptive —
     e.g. clamp at k× the running per-pixel mean instead of a constant);
   - dupmap cCap reduction (legitimately biased — quantify and set aside);
   - balance-heuristic MIS with visibility-free targets (test `sreuse=0`);
   - x1 reconstruction mismatch: G-buffer depth measured along the
     *jittered* primary ray but reuse reconstructs x1 from the unjittered
     center ray. **Fix regardless of bisection outcome**: store x1 (or the
     jitter) in the G-buffer. It also unblocks Area-ReSTIR-style ideas later.
   - firefly-clamp semantics differences (`Lo` clamp vs post-sum clamp).
3. Re-run the bias check after each fix; record channel means + PSNR in
   `test/eval` so regressions are visible forever after.

## 5. Phase 2 — Baseline measurement campaign (know the starting ratio)

Run the full paper-protocol suite for `base` / `gi` / `lin` / `ours`
(current flags), all four scenarios, 3+ repeats, FLIP + per-pass GPU ms:

1. Tune `lin` honestly first (paper defaults; verify σ=16 matches R=30
   average sample distance; cCap=20; dupmap α=0.1, cMin=1). Any `lin`
   mis-tuning inflates our ratio and destroys the claim.
2. Produce the first ablation table (`gi` → +unified → +paired →
   +footprint → +dupmap → +vector → each `ours` flag) and convergence
   curves. This tells us the current `ours`/`lin` ratio and how much
   headroom each Phase 3 idea must contribute.
3. Decide equal-time operating points (e.g. the GPU ms/frame of `lin` at
   default settings) and pin them for all subsequent comparisons.

## 6. Phase 3 — Research directions (all ideas, ranked)

One hypothesis at a time, each behind a flag, smoke-gated then promoted
per `RESEARCH_LOOP.md`. Promotion bar: ≥5% FLIP improvement at equal time
(or equal FLIP at ≥5% less time) on at least two scenarios, no scenario
regressing >2%. Kill fast; record kills in STATUS.md so they aren't retried.

### Tier 1 — cheap, high-confidence (do first, in this order)

1. **Measure `lightpower`** (already implemented, `RF_LIGHTPOWER`): alias
   table over emitted luminance vs uniform face sampling. Expected: big
   win in interior scenes with heterogeneous emitters; near-zero cost.
2. **Widened same-plane pairing σ.** Plane-exact validation (`RF_PLANE`)
   removes the blur/bias penalty that normally limits reuse radius. Sweep
   σ ∈ {16, 24, 32, 48} with plane validation on; also try *mixed* taps
   (e.g. two σ=16 textures + one σ=48) so near reuse handles detail and far
   reuse handles low-frequency GI. Voxel-specific; Lin cannot do this.
3. **Adaptive RIS candidate budgets.** Scale NEE candidate count with
   light count and receiver-to-light distance (paper uses a fixed 32 with
   inverse-square bounce decay). Keeps equal-time comparisons honest and
   shifts samples where they matter.
4. **Reservoir-confidence-driven denoising.** Feed reservoir confidence
   `c` and the dup score into à-trous strength and temporal history cap
   instead of raw history length. Cheap (uniforms + one texture read),
   attacks the presented-image quality axis. Report algorithm-only and
   denoised results separately.
5. **Sweep `fpc`** (ours default 0.0008 vs paper's τ/100 = 0.0002): the
   footprint criterion matters less in a Lambertian world, but a wrong
   default silently costs reuse acceptance.

### Tier 2 — bigger bets, voxel-native (the actual research)

6. **Per-brick light reservoirs (ReGIR-style world-space light grid).**
   Precompute per-brick (or per 4³-brick-cluster) light CDFs / reservoirs
   over the emissive face list, refreshed per frame with a handful of
   samples; initial NEE candidates draw from the local structure instead
   of the global alias table. The voxel grid makes cell assignment free.
   Expected: the single biggest DI win in interiors with many emissive
   faces; this is our analog of their light tiles (§6.1) but world-space
   and occlusion-aware over time.
7. **World-space (voxel-face) reservoir cache.** Store reservoirs keyed
   by voxel face in addition to per-pixel — a natural world-space hash no
   triangle renderer gets for free. Temporal reuse then survives camera
   motion and disocclusion trivially (a newly-visible surface already has
   a world-space reservoir). Start with GI reservoirs only (x2 `Lo`
   samples), merge world→screen with proper MIS. High risk/high reward;
   could subsume `rescue`.
8. **Intra-face sample mutation for decorrelation.** The dupmap trades
   correlation for bias. In a voxel world we can instead *mutate* a
   duplicated sample: jitter the reconnection point within the same
   emissive face / same surface plane. Area-measure target re-evaluation
   is exact and cheap (Jacobian 1 on the plane), so this decorrelates
   without touching MIS partition of unity — i.e. **decorrelation without
   the dupmap's darkening bias**. If it works, it replaces dupmap in the
   unbiased config and is a paper-worthy result on its own (cheap
   alternative to Sawhney et al.'s MCMC mutations).
9. **Pass fusion / bandwidth reduction.** At 1080p on WebGPU these passes
   are memory-bound: fuse dupmap into spatial reuse (it already reads the
   same reservoirs), consider fusing temporal-reuse into pathtrace tail.
   Measure with per-pass timestamps; renderer-wide, so apply to both
   `lin` and `ours` (improves absolute ms, keeps ratio honest).
10. **True dual motion vectors (§6.4).** The 3×3 rescue is an analog, not
    the technique. With `benchmove` strafes, disocclusion noise is a real
    term in the motion scenarios; implement occluder-relative reprojection
    and compare against `rescue` directly.

### Tier 3 — stretch / opportunistic

11. **Reservoir compression to 16 B/pixel** (paper went 88→64; we are at
    32): pack `Lo` as RGB9E5, x2 as grid coords + face id + 2×8-bit
    in-face UV. Halves reuse-pass bandwidth; pairs well with idea 9.
12. **Blue-noise / stratified initial-candidate seeds.** Paper's
    conclusion: quality is bounded by initial sampling. Spatiotemporal
    blue-noise masks for candidate RNG are drop-in and measurable.
13. **WebGPU subgroups** (where available) for reservoir merges and dupmap
    counting; gate on adapter feature, keep a fallback path.
14. **Half-res reservoir passes + full-res shade.** Reuse at half
    resolution, shade per-pixel with the plane-exact validation catching
    mismatches. Risky for detail; cheap to prototype with `scale`.
15. **Hierarchical empty-space skipping beyond brick masks** (coarse
    macro-mask or distance field). Renderer-wide; absolute ms only.
16. **Area-ReSTIR-lite subpixel reuse.** Once x1/jitter lives in the
    G-buffer (Phase 1), evaluate whether subpixel-aware temporal reuse
    reduces edge shimmer under motion. Their future-work section points
    here; smallest useful slice only.

### Explicitly not doing

- Porting to Falcor for apples-to-apples (protocol replication is the
  pragmatic equivalent — already agreed in STATUS.md).
- Glossy/specular BSDFs to exercise the hybrid shift — out of scope; the
  Lambertian specialization *is* the thesis.
- MCMC mutations (Sawhney 2024) as-published — expensive; idea 8 is the
  cheap voxel-native replacement.

## 7. Phase 4 — Consolidation and writeup

1. Final ablation table + convergence curves + repeated-run statistics
   for the surviving technique set; static and motion scenarios; biased
   and unbiased variants reported separately.
2. State the achieved ratio with its exact denominator; include failure
   cases (scenes/poses where the ratio dips) — the claim dies without them.
3. Update README (pipeline description is stale), write the results doc
   with the technique→paper-section mapping table.
4. Delete VoxelBench (its own rnd-log documents its techniques failing
   visual review; nothing in `pipeline/` is load-bearing).

## 8. Standing discipline

- The loop in `RESEARCH_LOOP.md` governs: one flag per hypothesis, smoke
  first, promote only via fixed-seed equal-time/equal-error evidence.
- Never conclude from sub-1080p runs; never conclude from wall clock when
  timestamps are available.
- After any change touching sampling or visibility, re-run the Phase 1
  bias check before trusting new FLIP numbers — energy loss shows up as
  "better FLIP" long before it shows up as an artifact.
- Keep STATUS.md current: every kill, every promotion, every retuned
  default gets a line, so the next session starts where this one ended.
