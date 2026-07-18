# World-space GI reuse cache (`?worldgi=1`) — design and result

Phase 4 of `docs/PLAN.md`. This was the project's primary voxel-native research
bet: stable brick/face identities let a transported-radiance sample be reused
by *any* receiver that later lands in the same world cell, so GI history should
survive disocclusion, camera return, and newly exposed surfaces that
screen-space temporal reuse cannot follow.

> **VERDICT (2026-07-18): killed with evidence.** Across three materially
> different designs and a dedicated camera-return probe (§7), the cache never
> beat its exact screen-only control (`ours`). Applied everywhere it produces
> brick-blocky GI (FLIP ~4× worse); gated to disocclusions it is neutral
> (FLIP within ±0.3%, a ~1–2% relRMSE nudge). Root cause: this renderer's
> screen-space reuse — voxel-exact plane reuse + strong emissive NEE +
> disocclusion rescue — already reconstructs re-exposed surfaces within ~2
> frames, leaving almost no gap for a persistent cache to fill. The code stays
> flag-gated and off by default as reproducible evidence. Per PLAN §4C this is a
> stop-and-document outcome, not an invitation to unbounded granularity tuning.
> The redirect (PLAN §4/§5): the remaining quality bottleneck is **initial
> sampling**, not reuse persistence — pursue voxel-aware initial proposals
> (the ReGIR-style light grid), not more reuse machinery.

This document is written before the code (PLAN §4A.2) so the estimator and the
null/empty accounting are fixed up front — the same discipline that caught the
null-reservoir energy inflation.

## 1. Why a voxel cache is unusually cheap here

A unified reservoir sample is either a surface point `x2` carrying
direction-independent outgoing radiance `Lo` (every voxel surface is
Lambertian), or a distant direction. The reconnection shift between two
receivers is therefore a pure re-evaluation of the *receiver's own* geometry
term with **Jacobian ≡ 1** — `evalF(sample, x1, n1)` already computes exactly
this (see `restir.wgsl`). Screen-space spatial reuse (`reuse_spatial.wgsl`) and
temporal reprojection (`reuse_temporal.wgsl`) both already exploit this to merge
reservoirs from *different* receivers. A world-space cache is the same GRIS
merge with the neighbour sourced from a persistent world cell instead of a
screen pixel; nothing about the transport math is new.

## 2. Receiver key

```
cell(x1, n1) = brickIndex(x1) * 6 + faceBucket(n1)
brickIndex(x1) = bx + by*BGRID + bz*BGRID*BGRID,  b = floor(x1 / (BRICK*VOXEL_SIZE))
faceBucket(n1) = dominant axis of n1 (0..2) * 2 + (n1[axis] < 0)   -> 0..5
```

- `BGRID = GRID/BRICK = 256/8 = 32`. Cells = `32³ * 6 = 196 608`.
- The brick coordinate is stable under camera motion and re-exposure; the
  6-way face bucket keeps opposite-facing surfaces of the same brick from
  sharing a slot (their cosine terms and visibility differ completely).
- This is the bounded first prototype PLAN §4A.1 asks for: one reservoir per
  (brick, face orientation), not per voxel face.

## 3. Storage and memory budget

- One `Reservoir` (32 B) per cell. The final plane-exact key (§2, with the
  8 sub-brick voxel layers) is `32³ × 6 × 8 × 32 B ≈ 50 MB`, single persistent
  buffer, created once (resolution-independent). Allocated only when
  `?worldgi=1` (a 32 B stub otherwise), since the feature is off by default.
- WebGPU zero-initialises the buffer, and a zero reservoir decodes as `SK_NONE`
  with confidence 0 — i.e. a fresh cache is *empty*, which is the inert state.
- No ping-pong: the cache is a running world history. Persistence across frames
  is the entire point (it is what screen-space history lacks).

## 4. Estimator

Two operations, both flag-gated on `RF_WORLDGI`, no new pass:

### 4A. Query (final: in `reuse_temporal.wgsl`, disocclusion `!found` branch)

The query started in `pathtrace.wgsl` (applied to every receiver — Design 1/2,
blocky) and was moved to `reuse_temporal.wgsl`'s disocclusion branch (Design 3):
only when screen-space temporal reprojection finds no valid history does the
receiver's cell reservoir `wc` get GRIS-merged into the fresh reservoir, using
the **same confidence-weighted balance-heuristic merge** as the history merge
(with the `?wgicap=` confidence cap on `wc.c`):

```
cF = out.c            // fresh confidence (=1)
cW = wc.c             // cached confidence (no cap — unbiased prototype)
pF = evalTarget(out, x1, n1)      // == the fresh selection target
pW = evalTarget(wc,  x1, n1)      // cached sample re-evaluated at THIS receiver
m  = max(cF + cW, eps)
wF = (cF/m) * pF * max(out.W, 0)
wW = (cW/m) * pW * max(wc.W, 0)
S  = wF + wW
select out (prob wF/S) else wc;  merged.W = S / target(selected);  merged.c = cF + cW
```

- **Empty cell** ⇒ `wc.kind = SK_NONE`, `cW = 0`, `pW = 0` ⇒ `wW = 0`, and the
  merge returns `out` with `W = wF/pF = out.W` and `c = 1` **bit-for-bit
  unchanged**. This is the inertness guarantee (§7 test).
- **Null fresh sample** (`out.kind = SK_NONE`, failed initial) but non-empty
  cell ⇒ `pF = 0`, `wF = 0`, so the cache sample is adopted — this is the
  disocclusion / re-exposure rescue the cache exists for.
