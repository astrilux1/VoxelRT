# State of the art: real-time GI for destructible voxel worlds

Scope: techniques relevant to a voxel engine with fully destructible/buildable
geometry, 0.0625 m (6.25 cm) voxels, and a low-end GPU target (GTX
1050/1060-class, no hardware ray tracing, no tensor cores). One paragraph per
technique plus explicit strengths/weaknesses for the voxel-destructible use
case. Sources cited inline as URLs.

## Teardown (Dennis Gustafsson, Tuxedo Labs)

Teardown's compute-shader renderer voxelizes each object into its own
brick/octree volume and ray-marches it for ambient occlusion, soft shadows,
and specular (reflection) occlusion, without RT hardware (blog.voxagon.se; a
January 2025 Software Engineering Daily interview covers the current
architecture, softwareengineeringdaily.com). **It does not compute true bounce
GI** — no indirect diffuse transport, no color bleeding, no multi-bounce
energy from emissive surfaces.
- *Strengths:* proven on entry-level GPUs with fully destructible per-object
  voxel volumes; per-object structures rebuild cheaply on edits; AO/shadow
  march is recomputed/lightly filtered every frame, no convergence problem.
- *Weaknesses:* no GI at all — emissive-lit rooms look flat, destruction that
  opens new light paths produces no new *indirect* response. This is the bar
  to beat at comparable cost, not match.

## DDGI / RTXGI (Majercik et al. 2019)

A world-space probe grid storing octahedral irradiance plus mean/mean² hit
distance, with a Chebyshev visibility test to suppress leaking, probe-to-probe
feedback for multi-bounce, and hysteresis-blended updates
(morgan3d.github.io/articles/2019-04-01-ddgi/; production implementation at
github.com/NVIDIA-RTX RTXGI).
- *Strengths:* resolution-independent, stable, shippable; multi-bounce via
  probe feedback is cheap relative to path tracing; doesn't strictly require
  RT hardware.
- *Weaknesses:* probes float in space, decoupled from geometry — leaking and
  over-blur near thin walls are structural, and at 6.25 cm scale "thin wall"
  = one voxel. Probe placement/density is a tuning problem that interacts
  badly with arbitrary destruction. Hysteresis that gives temporal stability
  also means **slow re-convergence after edits** — the exact case this
  project targets. Octahedral per-probe maps cost more bandwidth than a
  single scalar per surface.

## Radiance Cascades

Sannikov's technique (Path of Exile 2) restructures the radiance field into
cascades trading spatial resolution for angular resolution with distance,
giving near-noise-free GI at bounded cost (jason.today/rc explainer).
"Holographic Radiance Cascades" (arXiv:2505.02041, May 2025) reports 1.85 ms
at 512² and 7.67 ms at 1024² on an RTX 3080 laptop — **2D screen-space only**.
- *Strengths:* spend angular resolution where cheap, spatial resolution where
  needed — conceptually a good fit for sparse voxel worlds; noise-free output,
  no denoiser.
- *Weaknesses:* published high-performance results are 2D; a true 3D
  world-space cascade hierarchy at 6.25 cm scale needs a much larger radiance
  volume, and memory/bandwidth grows fast with cascade count — likely
  prohibitive on a 2-3 GB GTX 1050/1060 without aggressive sparsity. Ringing
  artifacts near small bright lights remain unresolved and would be visible
  against hard-edged voxel geometry.

## SHaRC (NVIDIA RTXGI 2.0)

A world-space spatial-hash radiance cache (github.com/NVIDIA-RTX/SHARC):
radiance samples are hashed by world position/normal at multiple voxel sizes,
an update pass traces ~4% of screen pixels to refresh entries, and primary
rays query the cache to terminate paths early. Plain compute, vendor-agnostic.
- *Strengths:* "world position" is naturally voxel-quantized already — no
  hashing precision issues; cache update decoupled from primary visibility,
  roughly constant cost; early termination is a big ray-count win on low-end
  GPUs; runs on a GTX 1050/1060.
- *Weaknesses:* generic spatial hashing over continuous positions still needs
  collision handling, aging/eviction, and level-of-detail selection — overhead
  a voxel engine doesn't need if it indexes directly by voxel/face coordinates.
  Edits need hashed-cell invalidation, an extra bookkeeping layer versus a
  structure already brick-aligned with geometry.

## Neural Radiance Caching (Müller et al. 2021)

