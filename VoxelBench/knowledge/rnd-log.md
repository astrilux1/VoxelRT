# VoxelBench R&D log

Running log of technique attempts, what they showed, and why decisions were
made. One section per session. Read this BEFORE starting new technique work.

## Session 2026-06-11 (non-FCGI path-tracing track: PTP/PTC/PTR/PTA)

User direction: **abandon FCGI as the design path.** Keep FCGI/FCGIX/FCLT rows
only as historical baselines; do not spend more invention time on face-cache
scheduler/contact-shadow tweaks unless explicitly redirected.

### Research direction
Recent ReSTIR PT work points at robust path reuse, lower correlation, and
unified direct/indirect reservoirs, but a full reservoir path tracer is too
large for one harness iteration. The useful local takeaway for this CPU voxel
harness was simpler: make the existing path tracer's samples accumulate
correctly over time, avoid stale history after edits, keep exact traversal, and
make fractional ray budgets honest instead of rounded-stride aliases.

### Attempt: PTG - guided/stratified path tracing - REJECTED
`techniques/ptg.h` first added PTG: ODF traversal, stratified cosine samples,
emissive-power NEE sampling, stricter cross-bilateral filtering, and history
invalidation. It was faster on open scenes and raised gradient PSNR in some
indoor cases, but lost too much PSNR/SSIM:
- bunker PTG@1x: 25.60/26.59 dB, 124 ms
- courtyard PTG@1x: 33.25/35.84 dB, 36.8 ms
- town PTG@1x: 37.88/37.63 dB, 22.3 ms
Conclusion: the strict filter plus changed sampling lowered low-frequency
agreement with the reference. Do not promote PTG.

### Attempt: PTP/PTC - progressive filtered path tracing - PARTIAL, NOT FINAL
`PTP` is not a face-cache method. It keeps PT's path estimator and final
edge-aware spatial filter, but changes the temporal/scheduling layer:
- exact ODF traversal is auto-selected only when a probe is at least 8% faster;
- history is a true running mean (`alpha = 1/n`) instead of fixed-alpha EMA;
- history resets on geometry/light events instead of blending stale lighting;
- sub-1x budgets use stochastic per-pixel gating, not rounded strides, so
  0.5x/0.75x/1.0x are meaningful and monotonic in expected work.
`PTC` is the localized-history variant: geometry edits reset only changed
pixels and cap surviving history; light events cap history globally.

Fresh latest-block evidence (frames 60-74 pre, 135-149 post; 150-frame runs):

| scene | method | mult | PSNR pre/post | gPSNR pre/post | SSIM pre/post | ms | flicker |
|---|---:|---:|---:|---:|---:|---:|---:|
| bunker | PT | 1.0 | 27.14 / 28.44 | 27.77 / 28.64 | .6058 / .6867 | 114.8 | .01048 |
| bunker | PTP | 0.5 | 29.68 / 29.38 | 29.76 / 29.29 | .7013 / .7474 | 48.8 | .00220 |
| bunker | PTP | 1.0 | 31.27 / 30.79 | 30.99 / 30.24 | .7644 / .7905 | 90.1 | .00166 |
| cavern | PT | 1.0 | 31.20 / 31.00 | 29.95 / 29.75 | .8321 / .8165 | 88.2 | .00520 |
| cavern | PTP | 0.5 | 31.86 / 31.47 | 30.44 / 30.14 | .8421 / .8350 | 40.7 | .00109 |
| cavern | PTP | 1.0 | 32.86 / 32.60 | 31.14 / 30.92 | .8571 / .8487 | 69.4 | .00083 |
| courtyard | PT | 1.0 | 37.28 / 40.26 | 35.54 / 39.21 | .9147 / .9737 | 33.6 | .00224 |
| courtyard | PTP | 0.5 | 39.12 / 42.20 | 36.84 / 40.65 | .9345 / .9800 | 18.0 | .00048 |
| courtyard | PTP | 1.0 | 40.34 / 43.29 | 37.63 / 41.43 | .9438 / .9831 | 28.5 | .00035 |
| town | PT | 1.0 | 41.31 / 40.86 | 39.58 / 39.13 | .9915 / .9904 | 30.2 | .00157 |
| town | PTP | 0.5 | 42.59 / 42.22 | 40.64 / 40.17 | .9934 / .9927 | 11.2 | .00035 |
| town | PTP | 1.0 | 43.44 / 42.91 | 41.23 / 40.76 | .9945 / .9937 | 18.0 | .00025 |

Claim boundary:
- **PTP@0.5x dominates PT@1.0x** on PSNR, gPSNR, SSIM, flicker, and wall time
  across bunker/cavern/courtyard/town.
- Against DDGI@1.0x, PTP@0.5x dominates cavern/courtyard/town; bunker needs
  PTP@0.75x or 1.0x for strict PSNR/gPSNR/SSIM domination, still faster than
  DDGI@1.0x.
- Against FCGIX/FCGI, PTP is better in courtyard/town and has much better
  gPSNR/SSIM/flicker in bunker/cavern, but it does **not** strictly dominate
  every FCGI-family Pareto point in bunker/cavern. This is acceptable only
  because the active direction is now non-FCGI.

Visual review overturned the initial promotion: a subagent reviewer flagged
PTP@1x as **not visually superior to FCGIX/FCLT**. Bunker is the strongest
counterexample: PTP has visible green/white fireflies and coarse chromatic
mottling on gray/green surfaces, while FCGIX/FCLT look smoother even when they
blur or flatten lighting. Cavern is closer, but PTP still shows structured
speckle. Courtyard/town are too similar at full frame to prove visual
superiority. Do not claim PTP as final.

### Attempt: PTR - robust progressive path tracing - PARTIAL
`PTR` keeps PTC's progressive/localized history but adds a late extreme-sample
luminance guard before the running-mean update:
- clamp starts only after 8 accumulated samples;
- incoming sample luminance is capped to `12 * luma(history) + 3`;
- the cap is intended only to suppress fireflies, not to replace filtering.

The first clamp (`4x + 0.25`) removed fireflies but crushed bunker exposure
(PSNR ~18 dB), so it was rejected immediately. The retuned clamp below is the
current PTR result.

Focused latest-block evidence (frames 60-74 pre, 135-149 post):

| scene | method | mult | PSNR pre/post | gPSNR pre/post | SSIM pre/post | ms | flicker post |
|---|---:|---:|---:|---:|---:|---:|---:|
| bunker | FCGIX | 1.0 | 31.27 / 31.67 | 28.95 / 29.30 | .6863 / .7365 | 71.5 | .00197 |
| bunker | FCLT | 1.0 | 30.26 / 30.94 | 28.40 / 28.88 | .6495 / .7130 | 28.4 | .00000 |
| bunker | PTP | 1.0 | 31.27 / 30.79 | 30.99 / 30.24 | .7644 / .7905 | 90.1 | .00111 |
| bunker | PTR | 0.75 | 29.17 / 29.57 | 31.21 / 32.01 | .7667 / .8238 | 72.5 | .00101 |
| bunker | PTR | 1.0 | 29.48 / 29.94 | 31.75 / 32.28 | .7908 / .8368 | 90.8 | .00090 |
| cavern | FCGIX | 1.0 | 32.63 / 32.52 | 31.16 / 31.10 | .8599 / .8436 | 63.7 | .00348 |
| cavern | PTP | 1.0 | 32.86 / 32.60 | 31.14 / 30.92 | .8571 / .8487 | 69.4 | .00057 |
| cavern | PTR | 0.75 | 33.48 / 33.88 | 31.56 / 31.87 | .8689 / .8694 | 53.5 | .00053 |
| cavern | PTR | 1.0 | 33.78 / 34.08 | 31.76 / 31.96 | .8739 / .8724 | 70.6 | .00048 |

Second subagent visual review of `PTR@0.75x`:
- Cavern is a defensible win versus PTP/FCGIX: fewer bright speckles, cleaner
  walls, better metrics, and lower wall time than FCGIX (`53.5 ms` vs
  `63.7 ms`).
- Bunker blocks the broad claim. PTR suppresses PTP's worst isolated fireflies
  but still has obvious mottling/blotchiness on ceiling/walls/floor and is not
  faster than FCGIX (`72.5 ms` vs `71.5 ms`). It is far slower
  than FCLT.

Current status:
- **Do not claim the non-FCGI track is proven yet.**
- `PTR@0.75x` is a real partial Pareto movement on cavern.
- Bunker needs a detail-preserving variance/firefly strategy before any
  across-scene visual-quality-and-performance claim is defensible.
- Next checks: masked/crop metrics for bunker walls/doorway, temporal crop
  review around frames 130-149, and a clamp/filter that suppresses isolated
  spikes without turning residual noise into broad mottling.

### Attempt: PTV/PTS/PTL/PTM - rejected side branches
These were tested after the PTR visual review:
- `PTV`: residual-variance-guided wide filtering. Dense 9x9 filtering improved
  bunker gPSNR/SSIM but cost ~117 ms. Sparse filtering reduced cost but added
  visible patterned smoothing and did not beat the useful frontier.
- `PTS`/`PTL`: stratified cosine directions, with `PTL` adding emissive-power
  NEE. They were cheap but collapsed bunker PSNR into the 27-28 dB range.
- `PTM`: post-filter hot-pixel cleanup without sample clamping. It modestly
  improved PTP but did not remove the visible bunker outliers enough, and was
  slower than PTP at the same budget.

