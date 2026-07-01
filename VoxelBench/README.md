# VoxelBench — real-time path-traced GI for destructible voxel worlds

R&D pipeline for inventing and *proving* voxel GI techniques against
SOTA-class baselines. Target: 0.0625 m voxels, fully destructible worlds,
low-end PCs / mobile (no RT hardware), beautiful output (no noise, no blur,
no leaks).

## Layout
- `pipeline/` — C++ benchmark harness. core (grid/brickmap/DDA/NEE/metrics),
  4 scenes, techniques behind a common interface (`technique.h`).
- `knowledge/sota-survey.md` — survey of Teardown, DDGI/RTXGI, Radiance
  Cascades, SHaRC, NRC, ReSTIR GI, Lumen, VXGI + gap analysis.
- `knowledge/rnd-log.md` — **attempt log: what was tried, what failed, why.**
- `knowledge/pipeline-notes.md` — how to run; environment quirks; guards.
- `prior_work/` — first-generation benchmark + FaceCache-GI report.
- `results/` — frozen references (results/raw), per-frame CSV, charts,
  rendered frames, visual comparison montages (results/charts/visual_*.png).

## Protocol (what makes a claim trustworthy here)
All methods share: traversal, scenes, camera, converged direct pass,
ray budget (4·W·H·mult rays/frame), 150-frame runs with a destruction or
lighting event at frame 75, references path-traced at 360-560 spp.
Metrics: tonemapped PSNR + SSIM + **gradient-PSNR (anti-blur metric)** +
temporal flicker, plus mandatory human visual review of frames and error
maps. Quality alone is not enough — wall time and rays are logged per frame.

## R&D direction
The target is **better real path tracing**, not approximations that fake a
smooth image by blurring, washing out color bleed, leaking through thin voxel
walls, or hiding lighting error behind temporal lag. ReSTIR PT Enhanced
(NVIDIA Research, 2026) is the reference direction: increase effective sample
count through principled spatiotemporal reuse, reciprocal neighbor selection,
robust shift/reconnection tests, lower correlation, and unified direct/global
illumination reservoirs. A technique only counts if it moves the image closer
to a high-spp path-traced golden render under visual review and hard metrics.

## Current state (2026-06-10)
**FCGI (FaceCache-GI) is the leading technique** — one RGB irradiance value
per exposed voxel face (the 6-normal collapse makes directional encoding
unnecessary), updated by short feedback gathers against a coarse L1 brick
cascade, shaded with face-plane bilinear interpolation, invalidated
brick-locally on edits. It beats 1spp-PT+denoise and DDGI on every scene on
every quality metric at equal ray budget, and still beats them at HALF its
budget (e.g. bunker: FCGI@0.5x = 30.6 dB @ 71 ms vs DDGI@1x 28.9 dB @ 184 ms,
PT@1x 26.5 dB @ 181 ms, single-core-class CPU numbers). Visual montages:
`results/charts/visual_{bunker,courtyard,cavern,town}.png`.

Tried and rejected (see rnd-log for the full autopsy): GFC (per-pixel full
gather — washout), GFCAO (AO modulation — bias), SFG (near/far split gather —
mottling; taught us feedback-convergence and stratification lessons), FCGI2
(longer history — post-edit regression).

Next: deepen bunker/cavern references, fix FCGI's budget scheduler
(coverage > rays-per-face), then GPU compute-shader port.

## Current non-FCGI track (2026-06-11)
Per user direction, new invention work has moved off FCGI, while FCGI/FCGIX/
FCLT remain historical baselines. `PTP` proved that progressive filtered path
tracing with exact ODF traversal and stochastic fractional budgets can beat
plain `PT@1.0x`, but subagent visual review rejected it as a final claim:
bunker/cavern still show fireflies and mottling versus smoother FCGIX/FCLT.