An online-trained MLP predicts outgoing radiance at shading points, replacing
a path-traced tail with one network evaluation — the neural analogue of
SHaRC's hash cache.
- *Strengths:* continuous learned representation generalizes across similar
  shading points; online training adapts to scene changes without explicit
  invalidation.
- *Weaknesses:* **not low-end-friendly** — frequent small-batch MLP inference
  and online SGD assume tensor-core throughput (RTX 20-series+). On a GTX
  1050/1060 with no tensor cores, inference alone would likely exceed the
  frame's entire ray budget. The opaque learned cache also makes O(1) edit
  invalidation hard — no per-voxel-face semantics to clear.

## ReSTIR GI (Ouyang et al. 2021)

Reservoir-based spatiotemporal resampling applied to indirect illumination:
each pixel keeps a reservoir of candidate paths, spatial/temporal resampling
shares good samples between neighbors and frames, sharply reducing required
samples per pixel.
- *Strengths:* very high quality per traced ray; resampling math is general,
  could run over a voxel DDA's hits in principle.
- *Weaknesses:* assumes a per-pixel "trace one path, resample many candidates"
  workload — i.e. RT-core-class throughput feeding the reservoirs. Without
  that, base noise is too high and reuse can amplify rather than hide it
  (boiling/ghosting). Spatiotemporal reuse buffers (motion vectors, history)
  break down after a destruction event invalidates large screen regions.

## Lumen (UE5)

Combines a mesh-card-based surface cache (per-object radiance atlases) with
screen-space probes traced against screen-space, mesh SDF, and global SDF
representations — the most complete shipped "infinite bounce, fully dynamic"
GI in a mainstream engine.
- *Strengths:* proves surface-anchored cache + cheap tracing against a
  distance-field proxy gives convincing infinite-bounce GI; screen probes fill
  per-pixel detail atop a coarser cache.
- *Weaknesses:* heavyweight — mesh cards, global SDF, and screen probes
  together assume VRAM/compute well above a GTX 1050/1060. More fundamentally,
  Lumen's cache is built around *meshes* (cards baked per static/movable
  mesh); a destructible voxel world has no stable mesh identity — every edit
  would mean re-baking cards, the wrong granularity (object-level, not
  6.25 cm voxel-face-level) for this project.

## Voxel cone tracing / VXGI

Voxelizes the scene into a sparse mipmapped volume of filtered
radiance/opacity, then computes indirect lighting by marching cones that
widen with distance and sample coarser mips as they widen.
- *Strengths:* directly voxel-based; cone marching against mips gives a
  cascade-style cheap-far-field property; well-understood, widely
  implemented.
- *Weaknesses:* the prefiltered-mip representation **leaks light through thin
  walls at coarse mips** — a 6.25 cm wall is sub-texel beyond the first couple
  of mip levels, so cones sample averaged opacity/radiance from both sides.
  Same root cause as DDGI's leaking, manifesting in the mip pyramid instead of
  a probe grid. Edits require re-voxelizing and re-filtering the affected mip
  chain, often touching a region much larger than the edit itself — not the
  O(1) response this project targets.

## Academic 2025 work

Ott, "Real-Time Global Illumination for Voxel Worlds" (TU Wien master's
thesis, cg.tuwien.ac.at) combines path tracing with voxel-adapted spatial
resampling, a light denoiser, and a custom TAA tuned for voxel edges —
evidence that "path-trace + resample + denoise + custom TAA" works for voxel
scenes, but it doesn't claim O(1) edit response or a surface-anchored cache;
resampling/denoising re-converges after edits much like ReSTIR/DDGI. Cosin
Ayerbe & Patow, "Dynamic Voxel-Based Global Illumination" (CGF 2025) splits
the scene into static/dynamic voxel sets ray-traced at different cadences,
focused on single-bounce GI for rigid dynamic objects — useful for the
static/dynamic split idea, but single-bounce and rigid-body motion are both
narrower than free-form, multi-bounce, arbitrary destruction.

## Brickmaps and sparse 64-trees (acceleration structures)

Independent of the GI algorithm, the traversal structure matters for edit
cost. Brickmaps (coarse grid of pointers to dense 8³ bricks) and sparse
64-trees / 64-bit occupancy masks traversed via HDDA
(dubiousconst282.github.io/2024/10/03/voxel-ray-tracing; NVIDIA NanoVDB) let
traversal use popcount/ctz to skip empty space in O(1) per level.
- *Strengths:* editing a voxel flips bits in one or two small masks and
  possibly allocates/frees one brick — genuinely **O(1) per edit**,
  independent of world size; cache-friendly; serves both primary visibility
  and any GI ray queries.