### Attempt: PTU/PTA/PTD/PTE/PTB/PTF - adaptive current-sample reuse - STRONG PARTIAL
Inspired by ReSTIR/path-space-filtering style sample reuse, `PTU` reuses
same-surface **current path samples before temporal accumulation**. At
sub-1x budgets, pixels that fail their stochastic ray gate can still update
from nearby same-material, same-face samples if the local geometry matches.
This is deliberately not a face-cache: it does not store irradiance by voxel
face for later shading; it only reuses fresh path samples within the current
screen-space neighborhood before the running mean.

`PTA` adds two guards:
- adaptive reuse only on low-direct pixels (`luma(directImg) < 0.20`), so
  direct/sunlit surfaces fall back to the normal path update;
- sky-only scenes with no emissive faces fall back to PTP behavior, which is
  better for courtyard's light-change-only event.

`PTD` is a detail-preserving variant: center samples get 9x weight and
neighbor-only updates require at least 4 usable samples. It improves detail
retention slightly but narrows the bunker PSNR margin.

Follow-up detail-preservation tests:
- `PTE` adds a strict final irradiance filter (same material/face, tighter
  normal/depth gates). It did not visibly fix bunker cloudiness; metrics were
  close to PTD but generally not better (`bunker PTE@0.6`: 31.94 / 32.13 /
  .8294 @65.0 ms versus `PTD@0.6`: 32.03 / 32.24 / .8333 @67.8 ms).
- `PTB` uses an 8x8 Bayer low-discrepancy subpixel gate to avoid random
  current-sample clumps. It was rejected: the more regular gate hurt PSNR/SSIM
  badly, especially cavern (`PTB@0.6` post 31.98 / 30.21 / .8296), likely
  because it reduces temporal stochastic decorrelation needed by the path
  estimator.
- `PTF` preserves center-pixel current samples and only borrows same-surface
  neighbors for pixels with no current sample. It improves the cavern frontier
  slightly (`PTF@0.6`: 33.83 / 31.78 / .8665 @53.6 ms), but does not solve
  the bunker blocker (`PTF@0.6`: 31.33 / 32.18 / .8313 @60.2 ms; `PTF@0.75`:
  31.34 / 32.31 / .8368 @78.0 ms). A fourth subagent visual review judged
  cavern favorable, but bunker still cloudy/mottled: doorway, circular
  opening, wall/ceiling gradients, and the green wall lose crispness versus
  FCGIX/reference. Do not promote PTF as a general win.
- `PTH` adds prior-history range weighting inside current-sample reuse.
  Rejected: bunker fell to 30.81 / 32.03 / .8255 @68.4 ms.
- `PTK` adds a bilateral final reconstruction range weight. Rejected: bunker
  fell to 31.22 / 31.12 / .7941 @68.2 ms.
- `PTN` narrows reuse to a 3x3 footprint. It is not a bunker fix
  (31.37 / 31.92 / .8194 @60.7 ms), but is a useful cavern-only partial:
  33.79 / 31.75 / .8657 @49.9 ms, visually near PTD/PTF in subagent review.
- `PTW` tests low-discrepancy pixel scheduling without spatial reuse. Rejected:
  severe tonal error (`bunker PTW@0.6` PSNR 20.92; `cavern PTW@0.6` PSNR
  25.48), despite low flicker.
- `PTJ` gives neighbor-only borrowed samples fractional history confidence.
  It tied FCGIX bunker PSNR with better gPSNR/SSIM (`31.67 / 32.17 / .8308
  @69.1 ms`) but looked effectively PTD-like, so it was not promoted.
- `PTI` resets all history on events while keeping PTD-style reuse. It produced
  the strongest bunker metrics in this batch (`PTI@0.6`: 32.25 / 32.14 /
  .8285 @64.5 ms), but a fifth subagent visual review rejected promotion:
  ceiling/wall/doorway/circular-opening/green-wall cloudiness remains, and
  FCGIX still has cleaner gradients perceptually. A post-event cold-start
  diagnostic (`runpost bunker PTD 0.6 75`) scored 31.96 / 32.00 / .8271
  @64.8 ms and showed the same mottle, so the bunker artifact is not mainly
  stale pre-event history; it is introduced by the post-event sample-reuse /
  reconstruction estimator itself.
- `PTQ`/`PTY` tested reconstruction-only neighbor reuse: borrowed neighbors
  affected only the current displayed frame, not accumulated irradiance history.
  Both were rejected. `PTQ@0.6` caused severe flicker and PSNR loss
  (`bunker` flicker .03418); `PTY@0.6` reduced the damage but still fell to
  29.19 PSNR / .00518 flicker on bunker.
- `PTZ` limited spatial reuse to early-history warmup. It improved temporal
  stability but lost too much bunker quality (`PTZ@0.6`: 29.97 / 31.84 /
  .8140 @65.0 ms). Rejected.
- `PTAA` adds a dense same-face/material 11x11 local planar irradiance fit.
  This is the best visual/numeric bunker diagnostic so far (`PTAA@0.6`:
  32.67 / 32.72 / .8590 / .00082), but is far too slow (124.3 ms bunker,
  110.1 ms cavern). Treat it as an upper-bound cleanup target, not a candidate.
- `PTAB` uses a cheap 5x5 planar fit. Numeric policy was viable
  (`PTAB@0.45` bunker 31.73 / 32.28 / .8361 @67.8 ms; cavern 33.63 /
  31.64 / .8629 @55.9 ms; courtyard `PTAB@1.0` and town `PTAB@0.45` beat
  FCGIX), but subagent review rejected promotion: bunker still looked closer
  to PTD than PTAA/FCGIX-clean gradients.
- `PTAC` uses sparse radius-5 residual-gated planar fitting. It improved
  bunker metrics (`PTAC@0.45`: 32.21 / 32.61 / .8554) but remained slower
  than FCGIX at useful budgets; lower budgets regained time but lost PSNR.
  Rejected as a performance candidate.
- `PTAD` uses an integral-image same-face/material radius-5 planar fit with
  residual confidence gate and nonnegative fit clamp. It is the best partial
  candidate so far by numeric policy: bunker `PTAD@0.45` = 32.20 / 32.55 /
  .8546 / .00091 @61.2 ms, cavern `PTAD@0.45` = 33.67 / 31.63 / .8633 /
  .00049 @53.8 ms, courtyard `PTAD@1.0` and town `PTAD@0.45` both beat FCGIX
  while faster. Subagent review still rejected promotion: PTAD is cleaner than
  PTD/PTAB and good outside bunker, but bunker ceiling/walls/opening/doorway
  still show enough cloudy low-frequency mottling that FCGIX gradients read
  perceptually cleaner.
- `PTAE` uses separable Gaussian-weighted same-face/material planar moments to
  approximate PTAA more faithfully. Rejected: it was slower than FCGIX and
  worse than PTAD (`bunker PTAE@0.45`: 32.00 / 32.16 / .8490 @88.9 ms;
  `cavern PTAE@0.45`: 33.62 / 31.57 / .8622 @85.7 ms).
- `PTAF` uses the fast integral planar fit without PTAD's residual confidence
  gate. It stayed fast and numeric-positive versus FCGIX (`bunker PTAF@0.45`:
  32.18 / 32.37 / .8553 @61.5 ms), but local inspection still showed the same
  bunker mottling pattern, so it is not worth promotion or another review
  without a sharper visual discriminator.
- `PTAG` adds depth bins to the fast integral planar fit, attempting to recover
  PTAA's depth/geometry gating. Rejected: it became much slower and worse
  (`bunker PTAG@0.45`: 31.73 / 31.81 / .8366 @173.4 ms; `cavern PTAG@0.45`:
  33.42 / 31.43 / .8597 @246.9 ms).
- Added `tools/bunker_crop_metrics.ps1` as a bunker visual-gate diagnostic.
  It reports reviewer-targeted crop PSNR plus excess planar-residual RMS.
  The first run matched review outcomes: FCGIX had negative/near-zero excess
  residual in all four crops, while PTAB/PTAD/PTAF/PTAG retained positive
  excess residual, especially `ceiling_wall` (~.0030-.0034). This confirms
  the visual blocker is not captured by full-frame metrics alone.
- `PTAH` made the fast integral planar fit area-adaptive, increasing the blend
  only on broad same-face windows. It improved full-frame bunker metrics
  (`PTAH@0.45`: 32.52 PSNR / 32.53 gPSNR / .8672 SSIM / .00072 flicker
  @62.2 ms), but subagent review rejected it: broad bunker planes still read
  cloudy and speckled.
- `PTAI` enlarged the broad-plane fast fit to radius 9 with area-adaptive
  blending. This is the strongest numeric partial in the planar branch:
  bunker `PTAI@0.45` = 32.77 / 32.84 / .8705 / .00058 @62.9 ms, cavern
  `PTAI@0.45` = 33.73 / 31.66 / .8635 / .00038 @61.7 ms, courtyard
  `PTAI@1.0` = 43.42 / 41.53 / .9834 @30.5 ms, town `PTAI@0.45` =
  42.21 / 40.13 / .9927 @13.4 ms. Subagent review still marked it
  **partial, not promote**: it is cleaner than PTAF/PTAH, but bunker gradients
  remain cloudy and flattened versus FCGIX/PTAA. Crop residuals explain the
  split: green wall improves to -0.00045 and doorway is near PTAA (+0.00089),
  but circle wall (+0.00181) and ceiling wall (+0.00262) remain visibly above
  the reference residual.