`PTA`/`PTD` established the non-FCGI partial frontier by reusing same-surface
current path samples before temporal accumulation, with sky-only fallback for
light-change scenes. Later planar cleanup experiments made bunker metrics much
stronger, but visual proof remains incomplete. `PTDM` is the newest aggregate
bunker scalar leader: `PTDM@0.35x` frame 149 = `34.58` PSNR / `32.98` gPSNR /
`.8743` SSIM / `.000376` flicker at `66.8 ms`, using slightly fewer rays than
`PTCX`. It adds first-bounce-only stratification on top of `PTCX`, which was the
prior numeric partial (`34.41` PSNR / `32.94` gPSNR / `.8734` SSIM / `.000365`
flicker at `66.8 ms`). `PTCX` adds a reference-free coherent-residual gate to
`PTBR`'s fast planar fit, then damps that gate when the residual fills the whole
local same-surface neighborhood, reducing the worst PTCR patch behavior.

It is still not a promotion. The latest visual sheet shows the same family of
doorway-floor smoothing and green-wall material-separation risk, reduced but
not gone. A 2026-06-11 subagent visual review rejected PTBR as promotable:
it is cleaner but softer than PTBG/REF, with broad-plane flattening, weaker
contact/detail, muted green-wall material, gray-purple wall bias, and remaining
circle/ceiling edge halos. `PTCX` improves scalar and luma/chroma errors, but
focused review sheets still show broad low-frequency wall patches and the green
wall remains under-saturated (`sat_ratio 0.828` vs `PTBR`'s `0.900`). Crop
excess residuals are also more negative than PTBR, so this remains a numeric
partial. The new low-frequency crop diagnostic supports the same read: `PTCX`
lowers green-wall low-frequency error versus PTBR but depresses the residual
ratio, so the failure is over-smoothing/material muting rather than extra noise.
`PTDM` improves aggregate metrics and cleans the doorway slab, but a subagent
visual review rejected it for promotion because the green wall is still more
muted than PTBR/PTCX and the broad wall smoothing remains. `PTDN` full-bounce
stratification is a hard reject due tone collapse. `PTDO`/`PTDP`/`PTDQ` add
increasing broad-plane chroma restore on `PTDM`; `PTDQ` raises green-wall
`sat_ratio` to `0.847` but loses most of the PSNR gain and runs at `73.5 ms`,
and the reviewer still judged it visually too broad, blunt, and not promotable.
`PTDR` then guarded first-bounce stratification by visible-surface material
chroma, but the green-wall crop stayed identical to PTDM (`sat_ratio 0.801`),
showing the blocker is transported radiance chroma rather than camera-surface
albedo. `PTDS`/`PTDT` added a planar-fit chroma-loss blend cap: the aggressive
version proves the signal is real by raising green-wall `sat_ratio` to `0.833`,
but it reintroduces mottling, worsens flicker/SSIM, and costs `93.9 ms`; the
milder version is fast enough (`67.8 ms`) but only reaches `sat_ratio 0.810`.
`PTDU` adds same-surface directional coherence to that chroma-loss signal, but
still over-fires (`34.40` PSNR / `.8697` SSIM / `.000442` flicker at `93.9 ms`)
and leaves visible mottling. `PTDV` uses fit-window mean chroma to suppress
random chroma texture, but it is effectively PTDM-identical on all blocker crops
while still costing `94.7 ms`. `PTDW`/`PTDX` move that chroma signal to connected
same-surface regions. That avoids the worst PTDU speckle preservation, but the
gate is too weak for the blocker and too expensive: `PTDW@0.35` = `34.56` PSNR /
`.8736` SSIM / `.000386` flicker at `75.9 ms`, green-wall `sat_ratio 0.807`;
`PTDX@0.35` = `34.52` / `.8725` / `.000403` at `73.2 ms`, green-wall
`sat_ratio 0.813`. Both stay below `PTCX` green-wall saturation (`0.828`) while
losing the PTDM scalar lead. `PTDY`/`PTDZ` then tried stable temporal-history
chroma directions; both were PTDM-identical on the crops and only added cost
(`71.1`/`79.2 ms`). Loosening the temporal gate proved the signal can move:
`PTDZA`/`PTDZB` raise green-wall `sat_ratio` to `0.847`/`0.843`, but collapse
quality (`~34.31` PSNR / `.8666-.8668` SSIM / `.000474-.000476` flicker) by
preserving cloudy chroma texture. The cheap center-history variant (`PTDZB`) is
still `73.8 ms`, so the failure is not only the radius-2 prepass. `PTDZC`/
`PTDZD` inject a low-pass history chroma field at fitted luma; scalar quality
stays near PTDM, but green-wall saturation gets worse (`0.797`/`0.794`).
`PTDZE`/`PTDZF` saturation-weight that field; they still miss the blocker
(`0.802`/`0.798`), lose scalar quality (`34.46`-`34.48` PSNR), add cost, and
show visible streak/patch artifacts. `PTDZG`/`PTDZH` replaced history RGB
chroma with an explicit first-secondary-bounce transported chroma estimate
(`secondary_albedo * NEE`) before the same bounded luma/chroma injection.
Rejected: `PTDZG@0.35` = `34.50` PSNR / `32.98` gPSNR / `.8743` SSIM /
`.000378` flicker at `71.6 ms`, green-wall `sat_ratio 0.807`; `PTDZH@0.35` =
`34.32` / `32.98` / `.8742` / `.000378` at `82.9 ms`, green-wall
`sat_ratio 0.787`. The focused sheet shows no visual upside over PTCX/PTDM.
`PTDZI`/`PTDZJ` then capped fast-planar blend on dark saturated visible
materials; the green-wall crop was bit-metric-identical to PTDM (`sat_ratio
0.801`) while full-frame quality/runtime regressed (`PTDZI` `34.54` PSNR at
`70.4 ms`; `PTDZJ` `34.50` PSNR at `92.4 ms`). Visible-material saturation is
still the wrong gate for this blocker. `PTDZK`/`PTDZL` tried a direct
green-opponent planar-loss cap and were inert: identical to PTDM on the blocker
while slower. `PTDZM`/`PTDZN` moved the decision earlier by disabling
first-bounce stratification when low-direct history already carries chroma.
They are the first post-PTDM probes to beat PTCX on green-wall crop metrics
(`sat_ratio 0.832`, `lowfreq_res_ratio 0.991-0.992`) while keeping full-frame
PSNR/SSIM above PTCX, but they are not promotions: latest runtimes are
`76.1`/`75.8 ms` and the focused sheet shows some cloudy low-frequency texture
returning. Positive-green-only `PTDZO`/`PTDZP` are more selective but lose the
blocker improvement (`sat_ratio 0.803`/`0.805`) and still cost `75 ms`.
The first follow-up gates did not clear that bar: `PTBS`-`PTBW` traded away
PTBR's PSNR/crop gains, and `PTBX`/`PTBY` post-lift broad-surface residual
restoration worsened gPSNR/SSIM/flicker and cost. `PTBZ`/`PTCA` direct-gradient
planar gating barely moved the result, and `PTCB`/`PTCC` lower-planar/stronger-
lift rebalancing over-flattened crops while damaging gradient/SSIM. `PTCD`-
`PTCF` voxel-contact gating was too weak, and `PTCG` legacy Bayer coverage
undersampled and collapsed tone; corrected `PTCH` still failed. `PTCI`/`PTCJ`
gradient-loss caps recovered too little structure while hurting gPSNR/SSIM/
flicker, and `PTCK`/`PTCL` luma-only planar fitting overshot chroma/saturation
while losing scalar quality. `PTCM`/`PTCN` bounded chroma anchoring reduced that
overshoot but still worsened chroma RMSE and full-frame metrics. `PTCO`/`PTCP`/
`PTCQ` temporal-stability gates were effectively inert. `PTCR`/`PTCS` introduced
the coherent-residual cue, and `PTCW`/`PTCX` added patch damping; `PTCX` is the
numeric winner, but still visually partial. `PTCT`/`PTCU`/`PTCV` added bounded
chroma anchoring to `PTCR`, and `PTCY`/`PTCZ` added luma-preserving chroma
restore to `PTCX`; both color paths recover only small saturation increments
while losing scalar quality or runtime. `PTDA`/`PTDB` then tried a material
albedo hue floor on top of PTCX, but the pass was effectively inert in the
perceptual crops and not worth the added runtime. `PTDC`/`PTDD` material-gated
planar chroma anchors were also effectively inert. `PTDE`/`PTDF` confirmed that
a blurred same-surface low-frequency restore can raise the depressed green-wall
residual ratio, but the gain is too small and costs PSNR/flicker/runtime.
`PTDG`/`PTDH` retuned PTCX's patch damping between PTCX and PTCW; they recover
only tiny saturation increments and remain visually near-identical to PTCX
while losing scalar quality. `PTDI`/`PTDJ` tried multiscale planar residuals,
and `PTDK`/`PTDL` gated that correction to saturated dark materials; all lost
scalar quality/runtime without a visual crop improvement. `PTDM` is a useful
sampling-side numeric partial, but `PTDN` and the `PTDO`/`PTDP`/`PTDQ` chroma
restore follow-ups show that stronger broad restore is not the right fix.
`PTDR`/`PTDS`/`PTDT` narrow that down further: visible-material gating misses
the failure, while direct chroma-loss caps either over-fire or move too little.
`PTDU`/`PTDV` show that simple local coherence or mean-window chroma does not
separate true color bias from texture cheaply enough. `PTDW`/`PTDX` show that
connected-region chroma is cleaner but still not strong or fast enough; the next
useful direction needs temporal evidence or a cheaper transported-radiance model,
not another broader post chroma restore. `PTDY`-`PTDZB` refine that: temporal
history direction is active when thresholds are loose, but raw chroma
preservation is the wrong action. The next plausible fix should model or
low-pass chroma separately, then combine it with the denoised luma/planar fit
instead of capping the fit toward noisy pre-fit RGB. `PTDZC`-`PTDZF` show that a
plain history-window chroma average is not that model: neutral history washes
out the green, and saturation weighting produces sparse/streaky fields.
`PTDZG`/`PTDZH` show that a naive first-secondary `albedo*NEE` chroma side
channel is also insufficient: it cleans neutral wall chroma but does not recover
the green-wall material separation and adds 5-16 ms. `PTDZI`/`PTDZJ` show that
dark saturated visible-material blend capping also misses the blocker.
`PTDZK`/`PTDZL` show source-vs-fit green-opponent loss is not where the color is
lost. `PTDZM`/`PTDZN` finally show the blocker is sensitive to PTDM's
first-bounce stratification; use that as the next lead, but it needs a cheaper
and less cloudy guard than broad history chroma.
`PTBR@0.40` is a numeric/budget partial (`33.66` PSNR / `32.76` gPSNR /
`.8674` SSIM at `69.7 ms`, faster than `PTAI@0.45`) but subagent review
rejected it visually: crop residuals become more negative and the image still
reads over-flattened/material-muted.
Follow-up attempts `PTBH` edge confidence, `PTBI` edge
high-pass restoration, and `PTBJ` luma-only planar scaling were rejected.
`PTBK`/`PTBN` isolated the useful tighter geometry gate, and `PTBL`/`PTBM`
ruled out adaptive material strictness as the cause. `PTBO`/`PTBP`/`PTBQ`/
`PTBR` are the current lift sweep on that branch. Earlier branches `PTAJ`-
`PTBD` are also rejected for tone bias, metric-friendly smoothing, or crop-gate
regressions; see `knowledge/rnd-log.md` for the evidence.

The claim is still bounded: the numeric frontier clears historical baselines on
the frozen scenes, but bunker visual proof is not achieved. See
`knowledge/rnd-log.md` for the evidence table, crop residuals, and rejected
branches.