- *Weaknesses:* solves *traversal*, not *lighting* — says nothing about where
  irradiance is cached or how it responds to the edits it makes cheap. A GI
  technique on top still needs its own invalidation/reconvergence answer, and
  gains nothing if it requires a full-volume re-bake (e.g. VXGI's mip
  re-filter) despite O(1) occupancy edits.

## Gap analysis

No surveyed technique satisfies all four properties this project needs at
once:

1. **Surface-anchored caching (no leaks).** DDGI and VXGI cache/filter
   radiance in *space* (probes, voxel mips) decoupled from surfaces — the
   structural cause of their leaking. SHaRC and Lumen's surface cache get
   closer (hashed positions / mesh cards) but neither maps cleanly onto a
   6.25 cm voxel face as the cache key.
2. **Cheap infinite-bounce.** Only cache-feedback approaches (SHaRC's
   early-termination cache, Lumen's surface cache feedback, neural radiance
   caching's learned fixed point) get effectively-infinite bounces without
   paying per frame. DDGI gets multi-bounce too, but only via slow
   probe-to-probe hysteresis.
3. **O(1) edit response.** Brickmaps/sparse-64-trees solve this for
   *occupancy*, but nothing surveyed solves it for *lighting state* at the
   same granularity — DDGI re-converges via hysteresis over many frames, VXGI
   needs mip re-filtering, Lumen needs card re-baking, SHaRC needs hash-cell
   invalidation coarser than one voxel face.
4. **Per-pixel detail at low-end cost.** Path tracing and ReSTIR GI deliver
   per-pixel detail but assume RT-core/throughput budgets unavailable on a
   GTX 1050/1060. Probe- and cache-based methods are cheap but blur detail
   between samples.

**The structural opportunity:** in a voxel world every exposed surface normal
is one of exactly six axis-aligned directions. The "directional" part of a
surface's irradiance — what probe-based and SH-based methods spend most of
their storage/bandwidth encoding — is degenerate. Diffuse irradiance at a
voxel face depends only on its position and which of the 6 axes it faces, so
the entire indirect solution collapses to **one scalar (or RGB) value per
exposed voxel face**, indexed by the same brick/voxel coordinates the geometry
already uses. This gives simultaneously: a cache that lives *on* surfaces (no
leaking by construction), a natural feedback path for cheap multi-bounce (a
ray hitting a face reads that face's cached value), O(1) invalidation (clear
sample counts of faces in the edited brick — the same bricks the occupancy
edit already touches), and per-face (6.25 cm) detail finer than any probe grid
or hash cell practical at this cost. This is the gap FaceCache-GI targets.

## Summary table

| Technique | Cost class (low-end GPU) | Quality | Edit response | Leak risk |
|---|---|---|---|---|
| Teardown AO/shadow march (blog.voxagon.se) | Low | No bounce GI (N/A) | Immediate (per-object revoxelize) | None (no GI to leak) |
| DDGI / RTXGI (morgan3d.github.io/articles/2019-04-01-ddgi/) | Medium | Medium (blur near geo) | Slow (hysteresis, many frames) | Medium-high near thin walls |
| Radiance Cascades, 2D (jason.today/rc, arXiv:2505.02041) | Low (2D only) | High, noise-free | N/A (2D demonstrated only) | Low in 2D; ringing near small lights |
| Radiance Cascades, 3D (extrapolated) | High (memory/bandwidth) | Potentially high | Unproven | Unknown / ringing risk |
| SHaRC (github.com/NVIDIA-RTX/SHARC) | Medium | Medium-high | Medium (hash-cell invalidation) | Low |
| Neural Radiance Caching (Müller et al. 2021) | Very high (needs tensor cores) | High | Adapts online but opaque | Low |
| ReSTIR GI (Ouyang et al. 2021) | High (needs RT-core throughput) | Very high | Poor after large screen changes | Low |
| Lumen surface cache + screen probes (UE5) | High | High | Slow (card re-bake per object) | Low-medium |
| Voxel cone tracing / VXGI | Medium-high | Medium (mip blur) | Poor (mip re-filter on edit) | High at coarse mips / thin walls |
| Brickmap / sparse-64-tree traversal (dubiousconst282.github.io, NanoVDB) | Low (traversal only) | N/A (acceleration structure) | O(1) (bit-mask flip) | N/A |
| FaceCache-GI (this project, per-face scalar cache) | Low (target) | Target: high, per-face detail | Target: O(1) (brick-aligned invalidation) | Target: none (surface-anchored) |