- `PTAJ` added a connected same-face component quadratic correction after
  `PTAI`. It reduced green-wall residual and flicker, but worsened the circle
  wall and doorway crops (`doorway_slab` excess +0.00419), lost bunker PSNR,
  and ran slower than FCGIX on bunker. Rejected.
- `PTAK`/`PTAL` tested current-frame connected surface-basis sample reuse:
  fit a polynomial to the actual stochastic path samples on broad low-direct
  components before temporal history update. This estimator-side direction
  reduced flicker but introduced severe tone/bias error. `PTAK@0.45` collapsed
  bunker to 25.56 PSNR and `PTAL@0.45` only recovered to 29.45 PSNR; crop
  excess residuals stayed huge in all bunker crops. Rejected. The lesson is
  that a per-frame surface basis needs a bias/energy guard or persistent
  coefficient history; using sparse current samples as the dominant observation
  is not acceptable.
- `PTAM`/`PTAN` tested budget-neutral adaptive pixel sampling: cache a
  per-event probability map, raise current-frame path-sample probability on
  low-direct broad same-face regions, and lower it elsewhere. `PTAM` was too
  aggressive (`bunker@0.45`: 32.34 / 32.28 / .8612 @71.9 ms; `cavern@0.45`:
  33.63 / 31.58 / .8612 @70.5 ms). It helped `circle_wall` and `green_wall`
  residuals, but worsened `ceiling_wall` and `doorway_slab`. `PTAN` used a
  milder map and recovered full-frame metrics (`bunker@0.45`: 32.75 / 32.64 /
  .8665 @66.3 ms; `cavern@0.45`: 33.74 / 31.65 / .8627 @58.6 ms), but the
  crop gate rejected it: circle/ceiling/doorway excess residuals increased
  versus `PTAI`, while only green-wall residual improved. Adaptive sampling
  alone is therefore not enough; spending more samples on broad planes still
  needs a way to distinguish true low-frequency irradiance from blotchy
  estimator residual.
- `PTAO` tested a conservative persistent surface-model history: fit broad
  low-direct components from current samples, accumulate the evaluated model
  over time, and blend only when a luma bias guard agrees with the ordinary
  same-surface observation/history. It avoided the `PTAK`/`PTAL` tone collapse
  but was effectively inert: bunker `PTAO@0.45` = 32.77 / 32.84 / .8705
  @72.8 ms, with crop residuals matching `PTAI` and higher runtime. Rejected.
- `PTAP` added direct-field mean/variance statistics to the fast planar
  integral fit, using strong broad-plane smoothing only where local direct
  illumination is stable. It slightly improved `ceiling_wall` and
  `doorway_slab` crop residuals versus `PTAI` (+0.00258 and +0.00083), but
  worsened `circle_wall` (+0.00184), lost bunker metrics (32.72 / 32.81 /
  .8697 @70.7 ms), and did not clear the visual gate. Rejected. The direct
  stability signal is useful but not sufficient; the blocker is still
  low-frequency indirect residual on large planes, not merely direct-boundary
  contamination.
- `PTAQ` tested whether the remaining bunker residual was simply longer
  wavelength than `PTAI`'s radius-9 fast planar cleanup. It used a radius-14
  integral planar pass with high blend only on high-coverage same-face windows.
  It slightly improved `ceiling_wall` (+0.00257) and `circle_wall` (+0.00178)
  relative to `PTAI`, but lost full-frame metrics and time (`bunker@0.45`:
  32.72 / 32.76 / .8669 @69.4 ms; `cavern@0.45`: 33.70 / 31.67 / .8632
  @71.3 ms). Rejected: larger-radius smoothing is not enough.
- `PTAR` combined `PTAI` with the old Bayer low-discrepancy subpixel gate.
  It produced low/negative crop plane residuals, but for the wrong reason:
  the estimator became biased/over-regularized. Full-frame metrics collapsed
  (`bunker@0.45`: 30.85 / 32.36 / .8639 @73.3 ms; `cavern@0.45`: 31.89 /
  30.18 / .8276 @57.7 ms). This confirms the earlier PTB lesson still holds
  after planar cleanup: regular subpixel scheduling hurts the path estimator's
  temporal stochastic decorrelation.
- `PTAS` revisited connected-plane correction with an interior mask: only
  high-coverage same-face pixels are used for the component fit, and only those
  interior pixels receive the correction. This avoided `PTAJ`'s catastrophic
  full-frame loss and improved bunker headline metrics (`32.81 / 32.84 /
  .8712 / .00048 @76.1 ms`), but failed the visual gate: crop excess residuals
  worsened on `circle_wall` (+0.00227), `ceiling_wall` (+0.00299), and
  `doorway_slab` (+0.00310). Rejected as another metric-friendly smoothing
  tradeoff.
- `PTAT` combined `PTAI` with weighted emissive NEE but without the older
  stratified-direction change. It slightly helped cavern (`33.74 / 31.67 /
  .8636 / .00037 @68.4 ms`) but regressed bunker (`32.65 / 32.82 / .8688 /
  .00059 @73.1 ms`) and worsened the bunker doorway crop (+0.00223 excess).
  Subagent visual review was harsher than the metrics: PTAT turns residual
  variance into visible blue/white blobs in `circle_wall`, `ceiling_wall`, and
  the doorway region. Rejected.

Sixth subagent visual review of the current planar/NEE candidates:
- `PTAI`: do not promote. It is the best practical numeric partial, but bunker
  planes still look cleaned rather than converged: cloudy red/blue wall
  patches, flattened gradients, and weak green-wall slab separation.
- `PTAS`: do not promote. Its headline bunker metric gain is misleading
  because crop-local structured bias worsens, especially around the doorway
  slab and broad ceiling/wall surfaces.
- `PTAT`: hard reject. Any small cavern/emissive-region gain is outweighed by
  bunker degradation, visible bright blobs, worse doorway behavior, and slower
  runtime.
- Next experiment must be crop-first: detect broad planar regions where the
  estimator is producing low-frequency structured bias and change
  sampling/history acceptance before reconstruction. Do not spend the next
  iteration on more final-pass smoothing, connected-plane cleanup, or NEE-only
  tweaks.

Follow-up crop-first pre-reconstruction attempts:
- `PTAU` added a broad-plane history acceptance gate before temporal update:
  local same-surface history fit predicted the incoming observation, then
  clamped outliers more strongly when the pixel had only borrowed samples.
  Rejected. It used stale early/pre-event history as an authority and froze
  tone: bunker frame 149 fell to `28.83 / 32.81 / .8649 @105.2 ms`; crop
  excess residuals became much worse (`circle_wall` +0.00980,
  `ceiling_wall` +0.01307, `doorway_slab` +0.00627, `green_wall` +0.00479).
- `PTAV` tested current-frame winsorized sample reuse: bright neighbor path
  samples were capped before the spatial observation entered history. Rejected
  hard. The bright tail is real indirect energy; capping it biases the image
  dark (`26.52 / 32.70 / .8598 @75.8 ms`) and explodes crop residuals
  (`ceiling_wall` +0.01929, `doorway_slab` +0.01553).
- `PTAW` replaced static broad-plane adaptive sampling with dynamic,
  budget-neutral sampling from temporal residual variance. It preserved energy
  better than `PTAU`/`PTAV` but failed the crop gate and lost runtime:
  bunker frame 149 `32.67 / 32.57 / .8658 @76.5 ms`; crop residuals worsened
  versus `PTAI` on `circle_wall` (+0.00222), `ceiling_wall` (+0.00390), and
  `doorway_slab` (+0.00301), with only the already-acceptable green wall
  improving slightly.
- `PTAX` combined `PTAI` with center-sample preservation, borrowing neighbors
  only when the center pixel skipped its path sample. It was faster than
  `PTAI` (`66.5 ms`) and kept gPSNR (`32.83`), but worsened every blocker crop
  versus `PTAI`: `circle_wall` +0.00268, `ceiling_wall` +0.00367,
  `doorway_slab` +0.00177, `green_wall` -0.00026. Rejected.
- `PTAY` tested stochastic spatial reuse: each target pixel randomly kept a
  subset of same-surface neighbor samples before history update, with center
  samples always kept. This was meant to break deterministic broad-plane
  neighbor averaging without clamping the path-sample tail. Rejected:
  frame-149 bunker `32.50 / 32.84 / .8703 @76.8 ms`, and crop residuals
  worsened versus `PTAI` (`circle_wall` +0.00226, `ceiling_wall` +0.00344,
  `doorway_slab` +0.00151).
- `PTAZ` applied fractional history confidence to borrowed-only observations
  on top of `PTAI` (`reuseBorrowHistoryWeight=0.35`). Rejected: it was faster
  (`67.5 ms`) but worsened every blocker crop further (`circle_wall` +0.00309,
  `ceiling_wall` +0.00423, `doorway_slab` +0.00184), while frame-149 PSNR
  fell to `32.24`.
