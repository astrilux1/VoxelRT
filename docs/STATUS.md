# Status: ReSTIR pipeline implementation (handoff, 2026-07-02)

Working notes for whoever picks this up next. Read alongside
`docs/lin2026-restirptenhanced.pdf` (Lin, Kettunen, Wyman — "ReSTIR PT
Enhanced", the golden reference for this work).

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
  path-hit-only sampling.
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
at shading, contribution clamping. Deterministic bench hooks: `?benchmove=`
(frame-indexed strafe), `?stopat=`, `?fseed=`, raw RGB capture
(`__voxelrt.capture().rgb`).

## Open issue: energy loss in `ours` (MUST FIX FIRST)

Bias validation (long-converged `ours` vs long-converged `base` at the same
interior pose, denoiser off, uncapped accumulation, 800 frames, 240×135):
**`ours` converges ~8–18% darker** (mean RGB 183/132/102 vs 200/152/124,
PSNR between converged images only 18.3 dB). The image is *clean* (reuse
works, noise is far lower than base at equal frames) but energy is lost.
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
correctness/smoke work but is the wrong instrument for the 2–3× claim:
resolution is far below the paper's 1920×1080, wall-clock boot/render timing
distorts cost ratios, and PSNR-on-tonemapped-8-bit is not their metric. Keep
small hardware runs for regression/bias checks; do the headline evaluation the
way the paper does:

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

## Suggested order of work

1. Fix the energy loss (bisection above; fix, then re-run the bias check —
   target >30 dB agreement of converged `ours&dupmap=0` vs `base`).
2. Add/verify HDR (pre-tonemap) readback + ꟻLIP scoring + `timestamp-query`
   per-pass timings to the harness; default benchmark resolution 1920×1080 on
   hardware, with smaller hardware-only smoke runs for quick checks.
3. Build the ablation table & convergence curves (protocol above). Tune
   `lin` honestly (σ=16 / R=30 / cCap=20 per paper defaults at 1080p).
4. Push `ours` past 2–3×: the biggest untapped, voxel-specific levers —
   - same-plane reuse with *widened* pairing σ (plane-exact validation
     removes the usual blur/bias penalty of long-range reuse);
   - per-brick 512-bit occupancy masks to cut DDA memory traffic (helps
     every config, improves absolute ms);
   - reservoir-confidence-driven accumulation/à-trous control (feed `c` and
     dup score into filter strength instead of raw history length);
   - light-list power sampling (build alias table over face `Le`; current
     sampling is uniform).
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
- `src/scene.js` — emissive face light list
- `src/main.js` — pass orchestration, presets/flags, bench hooks
- `test/bench.mjs` — headless harness: cached references, PSNR table,
  `--shot` one-off renders (to be upgraded per the pivot above)