- **Both null** ⇒ `S = 0`; keep `SK_NONE` and write pooled confidence
  `c = cF + cW`, exactly as the temporal merge pools nulls. A cache miss is a
  zero-valued outcome of an applicable domain, never a dropped domain — dropping
  it would reintroduce the `E[w | found] = E[w]/(1-q)` inflation.

The merged reservoir then flows through temporal/spatial/shading unchanged, so
a surviving cache sample still gets the `RF_FULLV` visibility retest at shading
under `ours` (controls ReSTIR shadow bias for adopted cache samples).

### 4B. Seed (in `reuse_spatial.wgsl`, at the final shaded write)

Scatter the post-spatial shaded reservoir `sel` into its cell:

```
if (RF_WORLDGI && sel.kind != SK_NONE) worldGi[cell(x1, n1)] = packReservoir(sel);
```

- Last-writer-wins across the many pixels that map to one cell. A storage race
  that picks one valid reservoir is an acceptable (lossy) cache update: every
  candidate written is itself an unbiased `(sample, W)` pair, so the survivor is
  too. A proper atomic reservoir stream-combine is a later refinement (PLAN
  §4C), not needed for a correct prototype.
- **Only non-null `sel` is written.** A pixel that produced no sample this frame
  must not erase a good cached sample — that is what gives the cache its
  cross-frame persistence through disocclusion. The two early-return null paths
  in the spatial pass deliberately do **not** seed.

## 5. Bias and correlation

Unbiasedness of each merge follows from GRIS with an identity shift and
confidence-weighted MIS, identical to the already-validated fresh+spatial path.
Like temporal reuse, chaining the cache across frames adds *correlation*
(variance), and — as STATUS records for temporal+spatial — this implementation
shows a small practical energy drift from history accounting. Therefore:

- `worldgi` is **not** in any default preset; it is an opt-in real-time axis.
- The permanent unbiased correctness gate keeps `worldgi=0` (as it keeps
  `treuse=0`).
- Any promotion must report the biased/unbiased split separately (PLAN §5
  fairness rules), and the prototype's first job is the §7 measurements, not a
  quality claim.

## 6. Invalidation (defined now, inert for claim scenes)

Claim scenes are static, so the prototype does no active invalidation. The
forward-looking rule (PLAN §4A.4): tag each brick with a version; a voxel edit
bumps the brick version, and a cell whose stored version ≠ current brick version
is treated as empty on read. This keeps a future dynamic path from silently
reusing stale GI. Not implemented in v0 — recorded so it is not forgotten.

## 7. Results (measured 2026-07-18, RTX 3080, 1920×1080)

Harnesses: `test/worldgi-check.mjs` (inertness/smoke) and
`test/worldgi-return.mjs` (camera return); FLIP at frame 96 under motion via
`bench.mjs --suite convergence --frame-list 96` against the frozen `_at96f`
references. Control is `ours` (identical non-cache work). HDR-FLIP, lower better.

1. **Inert when empty — PASS.** Single-frame `?worldgi=1` is byte-identical to
   `?worldgi=0` (empty cells skip the merge, consume no rand()); the renderer is
   deterministic (worldgi=0 twice → byte-identical). Regression guard.

2. **Design 1 — cache applied everywhere (query in pathtrace), uncapped.**
   **Catastrophic.** FLIP: interior_move 0.115→0.590, exterior_move 0.121→0.485,
   lamps_move 0.167→0.668 (~4–5× worse). Two compounding faults: (a) runaway
   confidence — the query→seed→query loop grows a cell's confidence every frame
   until it dominates and every receiver in the cell collapses onto one sample;
   (b) one reservoir per world cell is read identically by all pixels in it,
   giving piecewise-constant, **brick-blocky** GI (visible as ~0.5 m tan/grey
   blotches on interior walls). GPU time actually *drops* ~15% because the
   reservoirs go degenerate.

3. **Design 2 — confidence cap + plane-exact key (8× cells, 50 MB).** Capping
   the cache confidence (`?wgicap=`) recovers about half (exterior_move
   0.485→0.238 at cap1), confirming the runaway. Adding the sub-brick voxel-plane
   key barely helps (0.257→0.238) because the blockiness is *tangential*, not
   along the normal. Still ~2× worse than control. Coarse-cell aggregation is
   fundamental to a per-cell reservoir.

4. **Design 3 — disocclusion-gated (query moved to `reuse_temporal`'s `!found`
   branch).** Removes the catastrophe: exterior_move ties the control
   (0.1210 vs 0.1208 at cap1; +2.4% uncapped). But **neutral, not a win** — at
   frame 96 disocclusion is a thin fraction of the image, so fixing it does not
   move whole-image FLIP.

5. **Camera-return probe (the thesis's strongest case).** Warm 60 frames at pose
   B, look away (yaw+π) 60 frames, return to B, score vs the interior_static
   reference at +2/4/8/16 frames. FLIP delta **−0.3%…+0.2%** (noise); relRMSE
   ~1–2% better with the cache. FLIP is already flat ~0.104 from +2 frames after
   return — screen reuse recovers the re-exposed room in ~2 frames, so there is
   no persistence gap for the cache to exploit.

Conclusion: the cache is implemented correctly (it contributes, relRMSE
improves slightly) but the persistence hypothesis does not hold here. See the
verdict at the top of this document.

## 8. File map

- `src/shaders/common.wgsl` — `RF_WORLDGI` flag bit.
- `src/shaders/restir.wgsl` — `faceBucket`, `worldCellIndex` helpers.
- `src/shaders/pathtrace.wgsl` — binding 15 `worldGi` (read); query merge.
- `src/shaders/reuse_spatial.wgsl` — binding 15 `worldGi` (read_write); seed.
- `src/main.js` — `RF.worldgi`, persistent buffer, bind-group entries.
