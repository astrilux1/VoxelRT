# Pipeline operating notes (read before working in this repo)

## Build & run
- `make bench` (g++ -O2 -fopenmp, 2 threads). Binary: ./bench
- `./bench run <scene> <method> <mult>` appends 150 frames to
  results/results.csv (header: scene,method,mult,frame,psnr,ssim,gpsnr,
  flicker,rays,ms,steps). Event at frame 75. Images dumped at mult=1.0
  (frames 30,74,76,80,90,149) as .ppm; PTR also dumps these frames at
  lower budgets for visual review. Convert with `python3 tools/img.py
  convert` when Python is available, or a PowerShell/.NET PPM converter on
  Windows.
- `./bench runpost <scene> <method> <mult> [frames]` starts directly from the
  post-event state with zero method history and writes results/coldstart.csv.
  Use it to distinguish stale pre-event history from post-event estimator
  artifacts.
- `./bench ref <scene> <pre|post> <spp> <seed>` ACCUMULATES into
  results/raw/*.raw (count in 8-byte header). References are FROZEN as of
  2026-06-10 (bunker 560/480, cavern 504/480, courtyard 392/360, town
  416/376). If you top them up, re-run every config you want to compare,
  because psnr is computed at run time against the current raw.
- Charts/summary: `python3 tools/charts.py` -> results/charts/.
- Scenes: bunker (indirect-only rooms, wall blast), courtyard (sun/sky
  relight event), cavern (HDR emissive, ceiling collapse), town (320x96x320
  outdoor). Methods: PT, PTO, PTP, PTC, PTR, PTV, PTS, PTL, PTM, PTU, PTA,
  PTD, PTE, PTB, PTF, PTH, PTK, PTN, PTW, PTJ, PTI, PTQ, PTY, PTZ, PTAA,
  PTAB, PTAC, PTAD, PTAE, PTAF, PTAG, PTAH, PTAI, PTAJ, PTAK, PTAL, PTAM,
  PTAN, PTAO, PTAP, PTAQ, PTAR, PTAS, PTAT, PTAU, PTAV, PTAW, PTAX, PTAY,
  PTAZ, PTBA, PTBB, PTBC, PTBD, PTBE, PTBF, PTBG, PTBH, PTBI, PTBJ, PTBK,
  PTBL, PTBM, PTBN, PTBO, PTBP, PTBQ, PTBR, PTBS, PTBT, PTBU, PTBV, PTBW,
  PTBX, PTBY, PTBZ, PTCA, PTCB, PTCC, PTCD, PTCE, PTCF, PTCG, PTCH, PTCI,
  PTCJ, PTCK, PTCL, PTCM, PTCN, PTCO, PTCP, PTCQ, PTCR, PTCS, PTCT, PTCU,
  PTCV, PTCW, PTCX, PTCY, PTCZ, PTDA, PTDB, PTDC, PTDD, PTDE, PTDF, PTDG,
  PTDH, PTDI, PTDJ, PTDK, PTDL, PTDM, PTDN, PTDO, PTDP, PTDQ, PTDR, PTDS,
  PTDT, PTDU, PTDV, PTDW, PTDX, PTDY, PTDZ, PTDZA, PTDZB, PTDZC, PTDZD,
  PTDZE, PTDZF, PTDZG, PTDZH, PTDZI, PTDZJ, PTDZK, PTDZL, PTDZM, PTDZN,
  PTDZO, PTDZP, PTG, PTX, DDGI, FCGI/FCGIX/FCLT (historical face-cache
  baselines), GFC, GFCAO, SFG, FCGI2.

## Visual gate diagnostics
- `tools/bunker_crop_metrics.ps1` compares the bunker post reference and
  candidate PPMs on reviewer-targeted crops: circular opening surround,
  gray ceiling/wall, doorway slab, and green wall. Report crop PSNR plus
  excess planar-residual RMS; the latter tracks the cloudy low-frequency
  residue that full-frame PSNR/SSIM under-rate. Use `-Csv` for exact values;
  the default table display rounds small residuals too aggressively.
- `tools/bunker_perceptual_metrics.ps1` extends that gate with reviewer-facing
  signals: gradient preservation, planar-residual ratio, low-frequency residual
  ratio/error, edge-band error, green-opponent error, chroma RMSE, and
  saturation ratio. Use it when a scalar PSNR gain may be hiding broad-plane
  flattening or material-color drift.
- `tools/bunker_review_sheet.ps1` builds a labeled PNG sheet for the same
  crops, currently including REF, FCGIX, PTAA, PTAI, PTBG, PTBH, PTBI, PTBJ,
  PTBK, PTBN, PTBO, PTBQ, `PTBR@0.35`, `PTBR@0.40`, and the latest rejected
  PTCI/PTCJ/PTCK/PTCL/PTCM/PTCN/PTCO/PTCP/PTCQ/PTCR/PTCS/PTCT/PTCU/PTCV/
  PTCW/PTCX/PTCY/PTCZ/PTDA/PTDB/PTDC/PTDD/PTDE/PTDF/PTDG/PTDH/PTDI/PTDJ/
  PTDK/PTDL/PTDM/PTDN/PTDO/PTDP/PTDQ/PTDR/PTDS/PTDT/PTDU/PTDV/PTDW/PTDX/PTDY/
  PTDZ/PTDZA/PTDZB/PTDZC/PTDZD/PTDZE/PTDZF/PTDZG/PTDZH/PTDZI/PTDZJ/PTDZK/
  PTDZL/PTDZM/PTDZN/PTDZO/PTDZP probes. Use it before asking for visual review. The script also accepts an `-Images`
  override for focused sheets.
- Latest gate result: `PTDM` is the strongest aggregate bunker numeric partial,
  but not yet a promotion. It adds first-bounce-only stratification to `PTCX`:
  bunker frame 149 `PTDM@0.35` = `34.58` PSNR / `32.98` gPSNR / `.8743` SSIM /
  `.000376` flicker at `66.8 ms` and `59310` rays. Subagent visual review kept
  it at "promising but not promotable": doorway slab cleanup looks real, but the
  green wall is more desaturated/muted than PTBR/PTCX and broad wall smoothing
  remains. `PTDN` full-bounce stratification is a hard reject due visible tone
  collapse. `PTDO`/`PTDP`/`PTDQ` chroma restore on `PTDM` does not change the
  verdict; `PTDQ` raises green-wall `sat_ratio` to `0.8471`, but loses most of
  the PSNR lead (`34.47`) and runs at `73.5 ms`, while the reviewer still sees
  a blunt, broad restore with muted dark colored surfaces.
- Latest follow-up probes are also rejected. `PTDR` guards first-bounce
  stratification by visible-surface material chroma; it improves gPSNR/SSIM
  (`34.55 / 32.99 / .8746`) but runs at `73.8 ms` and leaves the green-wall
  crop identical to PTDM (`sat_ratio 0.8010`), so the blocker is transported
  radiance chroma, not camera-surface albedo. `PTDS` gates planar fit blend on
  relative chroma loss; it proves the signal is active by raising green-wall
  `sat_ratio` to `0.8325`, but drops SSIM to `.8722`, raises flicker to
  `.000407`, costs `93.9 ms`, and visibly reintroduces mottled wall texture.
  `PTDT` is a milder sweep (`34.57 / 32.98 / .8741` at `67.8 ms`) but only
  raises green-wall `sat_ratio` to `0.8104`, still below PTCX. `PTDU` adds
  same-surface directional coherence to the chroma-loss signal; it reaches
  green-wall `sat_ratio 0.8251`, but craters full-frame quality (`34.40 /
  32.91 / .8697 / .000442`) and costs `93.9 ms`, so it still protects mottled
  chroma texture. `PTDV` uses fit-window mean chroma; it is PTDM-identical on
  all blocker crops but still runs at `94.7 ms`, so the mean cue is too inert.
  `PTDW`/`PTDX` gate planar-fit chroma loss by connected same-surface components
  instead of per-pixel or 3x3 support. They are cleaner than PTDU but not
  promotable: `PTDW@0.35` = `34.56 / 32.97 / .8736 / .000386` at `75.9 ms`,
  green-wall `sat_ratio 0.8072`; `PTDX@0.35` = `34.52 / 32.96 / .8725 /
  .000403` at `73.2 ms`, green-wall `sat_ratio 0.8127`. Both remain below PTCX
  green-wall saturation (`0.8279`) and below PTDM scalar quality. `PTDY`/`PTDZ`
  use stable temporal-history chroma directions; both are PTDM-identical on the
  crop metrics while adding cost (`71.1`/`79.2 ms`). Loose temporal variants
  `PTDZA`/`PTDZB` prove the history direction can recover green-wall chroma
  (`sat_ratio 0.8469`/`0.8431`) but fail badly: `PTDZA@0.35` = `34.31 / 32.86 /
  .8668 / .000476` at `111.9 ms`; `PTDZB@0.35` = `34.31 / 32.86 / .8666 /
  .000474` at `73.8 ms`, with visible cloudy chroma texture. `PTDZC`/`PTDZD`
  inject a low-pass history chroma field at fitted luma; scalar quality stays
  near PTDM but green-wall saturation worsens to `0.7972`/`0.7943`.
  `PTDZE`/`PTDZF` saturation-weight the low-pass chroma source; they still fail
  (`34.4581 / 32.8752 / .87063 / .000425` at `71.4 ms`, and `34.4756 /
  32.8917 / .87176 / .000386` at `77.6 ms`), with green-wall `sat_ratio`
  only `0.8022`/`0.7977` and visible streak/patch artifacts. `PTDZG`/`PTDZH`
  replace history RGB chroma with a first-secondary-bounce transported chroma
  estimate (`secondary_albedo * NEE`) before the same bounded luma/chroma
  injection. Rejected: `PTDZG@0.35` = `34.4988 / 32.9829 / .87427 / .000378`
  at `71.6 ms`, green-wall `sat_ratio 0.8066`; `PTDZH@0.35` = `34.3222 /
  32.9818 / .87416 / .000378` at `82.9 ms`, green-wall `sat_ratio 0.7873`.
  The focused sheet stays muted versus PTCX/PTDM, so this side-channel is too
  weak and too costly. `PTDZI`/`PTDZJ` cap fast-planar blend on dark saturated
  visible materials. They are rejected because the green-wall crop is unchanged
  from PTDM (`sat_ratio 0.8010`, `lowfreq_res_ratio 0.9686`) while full-frame
  quality/runtime regresses: `PTDZI@0.35` = `34.5429 / 32.9673 / .87319 /
  .000384` at `70.4 ms`; `PTDZJ@0.35` = `34.4979 / 32.9460 / .87180 /
  .000396` at `92.4 ms`. `PTDZK`/`PTDZL` use source-vs-fit green-opponent
  loss to cap planar blend and are inert: bit-metric-identical to PTDM on the
  crops while slower (`77.9`/`83.2 ms`). `PTDZM`/`PTDZN` are useful partials:
  they disable first-bounce stratification when low-direct history already
  carries chroma, raising green-wall `sat_ratio` to `0.8319`/`0.8316` and
  `lowfreq_res_ratio` to `0.9914`/`0.9920`, beating PTCX on those crop metrics
  while keeping full-frame PSNR/SSIM above PTCX. They are not promotions because
  the latest rerun costs `76.1`/`75.8 ms` and the focused sheet shows cloudy
  texture returning. Positive-green-only `PTDZO`/`PTDZP` are more selective but
  lose the blocker improvement (`sat_ratio 0.8033`/`0.8047`) and still cost
  about `75 ms`.
- Prior gate result: `PTCX` keeps `PTBR`'s broad-plane lift and tighter geometry gate, then
  caps the high fast-planar blend where local fitted-plane residuals have
  coherent same-sign support, with patch damping when that residual fills the
  local same-surface neighborhood. Bunker frame 149: `PTCX@0.35` = `34.41`
  PSNR / `32.94` gPSNR / `.8734` SSIM / `.000365` flicker at `66.8 ms`, same
  rays as `PTBR@0.35` and better/faster than `PTBR@0.40`. Crop PSNR improves
  over PTBR on all blocker crops, but excess planar residuals become more
  negative (circle `-0.00231`, ceiling `-0.00248`, doorway `-0.00248`, green
  `-0.00279`) and focused sheets still show broad wall patches plus
  under-saturated green-wall material (`sat_ratio 0.828` versus `PTBR`
  `0.900`). The low-frequency diagnostic shows PTCX lowers green-wall
  low-frequency error versus PTBR but also depresses the low-frequency residual
  ratio, matching the visual read of over-smoothing/material muting rather than
  extra noise. Fresh subagent visual
  review still applies to the PTBR family: cleaner scalar results are not
  enough while broad-plane flattening/material loss remains visible.
  Simple lift gates and post-lift residual restoration have now failed; the
  next useful experiment needs a different reference-free cue for broad-plane
  bias/contact preservation, not another scalar lift-strength adjustment.
  `PTBS`/`PTBT`/`PTBU`/`PTBV`/`PTBW` tried lift gating; `PTBX`/`PTBY` tried
  post-lift broad-surface residual restoration; `PTBZ`/`PTCA` tried direct-
  gradient planar-fit gating; `PTCB`/`PTCC` tried lower planar flattening plus
  stronger lift; `PTCD`/`PTCE`/`PTCF` tried voxel-occupancy contact gating; and
  `PTCG`/`PTCH` retried Bayer low-discrepancy pixel gates. `PTCI`/`PTCJ` tried
  planar-fit gradient-loss caps; `PTCK`/`PTCL` tried chroma-preserving luma-only
  planar fitting; `PTCM`/`PTCN` tried bounded chroma anchoring between RGB and
  luma-only fits; `PTCO`/`PTCP`/`PTCQ` tried temporal-stability gating for the
  high area-driven planar blend. All were rejected by
  the bunker gate: the simple gates trade away PTBR's PSNR/crop gains or barely
  move the image, residual restoration worsens gPSNR/SSIM/flicker and cost,
  planar/lift rebalancing over-flattens crops while damaging gradient/SSIM,
  voxel-contact gating is too weak, and Bayer coverage collapses tone even after
  fixing the old underspend. The gradient-loss caps recover too little structure
  and add flicker; luma-only fitting improves green-wall structure but overshoots
  chroma/saturation; bounded chroma anchoring avoids the overshoot but still
  worsens chroma RMSE and scalar quality; temporal-stability gating is inert.
  `PTCR` was the first large scalar move from a reference-free coherent residual
  cue, and `PTCW`/`PTCX` patch damping improved it further; `PTCX` is still
  visually partial. `PTCT`/`PTCU`/`PTCV` added bounded chroma anchors on top of
  PTCR, and `PTCY`/`PTCZ` added luma-preserving chroma restore on top of PTCX.
  Both color paths recover too little saturation for their scalar/runtime cost.
  `PTDA`/`PTDB` tried a material albedo hue floor on top of PTCX, but the pass
  was effectively inert in the perceptual crops and not worth the runtime.
  `PTDC`/`PTDD` material-gated planar chroma anchoring was also effectively
  inert. `PTDE`/`PTDF` restored blurred same-surface low-frequency differences;
  this moved green-wall `lowfreq_res_ratio` toward PTBR/reference, but lost
  scalar quality and added too much runtime. `PTDG`/`PTDH` retuned patch damping
  between PTCX and PTCW; the focused sheet remains visually near-identical to
  PTCX and the scalar metrics regress. `PTDI`/`PTDJ` tried small-radius planar
  residual correction inside the large-radius fit; `PTDK`/`PTDL` gated that
  correction to saturated dark materials. The ungated versions move green-wall
  saturation/residual ratio only by giving up scalar quality and runtime; the
  gated versions are effectively inert on the blocker crops. `PTDM` first-bounce
  stratification is the current scalar high-water mark, but `PTDN` and
  `PTDO`/`PTDP`/`PTDQ` show the next useful work is not full-bounce
  stratification or more global chroma restore. `PTDR` shows visible-material
  gating is the wrong target, while `PTDS`/`PTDT` show a raw chroma-loss blend
  cap is either too noisy or too weak. `PTDU`/`PTDV` show simple local coherence
  and fit-window mean chroma are not enough. `PTDW`/`PTDX` show connected-region
  chroma avoids the worst local mottling but does not recover enough transported
  color and adds too much CPU cost. `PTDY`/`PTDZ` are too inert, while
  `PTDZA`/`PTDZB` show temporal direction can recover color only by preserving
  cloudy raw chroma. `PTDZC`/`PTDZD` show a plain low-pass history chroma field
  is washed out by neutral surfaces; `PTDZE`/`PTDZF` show saturation weighting
  makes that field sparse/streaky instead of correct. `PTDZG`/`PTDZH` show the
  simple first-secondary `albedo*NEE` transported-chroma side channel is also
  too weak on the green-wall blocker and too slow. `PTDZI`/`PTDZJ` show a
  visible-material dark/saturated blend cap also misses the blocker, and
  `PTDZK`/`PTDZL` show source-vs-fit green-opponent planar loss is inert.
  `PTDZM`/`PTDZN` identify the best lead: broad history-chroma fallback from
  first-bounce stratification restores the green-wall crop but is too slow and
  somewhat cloudy. The next experiment should make that fallback cheaper and
  more spatially selective without collapsing to the ineffective
  positive-green-only gate.
  `PTBR@0.40` is a numeric/budget partial (`33.66` / `32.76` / `.8674` at
  `69.7 ms`, faster than `PTAI@0.45`),
  but subagent review still rejected it because crop residuals move further
  negative and the image remains over-flattened.
  `PTBH` edge-confidence gating, `PTBI` clamped edge high-pass restoration, and
  `PTBJ` luma-only planar scaling were rejected. `PTBK`/`PTBN` showed the
  tighter geometry gate is useful; `PTBL`/`PTBM` showed adaptive material
  strictness is either duplicated or inert. `PTBO`/`PTBP`/`PTBQ`/`PTBR` are a
  lift sweep on that geometry-gated branch; PTCX is the prior numeric partial,
  and PTDM is the current aggregate scalar leader but not visually promotable.
- Do not repeat already-rejected acceptance/sampling variants without a new
  bias diagnostic: `PTAU` stale history, `PTAV` winsorization, `PTAW` variance
  sampling, `PTAX` center preservation, `PTAY` stochastic dropout, `PTAZ`
  fractional borrowed-history confidence, `PTBA` stricter history disagreement,
  `PTBC` first-bounce stratification, and `PTBD` larger planar fit all failed
  the bunker crop gate in different ways.

## Environment quirks (cost hours — don't rediscover)
- Shell calls have a hard 45 s limit and background processes DO NOT survive
  between calls (nohup/setsid useless). Long work must be chunked: one
  ./bench run per call (`timeout 42 ...`), reference accumulation via
  tools/topup*.sh (~35 s per call). PT/DDGI at 2x on bunker exceed the
  window and cannot be run at all.
- In the 2026-06-11 Windows shell, `python`/`python3` were not on PATH even
  though the repo has Python chart/image scripts. Use PowerShell/.NET for
  quick CSV summaries or restore Python before regenerating charts.
- Editing EXISTING files through the desktop file tools can TRUNCATE them at
  their original byte size on the Linux mount. Safe paths: create NEW files,
  or edit in place from bash (python/sed/cat). If a build suddenly shows
  hundreds of errors, check for truncation first (`tail` the file).
- results/*.csv cannot be rm'd from bash (permission), but can be rewritten:
  `grep -v ... > /tmp/r.csv && cat /tmp/r.csv > results/results.csv`.
  A run killed by the 45 s limit leaves partial rows — always check
  `awk -F, 'NR>1{print $1,$2,$3}' results/results.csv | sort | uniq -c |
  awk '$1!=150'` and scrub.

## Methodology guards
- gpsnr (gradient PSNR) is the anti-blur-gaming metric; report it alongside
  psnr. flicker = temporal stability (frames >=100).
- ALWAYS do visual review: per-method abs-error heatmaps vs ref caught every
  real defect this session; psnr alone ranked GFC's washout above PT.
- Energy-ratio check for any new technique: mean tonemapped rgb of frame-74
  image / ref must be 0.99-1.01. Catches unit bugs (a pi^2 bug produced
  3.8x) and slow-convergence (0.94 = equilibrium not reached).
- Bunker/cavern psnr above ~31 dB is compressed by reference noise; use
  courtyard/town to discriminate <1 dB differences.
