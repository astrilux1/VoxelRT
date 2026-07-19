# Ray-throughput calibration: what a 3080 can actually do

Research round 2026-07-19 (sources at bottom), triggered by the stakeholder
expectation of "30–80 GRay/s at least" from the RTX 3080. Verdict up front:

**No measured system — software or hardware, any GPU, any published paper —
reaches 30–80 GRay/s closest-hit throughput in a non-trivial scene.** The
figure corresponds to RT-core *marketing* ("10 Gigarays", Turing, never
reproduced; Ampere marketing dropped rays/s entirely) or to counting DDA
steps/mask tests as rays. Measured reality:

- **Hardware RT cores on this same RTX 3080**: 5.84 GRay/s primary,
  7.2 GRay/s shadow (Lighthouse 2 benchmark, triangle BVH, fixed-function
  silicon). An RTX 4090 measures 16.9/20.0. The 30–80 expectation exceeds a
  4090's RT cores by 2–4×.
- **Incoherent secondaries on RT cores**: 2.4 GRay/s unsorted, ~3.7 sorted
  (2080 Ti, Bistro).
- **Best published software voxel traversal**, FLOPS-scaled to a 3080:
  ESVO primaries ≈ 0.6–2.2 GRay/s (to 4.7 with beam+contours); shuffled GI
  rays ≈ 0.6–1.1 GRay/s.

**Our measured numbers are at 0.8–1.1× RT-core parity per ray class and at
or above the FLOPS-scaled software state of the art.** Dense 256³: primary
2.4–6.1, shadow 4.7–9.9, bounce 0.7–1.1 GRay/s. Sparse 1024³ (64 m views):
primary 2.75, shadow 2.6, bounce 0.73 GRay/s. The bounce number lands
exactly on the Aila & Laine shuffled-GI FLOPS scaling (0.6–1.1).

First-principles ceilings (3080: 29.8 TFLOPS, 760 GB/s, 5 MB L2): ~10–25
traversal iterations/ray × ~50–150 instructions ⇒ **~5–15 GRay/s absolute
compute ceiling** for cache-resident coherent rays; at 30 GRay/s the DRAM
budget is 25 bytes/ray — two out-of-L2 cache lines per incoherent ray caps
throughput at ~3 GRay/s. The 256³→1024³ drop (~2×) is the L2→DRAM cliff
plus log-depth step growth, not an implementation defect.

## Defensible target bands (1080p, RTX 3080, S64 stackless)

| Ray class | Dense 256³ | Sparse 1024³ | vs measured |
|---|---|---|---|
| Primary | 2.5–6 (stretch 8–10 w/ beam) | 2–4 | at parity |
| Shadow | 5–10 | 2.5–5 | at parity |
| Bounce | 0.7–1.5 (1.5–2.5 w/ reordering) | 0.5–1.2 (to ~2 w/ reordering) | at parity |

Scale-free metric to report alongside rays/s: **Gsteps/s**
(rays/s × mean iterations/ray), the convention the S64-origin work uses.

## Ranked remaining levers (published multipliers)

1. **Ray reordering/sorting for bounce rays**: 1.3–2.0× (Meister et al.) —
   our unsorted compaction already measured neutral-to-negative, consistent
   with the literature: the win requires the *sort*.
2. ~~Ancestor memoization / mirroring / skip-coalescing~~ **MEASURED 2026-07-19: all negative on the 3080** (docs/S64OPT.md §7) — the source guide's multipliers came from an integrated GPU; on a discrete card with a cache-resident TLAS the levers cost more than they save. Remaining credible levers: reordering (1) and LOD (3).
2. (superseded) **Ancestor-node memoization in stackless re-descent**: ~2× claimed in
   the S64-origin guide — our stackless kernel re-descends from the TLAS
   root every relocation, no memoization: **the largest unimplemented
   lever**. Bitmask skip-coalescing (+21%) and ray-octant mirroring (+10%)
   from the same source also unverified in our kernel.
3. **LOD/mip-DDA for distant rays** (Teardown's core trick): uncapped
   steps/ray reduction at 64 m+ views; converts the 1024³ penalty back
   toward 256³ behavior. Changes image content ⇒ quality-gated, not
   identity-gated.
4. **Beam/frustum pre-pass for primaries**: +5–40% (Laine & Karras).
5. **Shadow any-hit path**: 1.2–2× over closest-hit shadow.
6. Persistent threads: ≤1.1–1.2× on modern schedulers (was 1.5–2.2× in
   2009); speculative traversal: ≈0 for diffuse. Low priority.

Per docs/S64.md §4's budget ("no third design without re-registration"),
memoization/mirroring/any-hit constitute a new registered experiment before
implementation.

## Sources

Laine & Karras 2010 (ESVO) · Aila & Laine 2009 · dubiousconst282 sparse
64-tree guide + VoxelRT benchmark tables (the S64 origin) · van Wingerden
brickmap thesis · NanoVDB GPU blog · Lighthouse 2 RTX benchmark (Bikker) ·
Meister et al. ray-reordering (arXiv 2506.11273) · ACM SIGMETRICS 2025
RT-core case study · Hybrid Voxel Formats (arXiv 2410.14128) · Teardown
frame breakdowns (acko.net, juandiegomontoya). Full URLs in the research
transcript; key tables reproduced in RESULTS.md when cited.
