# S64 traversal optimization rung (`s64_trace_stackless` v2.1) — design and result

One-paragraph summary: pre-registered follow-up to the S64 promotion,
required by docs/S64.md §4's budget rule before any third traversal design.
Implements the three highest-ranked unimplemented levers from
docs/THROUGHPUT.md (ancestor memoization ~2× claimed at source, bitmask
skip-coalescing +21%, ray-octant mirroring +10%) plus a shadow any-hit
entry point (1.2–2× band). Terminal state: **pre-registered 2026-07-19,
not yet implemented.**

## 1. Hypothesis

The stackless kernel's full root re-descent on every relocation discards
locality that the source design memoizes: most relocations land in the same
chunk (64³) or same L1 cell (16³) as the exited cell, so re-entering from a
memoized ancestor skips 2–4 mask reads per step. Combined with mirroring
(sign-free DDA) and coalesced bitmask skips, the guide attributes ~2×;
shadow rays additionally never need normals/materials (any-hit).

## 2. Denominator

Existing native bench, sparse1024 + fixture256, `--wg 8x16`, warm
interleaved rounds, vs the promoted stackless kernel as control. Metrics:
Mrays/s per class + Gsteps/s (add an iterations/ray counter to the bench
first so both are reported).

## 3. Design and invariants

Memoized re-descent: retain the current chunk root + L1 node (masks +
childBase, ~6 registers); on relocation, test containment innermost-first
and re-enter at the deepest containing level; fall back to root descent on
chunk exit. Octant mirroring: transform ray to all-positive direction
octant, mirror cell bit indexing accordingly. Skip-coalescing: consume
contiguous zero-mask runs along the DDA axis in one step where the mask
word allows. Any-hit shadow: `trace_occluded() -> bool` skipping
normal/material bookkeeping. Invariants: identity gate v2 unchanged for
primary/bounce paths; shadow validated by exact occlusion-count equality
vs the control on both scenes (same rays, boolean result); determinism
unchanged.

Implementation notes (appended 2026-07-19, implementation pass; §1–6
registered text above unchanged): landed in
`native/shaders/s64_trace_stackless_opt.wgsl` behind per-lever consts
(`ENABLE_MEMO` / `ENABLE_MIRROR` / `ENABLE_SKIP` / `ENABLE_ANYHIT`),
selected via `--traversal stackless-opt`; `--opt-levers
<memo,mirror,skip,anyhit|none>` string-toggles the consts (absent = all
on). The promoted control kernel is untouched. Mirroring reflects through
the ORIGIN (x → −x, world interval [−1024, 0) on mirrored axes) rather
than through GRID: IEEE negation is exact, so all t arithmetic is
bitwise-identical to the control (a GRID − x transform was measured on the
CPU ports at ~0.1% knife-edge divergence from transform rounding and was
rejected). Skip-coalescing advances tMax by serial additions, so committed
jumps are bitwise-identical to per-cell stepping. `trace_occluded()`
shape: bench.wgsl's group-0 interface and entry-point set are frozen and
`trace()` cannot know its caller, so any-hit is a per-MODULE
specialization instead — the host compiles a second module with
`ENABLE_ANYHIT=true` and builds only the shadow/shadow_compact pipelines
from it; `trace()` then skips the material fetch and normal bookkeeping
(t, and therefore the occlusion boolean and counts, unchanged). CPU
cross-check (`cargo test`, native/src/verify.rs): a deterministic
12,468-ray bundle (axis-aligned / diagonal / grazing / random /
short-maxT) through 1:1 Rust ports asserts bitwise (t, voxel, material,
normal) equality vs the control port for memo / skip / anyhit
(individually and combined) with lever-engagement counters, gate-v2
tolerance with 0 observed divergences for mirror, and anchors the control
port against an independent f64 voxel-march oracle (0 divergences);
row-mask emptiness, mirrored bit indexing and memo containment are
property-tested. GPU measurement, occupancy check (§4) and the gates
remain queued.

No new buffers; register budget must stay ≤ current (verify occupancy via
GPU Trace before/after). Implementation: one design pass, no variant
explosion — the three levers land as independently toggleable const flags
so each is measured in isolation (Table-1 discipline).

## 5. Acceptance criteria

1. Gates: identity v2 passes (primary/bounce); shadow occlusion counts
   byte-equal to control on both scenes.
2. Promotion of a lever: ≥ +10% on ≥ 2 ray classes at sparse1024 with no
   class regressing > 5%, measured in isolation.
3. The rung overall replaces the promoted kernel only if the combined
   toggles reach ≥ +25% bounce or ≥ +20% aggregate (equal-weight classes)
   at sparse1024.

## 6. Kill condition and budget

Any lever measuring < +5% in isolation is dropped (recorded, not tuned).
If the combined best config misses criterion 3, the rung is closed with
the table and the promoted kernel stands. Budget: one implementation pass
per lever + one combined measurement session; no second redesign without
re-registration.

## 7. Results

Measured 2026-07-19 (RTX 3080, sparse1024, `--wg 8x16`, 3 warm interleaved
rounds; ms primary/bounce/shadow, control = promoted stackless):

| config | primary | bounce | shadow | verdict (§5.2: ≥+10% on ≥2 classes) |
|---|---|---|---|---|
| control | 0.751 | 1.232 | 0.345 | — |
| opt[none] anchor | 0.735 | 1.228 | 0.348 | ≈control ✓ (harness sane) |
| memo | 0.827 | 1.390 | 0.378 | **dropped: −10% everywhere** |
| mirror | 0.828 | 1.434 | 0.419 | **dropped: −11…−21%** |
| skip | 1.014 | 1.914 | 0.496 | **dropped: −35…−55%** |
| anyhit | 0.735 | 1.227 | 0.348 | **dropped: neutral (<+5%)** |
| all four | 1.363 | 2.539 | 0.623 | compounding losses, ~2× slower |

Correctness held throughout: identity gate v2 passed on both scenes (1 and
22 divergent pixels, unchanged), shadow occlusion counts byte-equal to
control, all-lever CPU cross-checks bitwise-clean. The implementations are
faithful; the *hypotheses* failed on this kernel/hardware/scene:

- Memoization: the promoted kernel's root re-descent is ≤4 mask reads from
  a 16.9 KB cache-resident TLAS; the memo branch adds ~8 live registers
  and containment tests that cost more than the reads they save.
- Mirroring: the removed per-axis sign-selects were already cheap
  (uniform-per-ray branches); the added coordinate transforms were not.
- Skip-coalescing: the row test executes every inner iteration; empty
  rows along the dominant axis are too rare at sparse1024 leaf occupancy
  to amortize it.
- Any-hit: shadow rays here terminate in ~1–3 steps and fetch one
  material per ray — there was nothing material to skip.

The source guide's multipliers (~2× memoization, +21% skip) were measured
on an integrated GPU with a far smaller cache and different
compute/bandwidth ratio — lever multipliers are platform-contingent, and
this rung is the measured proof for the discrete-GPU case.

## 8. Terminal state and redirect

**Closed 2026-07-19: all levers killed per §6; the promoted stackless
kernel stands unmodified.** The opt kernel and its CPU cross-check suite
stay in-tree as measurement infrastructure (and as the worked example that
faithful implementations of published optimizations can be honest
negatives). Redirect: per docs/THROUGHPUT.md the remaining credible
traversal levers are ray *reordering* for bounce rays (1.3–2×, requires a
sort — the unsorted compaction negative already bounds the no-sort case)
and LOD/mip-DDA for distant rays (quality-gated, not identity-gated). Both
are claim-v2-scale experiments, not micro-opts.