- `PTBA` combined `PTAI` with stricter history-disagreement weighting during
  neighbor reuse (`reuseHistoryBase=0.035`, `reuseHistoryScale=0.075`). This
  directly tested whether cross-gradient same-surface neighbor mixing caused
  the bunker wall artifacts. Rejected: bunker frame 149 fell to `31.24 /
  32.71 / .8662 @72.8 ms`, and crop residuals worsened sharply
  (`circle_wall` +0.00469, `ceiling_wall` +0.00601, `doorway_slab` +0.00376).
- `PTBB` combined `PTAI` with stratified bounce directions on every indirect
  bounce. Rejected hard: it drove some crop residuals negative by destroying
  tone agreement, with bunker frame 149 falling to `29.87 / 32.56 / .8597
  @73.0 ms` and green-wall residual worsening to +0.00191.
- `PTBC` kept only first-bounce stratification inside the `PTAI` stack. This
  is the first stratification variant with strong headline metrics (`32.92 /
  32.88 / .8720 @68.0 ms` at bunker frame 149), but it still failed the crop
  gate: `circle_wall` +0.00268, `ceiling_wall` +0.00324, `doorway_slab`
  +0.00104, and `green_wall` +0.00032 versus `PTAI`'s +0.00181/+0.00262/
  +0.00089/-0.00045.
