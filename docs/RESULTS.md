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

### v1 baseline convergence campaign (PLAN §7.6)

Status: **running 2026-07-18** (`npm run bench:baseline`, log:
`test/eval/logs/baseline-2026-07-18.log`). 6 scenarios × 5 configs
(`base`/`gi`/`lin`/`ours_unbiased`/`ours`) × 9 frame checkpoints × 5 repeats,
1920×1080 scale 1, HDR-FLIP vs frozen 1600-frame rb12 references.

Tables and equal-time curves land here from `npm run analyze` when the
campaign completes. Note: diagnostic seed budget (5 repeats), not the claim
manifest's frozen quality-seed budget — direction-check evidence, not claim
evidence.

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
