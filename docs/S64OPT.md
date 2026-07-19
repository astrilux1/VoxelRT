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

## 4. Cost budget

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

Not yet run.

## 8. Terminal state and redirect

Pre-registered 2026-07-19; queued behind the v1 bias-map runs.