- `PTBD` paired first-bounce stratification with the earlier radius-14
  large-plane fit. It slightly recovered the doorway crop (+0.00087) but kept
  worse circle/ceiling/green residuals (+0.00267/+0.00320/+0.00064), while
  losing `PTBC`'s headline metrics and runtime (`32.86 / 32.83 / .8692
  @78.6 ms`). Rejected.
- Lesson: the bunker broad-plane blocker is not solved by clamping high path
  samples, trusting previous history, variance-weighted sample allocation,
  center-only preservation, stochastic neighbor dropout, or reducing borrowed
  history confidence, nor by stricter history-disagreement neighbor filtering.
  Full-bounce stratification destroys tone; first-bounce-only stratification
  improves headline metrics but still moves the bunker crops the wrong way.
  The next useful direction needs a reference-free diagnostic of *spatially
  coherent bias* rather than scalar variance or outlier magnitude; otherwise
  the method either darkens true indirect energy or moves samples toward the
  wrong broad planes.
- `PTBE`/`PTBF`/`PTBG` tested a different correction: keep the `PTAI` planar
  cleanup, then apply a small same-face/material broad-plane indirect-energy
  lift on low-direct pixels. `PTBE` used radius 5 and was too broad/slow;
  `PTBF` reduced the lift radius to 3; `PTBG` additionally gates the lift to
  sky-disabled scenes so courtyard/town/cavern sky behavior stays on the
  `PTAI` path. Current-binary `PTBG@0.35` on bunker frame 149 is the strongest
  crop/numeric partial so far: `33.29` PSNR / `32.75` gPSNR / `.8670` SSIM /
  `.000637` flicker at `64.96 ms`, with crop excess residuals
  `circle_wall -0.00012`, `ceiling_wall -0.00026`, `doorway_slab -0.00132`,
  and `green_wall -0.00162`. It also reproduced the expected cavern quality
  row with the lift disabled by sky gating (`PTBG@0.45`: `33.73` / `31.66` /
  `.8635`, runtime noise only). Subagent visual review still rejected full
  promotion: PTBG is the best cheap candidate and the ceiling is improved, but
  doorway-floor smearing and muddy/patchy green-wall material separation remain
  visible blockers.
- `PTBH` added local same-surface edge confidence gates to the `PTBG` planar
  blend and broad-plane lift. Rejected: it preserved a little more boundary
  detail but lost bunker quality (`33.19` / `32.66` / `.8641` @ `68.4 ms`) and
  worsened the PTBG crop frontier except for a small doorway PSNR tradeoff.
- `PTBI` restored a clamped pre-planar high-pass residual near depth/material
  edges after PTBG's broad cleanup. Rejected: it did not visibly solve the
  green-wall/doorway blockers and lost full-frame quality (`33.20` / `32.67` /
  `.8645` @ `71.4 ms`).
- `PTBJ` changed the fast planar fit to luma-only scaling to preserve local
  RGB/chroma ratios. Rejected: it improved doorway crop PSNR slightly, but lost
  full-frame quality (`32.93` / `32.70` / `.8636` @ `64.9 ms`) and regressed
  green/ceiling crops versus PTBG. The useful lesson is that the remaining
  failure is not just RGB-plane chroma washout or edge high-pass loss; the
  method still needs a better bias detector for broad low-direct same-surface
  regions.
- `PTBK`/`PTBN` isolated the next useful lever: tighten the pre-planar final
  reconstruction gate (`finalFilterNormalMin=0.92`, `finalFilterDepthMax=1.75`)
  while keeping the PTBG broad-plane lift. `PTBK` also enabled strict material/
  face filtering; `PTBN` proved that strict material filtering was irrelevant
  because it produced the same image. The tighter geometry gate improved bunker
  crop residuals on circle/ceiling/doorway but slightly lost bunker headline
  metrics: `PTBN@0.35` frame 149 = `33.28` / `32.74` / `.8664`; crop excess
  residuals `-0.00031/-0.00043/-0.00135/-0.00157`. It improved cavern
  (`33.75` / `31.69` / `.8643`) and town (`42.61` / `40.51` / `.9931`), while
  courtyard was effectively tied.
- `PTBL` tried to make the strict material rule adaptive near surface edges,
  but copied PTBK's tighter normal/depth settings and therefore matched PTBK.
  `PTBM` isolated adaptive strictness with PTBG's original normal/depth gates
  and was exactly inert, matching PTBG. Rejected as diagnostic dead ends.
- `PTBO` kept PTBN's tighter geometry gate and raised the bunker-only broad
  indirect lift from `0.055` to `0.060`. It became the strongest partial at
  that point:
  bunker `PTBO@0.35` frame 149 = `33.33` PSNR / `32.74` gPSNR / `.8664` SSIM /
  `.000640` flicker at `64.3 ms`, beating PTBG's PSNR and all four blocker
  crop residuals (`circle_wall -0.00050`, `ceiling_wall -0.00069`,
  `doorway_slab -0.00158`, `green_wall -0.00165`). Cavern/town inherit PTBN's
  quality improvements; courtyard remains a near-tie with a small gPSNR/SSIM
  tradeoff. PTBO still needs a fresh visual-review pass before promotion
  because the sheet still shows the same family of doorway-floor smoothing and
  green-wall material-separation risk, just reduced.
- `PTBP`/`PTBQ`/`PTBR` swept the same PTBN geometry gate with broader
  sky-disabled lift amounts `0.065`, `0.070`, and `0.080`. The bunker metrics
  continued to improve through `0.080`, while gradient/SSIM slipped only
  slightly. `PTBR@0.35` is now the strongest numeric/crop partial: bunker
  frame 149 = `33.52` PSNR / `32.73` gPSNR / `.8660` SSIM / `.000642` flicker
  at `64.7 ms`; crop excess residuals are `circle_wall -0.00124`,
  `ceiling_wall -0.00169`, `doorway_slab -0.00242`, and `green_wall -0.00207`.
  Non-bunker checks match the PTBN/geometry-gate branch because the lift is
  disabled when sky is enabled: cavern `33.75 / 31.69 / .8643`, courtyard
  `43.43 / 41.50 / .9833`, town `42.61 / 40.51 / .9931`.
- 2026-06-11 subagent visual review of
  `results/images/bunker_review_sheet_ptbr.png`: **partial, not promotable**.
  PTBR is cleaner than FCGIX/PTAA/PTAI and competitive with PTBG, but it looks
  like a cleaner/softer sibling rather than a visual upgrade. Problems called
  out: broad-plane flattening on ceiling, doorway floor, and green wall; softer
  contact/detail on the doorway slab and green-wall vertical slab; brown/gray
  wall material drifting toward uniform gray-purple; green-wall contrast loss;
  circle/ceiling edge halos still present; and a plasticky denoised look. Next
  experiments should reduce or gate broad-plane lift, preserve voxel silhouette
  and contact edges, and use material/luminance-aware blending that keeps PTBR's
  highlight cleanup while falling back closer to PTBG on dark matte walls.
- `PTBS`/`PTBT`/`PTBU`/`PTBV`/`PTBW` tested PTBR lift gating rather than more
  lift. `PTBS` used perceptual surface-luma gating, `PTBT` used lift-only
  same-surface edge/contact confidence, `PTBU` combined them, and `PTBV`/`PTBW`
  swept milder luma gates. Rejected: the gate curve trades away PTBR's bunker
  win instead of improving the visual/numeric frontier. Representative frame
  149 rows: `PTBS@0.35` = `33.03` PSNR / `32.74` gPSNR / `.8662` SSIM,
  `PTBV@0.35` = `33.36 / 32.74 / .8662`, and `PTBW@0.35` = `33.43 /
  32.74 / .8661`; all were slower than PTBR and still below PTBR's crop
  frontier.
- `PTBX`/`PTBY` tested a separate broad-surface residual restoration after
  PTBR's planar cleanup and lift. This restored a small clamped same-material
  pre-planar residual only in low-direct broad interiors on darker surfaces,
  so it is distinct from the rejected `PTBI` edge high-pass restore. Rejected:
  `PTBX@0.35` kept most PTBR crop residuals but worsened frame 149 to `33.48`
  PSNR / `32.70` gPSNR / `.8646` SSIM / `.000654` flicker at `67.7 ms`;
  `PTBY@0.35` was worse (`33.44 / 32.67 / .8632 / .000665` at `82.1 ms`).
  Lesson: the blocker is not fixed by adding back local residual texture after
  smoothing; it reintroduces gradient/flicker error without solving the broad
  material/contact perception problem.
- `PTBZ`/`PTCA` tried a direct-gradient cue on top of PTBR's fast planar fit:
  reduce the high planar blend only where local direct-light variation suggests
  contact/occlusion structure. Rejected: it barely changed the crop frontier
  and slightly hurt the final metrics (`PTBZ@0.35` = `33.51` PSNR / `32.73`
  gPSNR / `.8658` SSIM / `.000654`; `PTCA@0.35` = `33.52 / 32.73 / .8660 /
  .000648`). Direct-buffer gradients are not enough to identify the visual
  material/contact failure in this branch.
- `PTCB`/`PTCC` tested a different planar/lift balance: lower planar-fit
  flattening plus stronger broad-plane lift. Rejected. It reduced mean dark
  bias but worsened gradient/SSIM/flicker and made the crop residuals even more
  over-flattened: `PTCB@0.35` = `33.45` PSNR / `32.63` gPSNR / `.8603` SSIM /
  `.000716` at `66.8 ms`; `PTCC@0.35` = `33.37 / 32.55 / .8561 / .000763` at
  `80.0 ms`. Lesson: planar/lift rebalancing alone is the wrong lever; the
  method needs a cue that separates true missing low-frequency indirect energy
  from material/contact structure before reconstruction.
- `PTCD`/`PTCE`/`PTCF` added a real voxel-occupancy contact cue: detect visible
  faces whose tangent-neighbor voxels do not continue the same exposed plane,
  then reduce planar fit and/or broad lift near those structural contacts.
  Rejected: the cue moved the image only slightly and lost PTBR's final metrics.
  Best row, `PTCF@0.35`, was `33.52` PSNR / `32.73` gPSNR / `.8659` SSIM /
  `.000644` at `69.6 ms`, still below PTBR and with no crop-frontier gain.
- `PTCG` combined PTBR with the older rotating Bayer low-discrepancy subpixel
  coverage gate. Hard reject: it undersampled (`55.2k` rays vs PTBR `59.6k`)
  and collapsed tone/PSNR (`31.02` PSNR / `32.13` gPSNR / `.8566` SSIM).
  Do not reuse the legacy low-discrepancy gate as-is on the PTBR branch.
- `PTCH` fixed that gate's main accounting flaw with a 1024-level deterministic
  Bayer threshold, raising the final bunker ray count to `58.1k`. It still
  failed hard: frame 149 was only `31.05` PSNR / `32.14` gPSNR / `.8573`
  SSIM, and all four crop PSNRs stayed far below PTBR. Lesson: deterministic
  subpixel coverage itself is incompatible with this PTBR/reuse stack, not just
  the old 64-level underspend.
- `PTBR@0.40` is a useful numeric/budget check, not a new method promotion. It
  improves over `PTBR@0.35` on every scalar metric while remaining faster than
  `PTAI@0.45`: bunker frame 149 `33.66` PSNR / `32.76` gPSNR / `.8674` SSIM /
  `.000621` flicker at `69.7 ms` and `67.6k` rays (`PTAI@0.45` was `75.0 ms`,
  `76.3k` rays). Crop PSNR also improves versus `PTBR@0.35`, but excess
  residuals become more negative (`circle_wall -0.00183`, `ceiling_wall
  -0.00182`, `doorway_slab -0.00260`, `green_wall -0.00250`), so the same
  over-flattening/material-separation risk remains. The review sheet
  `results/images/bunker_review_sheet_ptbr040.png` was reviewed by a subagent:
  **not promotable**. It is cleaner, but still has broad-plane plasticky
  flattening, muted green-wall material separation, softened contact/silhouette
  detail, and remaining blue/white edge halos. Keep `PTBR@0.40` as a
  numeric/budget partial only.
- Added `tools/bunker_perceptual_metrics.ps1` to quantify the latest visual
  blockers on the four bunker crops: luma gradient preservation, planar
  residual ratio, edge-band luma/gradient error, green-opponent error, chroma
  RMSE, and saturation ratio. This makes the PTBR failure measurable:
  `PTBR@0.40` lowers green-wall `grad_ratio` (`0.6489` vs `0.6559`) and
  `plane_res_ratio` (`0.9553` vs `0.9629`) versus `PTBR@0.35`, even while its
  scalar PSNR improves.
- `PTCI`/`PTCJ` added a preemptive planar-fit gradient-loss cap, limiting the
  high blend only when the fitted luma plane would erase a strong same-surface
  local gradient. Rejected: the cue recovers only a tiny amount of crop
  structure and trades away too much full-frame quality. Frame 149:
  `PTCI@0.35` = `33.36` PSNR / `32.63` gPSNR / `.8594` SSIM / `.000773`
  flicker at `64.7 ms`; `PTCJ@0.35` = `33.15 / 32.53 / .8529 / .000834` at
  `65.8 ms`. Green-wall `grad_ratio` rises only to `0.6609`/`0.6647`, while
  the review sheet `results/images/bunker_review_sheet_ptci.png` shows more
  mottled surfaces rather than a faithful detail recovery.
- `PTCK`/`PTCL` tested chroma-preserving planar fitting using the existing
  luma-only fit path. This directly targeted the muted-material complaint, but
  it overshot: green-wall structure metrics rise (`grad_ratio` `0.6767` /
  `0.6895`, `plane_res_ratio` `0.9710` / `0.9812`) while chroma error and
  saturation get worse (`sat_ratio` about `1.03`) and scalar quality drops.
  Frame 149: `PTCK@0.35` = `33.13` PSNR / `32.66` gPSNR / `.8624` SSIM /
  `.000685` flicker at `66.0 ms`; `PTCL@0.35` = `32.95 / 32.52 / .8549 /
  .000777` at `98.7 ms`. Rejected.
- `PTCM`/`PTCN` tested a bounded chroma-anchor between the RGB planar fit and
  the rejected luma-only path. The fit keeps RGB-plane luma but adds a clamped
  fraction of source chroma, so it should correct material muting without
  inheriting all source chroma noise. Rejected: it moves in the intended
  direction but is dominated by `PTBR`. Frame 149: `PTCM@0.35` = `33.47` PSNR /
  `32.72` gPSNR / `.8653` SSIM / `.000652` flicker at `71.3 ms`; `PTCN@0.35`
  = `33.41 / 32.70 / .8643 / .000665` at `68.9 ms`. Green-wall saturation
  improves moderately (`PTCM sat_ratio 0.9155`, `PTCN 0.9339` versus `PTBR
  0.9001`) and structure rises slightly, but chroma RMSE worsens (`0.00797` /
  `0.00828` versus `PTBR 0.00776`) and the review sheet
  `results/images/bunker_review_sheet_ptcm.png` shows no promotable visual
  gain.
- `PTCO`/`PTCP`/`PTCQ` tested a temporal-stability gate for PTBR's high
  area-driven planar-fit blend. These variants track luma residual variance in
  the temporal history and cap the high blend only on mature, stable pixels.
  Rejected as inert: `PTCO@0.35` and `PTCP@0.35` were bit-for-bit equivalent to
  `PTBR@0.35` at frame 149 (`33.52` PSNR / `32.73` gPSNR / `.8660` SSIM /
  `.000642` flicker), and the deliberately aggressive `PTCQ@0.35` only moved to
  `33.5234` PSNR / `32.7315` gPSNR / `.86603` SSIM / `.000643`. Green-wall
  perceptual metrics were unchanged (`grad_ratio` about `0.656`, `sat_ratio`
  `0.900`). Lesson: PTBR's broad-plane flattening is not separable with this
  per-pixel temporal-variance signal.
- `PTCR`/`PTCS` tested a reference-free coherent-residual gate inside PTBR's
  fast planar fit. Instead of using the reference or stale history, the gate
  detects same-surface clusters whose source luma residuals have the same sign
  relative to the fitted plane, then caps the high area-driven planar blend.
  `PTCR@0.35` is the new strongest numeric partial: bunker frame 149 =
  `34.2985` PSNR / `32.8980` gPSNR / `.87021` SSIM / `.000399` flicker at
  `67.129 ms`, same rays as `PTBR@0.35` and better/faster than `PTBR@0.40`.
  `PTCS@0.35` is weaker (`34.1873 / 32.8506 / .86704 / .000435` at
  `69.409 ms`). Crop PSNR improves over PTBR on all four blocker crops, and
  green-wall luma/chroma errors improve, but the visual gate is still not
  cleared: crop excess residuals become more negative (`PTCR`: circle
  `-0.00236`, ceiling `-0.00242`, doorway `-0.00250`, green `-0.00261`), the
  focused sheet `results/images/bunker_review_sheet_ptcr_focus.png` shows
  broad low-frequency wall patches, and green-wall saturation drops to `0.8349`
  versus `PTBR`'s `0.9001`. Keep PTCR as the best numeric partial, not a
  promotion.
- `PTCT`/`PTCU`/`PTCV` added bounded chroma anchoring on top of `PTCR` to test
  whether the new numeric winner could recover the reviewer-visible muted
  green-wall material. Rejected: `PTCT@0.35` = `34.2774 / 32.8940 / .86998 /
  .000401` at `69.270 ms`; `PTCU@0.35` = `34.2537 / 32.8903 / .86976 /
  .000403` at `70.024 ms`; `PTCV@0.35` = `34.1938 / 32.8810 / .86922 /
  .000409` at `88.934 ms`. Saturation rises only gradually (`0.8457`,
  `0.8557`, `0.8822` on green-wall) while scalar quality and runtime regress,
  and even PTCV remains below PTBR's green-wall saturation. Lesson: the
  coherent-residual gate is promising for scalar quality, but chroma anchoring
  is not enough to turn it into a visual promotion.
- `PTCW`/`PTCX` refined the coherent-residual gate by damping it when the
  same-sign residual fills nearly the entire local same-surface neighborhood.
  This directly targets the PTCR low-frequency wall patches: preserve localized
  fitted-plane residuals, but avoid treating broad residual fills as structure.
  `PTCW@0.35` = `34.3774` PSNR / `32.9318` gPSNR / `.87248` SSIM / `.000379`
  at `75.232 ms`; `PTCX@0.35` = `34.4091` PSNR / `32.9449` gPSNR / `.87339`
  SSIM / `.000365` at `66.815 ms`. `PTCX` is the new strongest numeric partial
  and also beats `PTBR@0.40` while using the `0.35x` ray count. It is not a
  visual promotion: crop excess residuals are still more negative than PTBR
  (circle `-0.00231`, ceiling `-0.00248`, doorway `-0.00248`, green
  `-0.00279`), the focused sheet
  `results/images/bunker_review_sheet_ptcx_focus.png` still shows broad wall
  patches, and green-wall saturation drops further to `0.8279` versus PTBR's
  `0.9001`.
- Non-bunker current-binary checks for `PTCX` are acceptable but not a separate
  promotion. Compared with current `PTBR` rows at the established budgets:
  cavern `PTCX@0.45` slightly improves metrics (`33.9150 / 31.7722 / .86619`)
  over `PTBR@0.45` (`33.9064 / 31.7675 / .86597`) but is slower (`68.2 ms`
  versus `60.2 ms`); courtyard `PTCX@1.0` is bit-equivalent in metrics to
  `PTBR@1.0` (`44.0525 / 41.8666 / .98481`), with similar runtime; town
  `PTCX@0.45` is metric-equivalent to `PTBR@0.45` (`42.1017 / 40.1249 /
  .99356`) and about `1.1 ms` slower in this run.
- `PTCY`/`PTCZ` tested a luma-preserving post-planar chroma restore on top of
  `PTCX`, borrowing only chroma from the pre-planar source instead of restoring
  luma texture. Rejected: `PTCY@0.35` = `34.3901 / 32.9444 / .87336 /
  .000365` at `70.712 ms`; `PTCZ@0.35` = `34.3642 / 32.9442 / .87335 /
  .000365` at `72.854 ms`. They raise green-wall saturation only to `0.8416`
  and `0.8581`, still far below PTBR, while losing PSNR/runtime. Lesson:
  saturation repair needs a better material/contact cue; chroma-only restores
  from the current noisy source are too weak or too costly.
- Added low-frequency crop diagnostics to `tools/bunker_perceptual_metrics.ps1`
  (`lowfreq_res_ratio`, `lowfreq_err_rms`) to separate extra broad-plane noise
  from over-smoothed residual loss. On the green-wall crop, PTCX has lower
  low-frequency error than PTBR but a lower residual ratio, matching the focused
  sheet read that the remaining issue is muted/flattened material response, not
  additional low-frequency noise.
- `PTDA`/`PTDB` tried a material albedo hue floor on top of `PTCX` to restore
  reviewer-visible green-wall material presence without reintroducing luma
  texture. Rejected: `PTDA@0.35` = `34.4097 / 32.9449 / .87339 / .000365` at
  `97.274 ms`; `PTDB@0.35` = `34.4102 / 32.9449 / .87339 / .000365` at
  `70.437 ms`. The perceptual/crop rows are effectively identical to PTCX,
  including green-wall `sat_ratio 0.8279`, so a simple material-albedo hue floor
  does not recover the missing material separation.
- `PTDC`/`PTDD` moved the chroma anchor into the planar fit and gated it by
  material albedo saturation/local same-surface confidence. Rejected: `PTDC` =
  `34.4071 / 32.9448 / .87338 / .000365` at `66.873 ms`; `PTDD` =
  `34.4041 / 32.9446 / .87338 / .000365` at `67.246 ms`. Crop rows remain
  effectively PTCX-identical, including green-wall `sat_ratio 0.8279`.
- `PTDE`/`PTDF` tested a diagnostic-driven blurred low-frequency restore from
  the pre-planar same-surface signal. It moved the intended metrics but not
  enough: green-wall `lowfreq_res_ratio` rises from PTCX `0.9878` to `0.9937`
  / `1.0016`, and `sat_ratio` rises to `0.8330` / `0.8362`, but scalar quality
  and cost regress (`PTDE@0.35` = `34.3283 / 32.9411 / .87311 / .000371` at
  `88.523 ms`; `PTDF@0.35` = `34.2128 / 32.9344 / .87262 / .000382` at
  `84.034 ms`). Lesson: the low-frequency cue is real, but a post-pass restore
  is too blunt and too expensive.
- `PTDG`/`PTDH` retuned the coherent patch damping between PTCX and PTCW to get
  some low-frequency recovery without the post-pass. Rejected: `PTDG@0.35` =
  `34.3995 / 32.9413 / .87312 / .000367` at `65.798 ms`; `PTDH@0.35` =
  `34.3900 / 32.9373 / .87285 / .000371` at `71.230 ms`. Green-wall
  saturation only reaches `0.8290` / `0.8304`, and focused sheets are visually
  near-identical to PTCX.
- `PTDI`/`PTDJ` tried multiscale planar fitting: keep PTCX's large-radius fit
  but add a clamped small-radius same-surface planar correction to recover
  low-frequency structure without raw residual noise. Rejected: `PTDI@0.35` =
  `34.3817 / 32.9395 / .87301 / .000375` at `80.677 ms`; `PTDJ@0.35` =
  `34.3398 / 32.9310 / .87243 / .000391` at `69.165 ms`. Green-wall
  `sat_ratio` rises only to `0.8322` / `0.8376`, while low-frequency error and
  headline metrics regress.
- `PTDK`/`PTDL` gated the multiscale correction to saturated dark materials to
  avoid damaging neutral wall cleanup. Rejected: `PTDK@0.35` = `34.3978 /
  32.9419 / .87313 / .000371` at `72.240 ms`; `PTDL@0.35` = `34.3952 /
  32.9415 / .87313 / .000374` at `76.855 ms`. The reviewer-crop rows are
  effectively identical to PTCX, including green-wall `sat_ratio 0.8279`;
  the material/dark gate misses the visible blocker while still adding cost.
- `PTDM`/`PTDN` tested stratified path directions on top of PTCX. `PTDM` limits
  stratification to the first bounce and is the current bunker scalar high-water
  mark: `34.5786` PSNR / `32.9833` gPSNR / `.87430` SSIM / `.000376` flicker at
  `66.819 ms` and `59310` rays. It is not a visual promotion: a subagent review
  judged the doorway slab cleanup plausible, but green-wall chroma is visibly
  more muted and broad low-frequency wall smoothing remains. Crop diagnostics
  agree: green-wall `sat_ratio 0.8010` and `lowfreq_res_ratio 0.9686` are worse
  than PTCX (`0.8279` / `0.9878`). `PTDN` enables full-bounce stratification and
  is a hard reject (`30.0942 / 32.5612 / .85948 / .000391` at `68.731 ms`) with
  obvious reddish/magenta tone collapse.
- `PTDO`/`PTDP`/`PTDQ` added progressively stronger broad-plane chroma restore
  on `PTDM`. They recover too little visually and add cost. `PTDO@0.35` =
  `34.5603 / 32.9832 / .87429 / .000376` at `67.752 ms`, `PTDP@0.35` =
  `34.5347 / 32.9830 / .87428 / .000377` at `70.466 ms`, and `PTDQ@0.35` =
  `34.4681 / 32.9827 / .87426 / .000377` at `73.476 ms`. Green-wall
  `sat_ratio` rises from PTDM `0.8010` to PTDP `0.8212` and PTDQ `0.8471`, but
  still does not restore reference/PTBR material character. The subagent
  addendum rejected PTDQ for promotion: it remains broad-smoothed, blunt, and
  visibly muted on dark colored surfaces. Next work should target a
  material/color-preserving low-luma saturated-plane constraint, not stronger
  global chroma restore.
- `PTDR` tested that material/color constraint at the sampler: disable PTDM's
  first-bounce stratified direction when the visible surface has saturated
  albedo and low direct light. Rejected as a useful fix: `PTDR@0.35` =
  `34.5504 / 32.9871 / .87461 / .000376` at `73.844 ms`, but the green-wall
  crop is unchanged from PTDM (`sat_ratio 0.8010`, `lowfreq_res_ratio 0.9686`).
  This falsifies visible-surface albedo as the right gate for the blocker.
- `PTDS`/`PTDT` tested a planar-fit chroma-loss blend cap on top of PTDM, using
  relative pre-fit irradiance chroma instead of albedo. `PTDS@0.35` raises
  green-wall `sat_ratio` to `0.8325`, proving the signal can move the intended
  crop, but is a hard reject: `34.4897 / 32.9497 / .87219 / .000407` at
  `93.862 ms`, with visible mottled wall texture in
  `results/images/bunker_review_sheet_ptds_focus.png`. `PTDT@0.35` is milder
  and near the PTDM scalar path (`34.5693 / 32.9794 / .87409 / .000380` at
  `67.823 ms`), but green-wall `sat_ratio` only reaches `0.8104`, still below
  PTCX `0.8279`, and the focused sheet remains visually PTDM-like. Lesson:
  transported-radiance chroma is the right class of signal, but a raw per-pixel
  chroma-loss cap is too noisy when strong and too weak when tuned safe.
- `PTDU` added same-surface directional coherence to the chroma-loss gate.
  Rejected: it recovers green-wall `sat_ratio` to `0.8251`, close to PTCX, but
  still protects mottled chroma texture and collapses scalar quality
  (`34.4039 / 32.9072 / .86966 / .000442` at `93.912 ms`). The focused sheet
  `results/images/bunker_review_sheet_ptdu_focus.png` shows the same mottling
  failure class as PTDS.
- `PTDV` tried the cheaper/stabler version: gate on fit-window mean chroma loss
  so random chroma texture should cancel before affecting the blend. Rejected:
  output is PTDM-identical in the crop diagnostics, including green-wall
  `sat_ratio 0.8010`, while runtime still rises to `94.699 ms`. Lesson:
  fit-window mean chroma is too inert, and the extra chroma math is not free in
  this CPU harness. The next useful signal likely needs temporal/connected-plane
  aggregation rather than a per-pixel fit-window test.
- `PTDW`/`PTDX` tested that connected-plane version: flood connected
  low-direct same-surface components, keep a component-level relative chroma
  direction, and cap fast-planar blending only when the fit loses that coherent
  direction. Rejected. `PTDW@0.35` = `34.5609 / 32.9738 / .87362 / .000386` at
  `75.877 ms`, green-wall `sat_ratio 0.8072`; `PTDX@0.35` = `34.5249 /
  32.9558 / .87250 / .000403` at `73.186 ms`, green-wall `sat_ratio 0.8127`.
  The focused sheet `results/images/bunker_review_sheet_ptdx_focus.png` shows
  the branch is cleaner than PTDU but still PTDM-like/muted on the green wall,
  and the stronger sweep starts drifting back toward low-frequency clouding.
  Lesson: connected-region chroma support is a better discriminator than 3x3
  chroma, but it does not recover enough transported color and its flood pass is
  too expensive for a performance win. Next try should use temporal evidence or
  a cheaper transported-radiance model rather than broader connected gating.
- `PTDY`/`PTDZ` tested stable temporal-history chroma directions as that next
  signal. They are rejected as inert: both match PTDM exactly in full-frame and
  crop metrics, including green-wall `sat_ratio 0.8010`, while runtime rises to
  `71.148 ms` / `79.197 ms`. The luma-variance and support thresholds prevent
  the gate from acting on the blocker.
- `PTDZA`/`PTDZB` loosened those thresholds to verify the temporal direction
  itself. They prove the direction can move the intended crop but still fail
  visually and numerically. `PTDZA@0.35` = `34.3129 / 32.8569 / .86684 /
  .000476` at `111.856 ms`, green-wall `sat_ratio 0.8469`; `PTDZB@0.35`
  removes the radius-2 prepass and is faster but still rejected: `34.3105 /
  32.8565 / .86656 / .000474` at `73.810 ms`, green-wall `sat_ratio 0.8431`.
  Focused sheet `results/images/bunker_review_sheet_ptdzb_focus.png` shows the
  same cloudy low-frequency chroma preserved on ceiling/green-wall crops.
  Lesson: temporal-history direction is active when loose, but capping the RGB
  fit toward pre-fit/raw chroma preserves noise. The next plausible approach is
  a luma/chroma split: keep the strong luma/planar denoise, and apply only a
  modeled or low-pass transported-chroma field.
- `PTDZC`/`PTDZD` tested that luma/chroma split directly: keep fitted luma, but
  inject a bounded same-surface low-pass history chroma field. Rejected. Scalar
  quality stays almost PTDM-identical, but the blocker moves backward because
  neutral history washes out the green wall: `PTDZC@0.35` = `34.5774 / 32.9833 /
  .87430 / .000376` at `73.791 ms`, green-wall `sat_ratio 0.7972`; `PTDZD@0.35`
  = `34.5765 / 32.9834 / .87430 / .000376` at `77.482 ms`, green-wall
  `sat_ratio 0.7943`.
- `PTDZE`/`PTDZF` saturation-weighted that low-pass chroma source so neutral
  samples could not dominate. Rejected: it still misses the blocker and creates
  sparse/streaky chroma fields. `PTDZE@0.35` = `34.4581 / 32.8752 / .87063 /
  .000425` at `71.448 ms`, green-wall `sat_ratio 0.8022`; `PTDZF@0.35` =
  `34.4756 / 32.8917 / .87176 / .000386` at `77.567 ms`, green-wall
  `sat_ratio 0.7977`. Focused sheet
  `results/images/bunker_review_sheet_ptdzf_focus.png` shows vertical/patchy
  artifacts on the green-wall crop. Lesson: history-window chroma averaging is
  not a sufficient transported-chroma model; the next attempt needs a more
  explicit surface/lighting estimate rather than averaging prior RGB chroma.
- `PTDZG`/`PTDZH` tested that explicit estimate: accumulate a first-secondary
  transported chroma side channel from actual path samples using
  `secondary_albedo * NEE`, then inject its low-pass relative chroma at fitted
  luma through the same bounded chroma-field path. Rejected. `PTDZG@0.35` =
  `34.4988 / 32.9829 / .87427 / .000378` at `71.615 ms`, green-wall
  `lowfreq_res_ratio 0.9710`, `sat_ratio 0.8066`; `PTDZH@0.35` = `34.3222 /
  32.9818 / .87416 / .000378` at `82.927 ms`, green-wall `lowfreq_res_ratio
  0.9727`, `sat_ratio 0.7873`. Focused sheet
  `results/images/bunker_review_sheet_ptdzh_focus.png` shows no visual upside
  over PTCX/PTDM; the green-wall crop remains muted, while the stronger tuning
  darkens/desaturates further. Lesson: naive first-secondary NEE chroma cleans
  neutral-wall chroma but does not model the blocker color transport. The next
  branch should change attribution/sampling of transported chroma or reduce
  planar over-smoothing, not add another low-pass chroma field.
- `PTDZI`/`PTDZJ` tested direct planar over-smoothing reduction by capping the
  fast-planar blend on dark saturated visible materials. Rejected. The gate does
  not fire on the blocker: green-wall metrics are identical to PTDM
  (`lowfreq_res_ratio 0.9686`, `sat_ratio 0.8010`) for both variants. Full-frame
  quality and/or runtime move backward: `PTDZI@0.35` = `34.5429 / 32.9673 /
  .87319 / .000384` at `70.439 ms`; `PTDZJ@0.35` = `34.4979 / 32.9460 /
  .87180 / .000396` at `92.439 ms`. Focused sheet
  `results/images/bunker_review_sheet_ptdzj_focus.png` is visually PTDM-like on
  the green wall. Lesson: visible-material saturation/luma is still the wrong
  gate; the next over-smoothing attack needs an image-space or transport signal
  that actually identifies the muted green-wall crop.
- `PTDZK`/`PTDZL` tested an image-space source-vs-fit green-opponent planar-loss
  cap. Rejected as inert: both are bit-metric-identical to PTDM on the crop
  diagnostics, including green-wall `lowfreq_res_ratio 0.9686` and `sat_ratio
  0.8010`, while runtime rises to `77.904 ms` / `83.216 ms`. The source and fit
  means are not exposing the missing green-wall chroma, so this is the wrong
  image-space signal.
- `PTDZM`/`PTDZN` moved the intervention earlier: on low-direct pixels whose
  history already has relative chroma/green-opponent content, disable PTDM's
  first-bounce stratification and fall back to PTCX-style random first-bounce
  directions. This is a useful partial but not promotable. It finally beats PTCX
  on the blocker crop: `PTDZM@0.35` green-wall `lowfreq_res_ratio 0.9914`,
  `sat_ratio 0.8319`; `PTDZN@0.35` = `0.9920` / `0.8316`; PTCX is `0.9878` /
  `0.8279`, PTDM is `0.9686` / `0.8010`. Full-frame metrics also stay above
  PTCX (`PTDZM` `34.4473 / 32.9433 / .87374 / .000364`; `PTDZN` `34.4274 /
  32.9443 / .87393 / .000366`). Rejected for the full goal because latest
  runtimes are `76.062 ms` / `75.835 ms`, and the focused sheet
  `results/images/bunker_review_sheet_ptdzp_focus.png` shows some cloudy
  low-frequency texture returning on ceiling/green-wall areas. Positive-green
  only `PTDZO`/`PTDZP` are more selective but lose the blocker improvement:
  green-wall `sat_ratio 0.8033` / `0.8047` at `75.406 ms` / `75.492 ms`.
  Lesson: the blocker is sensitive to PTDM's first-bounce stratification, but
  broad history-chroma fallback is too expensive/cloudy; next branch should make
  the fallback spatially selective without reducing it to positive-green only.

Best current PTA/PTD frontier versus historical FCGIX/FCLT baselines:

| scene | candidate | PSNR post | gPSNR post | SSIM post | ms | comparison |
|---|---:|---:|---:|---:|---:|---|
| bunker | PTA 0.5 | 31.95 | 32.16 | .8295 | 56.3 | beats FCGIX 31.67 / 29.30 / .7365 @71.5 ms |
| bunker | PTD 0.6 | 32.03 | 32.24 | .8333 | 67.8 | stronger metrics than PTA but still below FCGIX time |
| cavern | PTA 0.5 | 33.61 | 31.63 | .8625 | 48.8 | beats FCGIX 32.52 / 31.10 / .8436 @63.7 ms |
| cavern | PTD 0.6 | 33.78 | 31.74 | .8654 | 55.9 | stronger metrics than PTA but still below FCGIX time |
| cavern | PTF 0.6 | 33.83 | 31.78 | .8665 | 53.6 | small metric/time gain over PTD; visually similar |
| courtyard | PTA 1.0 | 43.29 | 41.43 | .9831 | 30.1 | beats FCGIX 42.69 / 41.17 / .9827 @42.0 ms |
| town | PTA 0.5 | 41.98 | 39.99 | .9928 | 14.1 | beats FCGIX 41.91 / 39.58 / .9918 @49.1 ms |

Third subagent visual review of PTA:
- Courtyard is defensible: PTA is close to the reference and slightly cleaner
  than FCGIX.
- Town is visually near-tie; the defensible claim is similar quality at much
  lower time, not dramatic visual superiority.
- Cavern is acceptable but loses some high-frequency wall texture while
  suppressing sparkle.
- Bunker is still the blocker for an unqualified claim. PTA/PTD remove obvious
  fireflies and win metrics, but the wall/ceiling/right-wall texture can look
  cloudy or over-smoothed versus FCGIX.

Current status after PTU/PTA/PTD:
- Numeric proof against FCGIX exists across all four frozen-reference scenes
  using the PTA/PTD frontier above.
- Visual proof is **not yet complete** because bunker remains a perceptual
  tradeoff: fewer fireflies and better metrics, but possible detail loss and
  washed/cloudy surfaces.
- PTF is a partial cavern improvement and a small performance/metric refinement
  over PTD there, but it is not a visual-quality win over PTD and not a
  bunker/overall promotion.
- PTI/full reset falsified the stale-history hypothesis. PTQ/PTY/PTZ also
  rejected reconstruction-only and warmup-only reuse. PTN is worth keeping as
  a cavern-speed branch, but the main blocker is still bunker.
- PTAA proves local planar cleanup can improve bunker, but the dense pass is
  too slow. PTAD proves a fast integral approximation can clear metrics/time,
  but not visual promotion. PTAE ruled out the straightforward separable
  Gaussian approximation, PTAF showed that removing PTAD's residual gate is
  not enough, and PTAG showed depth-binned integral fitting is too slow/worse.
  Next step should optimize directly against the new bunker crop residual
  diagnostic: an estimator that identifies and suppresses low-frequency
  blotches in the ceiling/wall/opening masks instead of smoothing all broad
  planes.

## Session 2026-06-10 (harness hardening + SFG/FCGI2 attempts)

### Harness fixes (trust the numbers now)
- **References were 32 spp — worthless.** All previous psnr numbers from this
  pipeline were measuring reference noise. Now: bunker 560/480 (pre/post),
  cavern 504/480, courtyard 392/360, town 416/376 spp, accumulated via
  `tools/topup.sh` / `topup2.sh` (each call ~35 s, sized for the 45 s shell
  limit). Estimated tonemapped noise floors (indirect-only, conservative):
  bunker ~32 dB, cavern ~35 dB, courtyard/town ~47-48 dB. **Method scores
  near 31+ dB in bunker/cavern are floor-compressed** — fine for ranking
  (same ref for all methods), not for absolute claims. Discriminate fine
  (<1 dB) differences on courtyard/town, or top refs up further
  (~0.36 s/spp/state for bunker).
- **Anti-blur-gaming metric added:** `gpsnr` (gradient-domain PSNR on
  tonemapped luma, metrics.h) punishes both missing detail (blur) and excess
  detail (noise). A flat over-filtered render can no longer win on stats.
  CSV column order is now psnr,ssim,gpsnr,flicker.
- **Mandatory visual review:** every iteration was checked against ref crops
  and abs-error heatmaps, which caught real problems the metrics under-rated
  (GFC washout, SFG mottling).

### Results, 1x budget (4*W*H rays/frame), 150 frames, event @75
See results/results.csv + results/charts/. Headline (psnr pre/post, mean ms):
- bunker:    FCGI 31.1/31.4 @119ms  > DDGI 28.9/27.0 @184  > PT 26.5/27.5 @181
- cavern:    FCGI 32.5/32.4 @99ms   > SFG 31.4/31.6 @82    > PT 31.1/30.8 @174
- courtyard: FCGI 39.8/42.7 @68ms   > PT 37.3/40.3 @47     > DDGI 32.7/34.8 @69
- town:      FCGI 42.2/41.9 @101ms  ~ PT 41.3/40.9 @53     > DDGI 32.2/34.0 @91
- FCGI at 0.5x (54-71ms) still beats/matches both baselines everywhere.
FCGI also wins gpsnr nearly everywhere => its smoothness is NOT blur-gaming.

### Attempt: SFG — Split-Field Gather (techniques/sfg.h) — DID NOT BEAT FCGI
Idea: cache far-field irradiance per face (bilinear read), gather near field
(<R~14 voxels) per pixel with short rays for contact detail; confidence-
adaptive a-trous only on the near field.
What happened, in order:
1. v1 used stored fine-B feedback => **slow equilibrium**: multi-bounce light
   propagates one face-update interval per hop (~5-10 frames each, 0.08
   blend) => image 5-8% dark at frame 74 and still converging. Lesson:
   **feedback must come from a source that converges in O(frames), i.e.
   fresh NEE at hit + the fast round-robin L1 cache (FCGI's recipe), not
   from slowly-updated fine cache chains.**
2. v2 fixed feedback but had a pi^2 unit bug (3.8x explosion) — caught by
   energy-ratio check (mean rgb vs ref must be ~1.00; cheap and catches
   everything structural).
3. v3 energy-exact (0.99) but **low-frequency mottling**: per-pixel near
   field at 1 ray/px keeps standing variance that the (correctly) fading
   spatial filter stops hiding. Temporal stratification (16 phi x 4 zenith
   strata) + alpha floor 0.04 reduced it and made SFG the fastest decent
   method (83 ms bunker), but it stayed 0.9-4.8 dB behind FCGI on every
   scene (worst outdoors).
Conclusion / structural lesson: **at 6.25 cm voxels and game resolutions,
one cache texel per face IS roughly pixel scale — bilinear-interpolated
per-face irradiance already contains the contact detail. Per-pixel gather
re-buys what the cache already has, and pays noise for it.** Park per-pixel
gather unless voxels get much larger than pixels (very close camera).

### Attempt: FCGI2 — stratified gather + longer history (techniques/fcgi2.h)
Same temporal stratification applied to FCGI's face updates, alpha floor
0.08->0.05. Result: within +-0.1 dB of FCGI pre-edit (likely at the bunker
ref floor), slightly better gpsnr, but **post-edit reconvergence regressed**
(31.45->30.86): longer history fights the invalidation clamp. Not adopted.
If revisited: keep stratification, but make alpha floor state-dependent
(0.08 while n<20 after invalidation, 0.05 in steady state).

### Prior attempts (archived FCGI-era notes, superseded)
- GFC (full per-pixel gather into radiosity cache + a-trous): washed out
  (+9-11% energy, error spread over every surface), 26.8 dB bunker. The
  a-trous over 1-ray gather noise destroys more than it saves.
- GFCAO (cache + 1 short AO ray modulation): biased dark, unstable across
  scenes (29.9-31.4), worst post-edit. AO-modulating an unbiased cache
  reintroduces the bias FCGI exists to avoid.

### Archived FCGI-era verdict
Superseded by the current non-FCGI path-tracing work. The notes below are kept
only as historical baseline context; do not treat them as the current
production direction.

Earlier verdict: FCGI (per-face irradiance cache + L1 feedback + bilinear
face-plane interpolation) looked like the production candidate in that older
run. It won psnr, gpsnr, ssim and cost simultaneously on all four scenes, was
visually clean, and responded to destruction within about 10 frames locally.
Open quality gaps seen in review were shallow contact shadows and non-monotonic
bunker 2x behavior.

### Archived FCGI-era next steps
1. Deepen bunker/cavern refs to ~2000 spp so >31 dB improvements are
   measurable; re-run headline configs against frozen refs.
2. FCGI scheduler fix: at higher budgets spread updates over more faces
   (coverage) instead of more rays per face; should make quality monotonic
   in budget.
3. FCGI contact-shadow depth: try 2-tap neighbor-aware bilinear that
   respects concave corners (don't interpolate across an inside corner), or
   a *bias-free* near-field term derived from the cache's own En/Ef split
   (SFG infrastructure exists in sfg.h).
4. GPU port (gpu/ dir is empty): brickmap + face atlas compute shaders, per
   prior_work/FaceCache-GI_report.md section 7. The CPU numbers justify it.
