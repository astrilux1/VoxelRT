# VoxelRT results (living document)

Started 2026-07-18 per stakeholder decision: this document grows with the
evidence — every campaign's tables land here with the exact commands, hardware
context, and manifest hashes behind them, failure cases included, when they
happen. The final claim write-up is an edit of this file, not a reconstruction.

Rules of this document:

- Nothing lands here without the command that produced it and the manifest
  SHA-256 it was bound to (or an explicit "diagnostic, non-claim" label).
- Speedups are reported as per-scenario ranges (min–median–max), never a
  single number (Lin 2026 reports 2.08×–3.05×, not "2.6×").
- Failure cases and killed hypotheses get sections, not footnotes.
- Claim-v1 = diffuse-only track (frozen claim-manifest v1, closed as a
  milestone). Claim-v2 = materials track (glossy/specular/glass, staged).

---

## Claim-v1 (diffuse-only track)

### Hardware and runtime context

RTX 3080 (Ampere), Windows 11, Chromium via Playwright, WebGPU adapter
nvidia/ampere, fp32 accumulation, timestamp queries enabled. Full adapter
diagnostics are recorded in each results.json.

### v1 baseline convergence campaign (PLAN §7.6) — COMPLETE 2026-07-18

`npm run bench:baseline` (log: `test/eval/logs/baseline-2026-07-18.log`),
6 scenarios × 5 configs × 9 frame checkpoints × 5 repeats, 1920×1080 scale 1,
HDR-FLIP vs frozen 1600-frame rb12 references, GPU cv typically 0.2–4%.
Diagnostic seed budget — direction-check evidence, not claim evidence.
Full tables: `test/eval/analysis.md` (`npm run analyze`).

**Headline (equal-FLIP speedup medians, `npm run analyze` log-log
interpolation, no extrapolation):**

| pair | per-scenario median range | equal-time FLIP ratio range |
|---|---|---|
| **ours vs lin** | **0.87× – 1.19×** | **0.97× – 1.02×** |
| ours vs base | 4.22× – 26.36× | 1.56× – 3.44× |
| lin vs base | 4.40× – 27.08× | 1.51× – 3.52× |

**The honest reading: `ours` is currently at parity with faithful Lin 2026 —
not 2-3×.** The cumulative extensions cost ~2.2 ms/f more spatial-reuse time
(9.7 vs 7.5 ms at 128f interior) and buy back roughly that much quality —
slightly behind lin on interior/exterior static, ~1.18× ahead on the lamps
scenes (heterogeneous emitters favor the power-sampled light list). Both
reuse stacks beat plain PT by 1.5–3.5× lower FLIP at equal time. The 2-3×
claim therefore currently rests entirely on *future* promoted techniques
(Phase 3 ladder, redirected initial-sampling bet) — exactly what PLAN §4
suspected when it listed the unfrozen ratio as blocker #1. That blocker is
now resolved by measurement: **frozen at parity**.

Per-pass GPU cost at 128f interior_static (optimization targets):
base 12.9 ms (pathtrace 12.7); lin 31.7 (pathtrace 22.9, spatial 7.5,
dupmap 0.7); ours 34.2 (pathtrace 23.2, spatial 9.7, dupmap 0.7). Initial
sampling is 68–72% of frame cost in both reuse stacks.

**Methodology gap exposed (fix before the claim campaign):** on `_move`
scenarios every config — including `base` — shows non-monotone FLIP at the
same checkpoints (e.g. interior_move 64f→96f), because each frame count
evaluates at a *different pose*: those curves mix convergence with pose
difficulty. Lin 2026 §7.4 instead captures a fixed view reached through
motion. The static-scenario medians are the trustworthy claim shape; the
move-scenario equal-error extremes (0.15×, 3.35×) are interpolation through
pose changes, not signal. The harness needs a fixed-capture-pose motion mode
for v2.

### Killed hypotheses (v1)

- **World-space GI reuse cache** — killed with evidence 2026-07-18 after three
  materially different designs plus a camera-return probe; screen reuse here
  rebuilds re-exposed regions in ~2 frames, leaving no persistence gap. Full
  record: `docs/WORLDGI.md`, STATUS 2026-07-18. **Re-opens under claim-v2**:
  the kill was reached in a diffuse-only renderer; glossy reuse validity is
  view-dependent and may change the calculus.

### Known limitations of the v1 record

- Diffuse-only (Lambertian + emissive): footprint/reconnection rows
  structurally cannot show their paper deltas (Lin 2026 Fig. 12 shows
  near-zero delta on diffuse scenes).
- No hardware-counter verification of *where* GPU time goes; timestamp-query
  per-pass ms only (toolchain being selected to close this).
- Reference bounce truncation: rb12 vs the ~+0.3% R Neumann tail recorded in
  STATUS; a known, stated bias floor.

---

## Claim-v2 (materials track)

Opens when the glossy stage's pre-registration lands. Staged rollout: GGX
rough conductors → smooth specular mirrors → dielectric glass (PLAN §3).
Scene specs for glossy/specular/glass stress scenarios: pending the scene
research round. Seed-averaged bias maps (`docs/BIASMAP.md`) run against v1
first, then carry over as a standing v2 gate.
