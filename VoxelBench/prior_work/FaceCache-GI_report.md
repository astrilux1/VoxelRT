# FaceCache-GI: Real-Time Path-Traced Global Illumination for Destructible Voxel Worlds

**Target:** low-end PCs (no RT hardware) · fully destructible/buildable worlds · 0.0625 m voxels
**Deliverables:** technique design, SOTA survey, and a controlled benchmark against two SOTA-class baselines.

---

## 1. Problem statement

A game engine with 6.25 cm voxels and unrestricted destruction/building needs global illumination that (a) runs on integrated and entry-level GPUs, (b) responds within frames when geometry changes, and (c) doesn't exhibit the noise of brute-force path tracing or the leaking/blur of probe grids. No production system satisfies all three simultaneously, which motivated designing a new technique and validating it empirically.

## 2. Survey of current state of the art

**Teardown (Tuxedo Labs / Dennis Gustafsson).** The benchmark for destructible voxel rendering on weak hardware, but notably it does *not* compute true GI: it ray-traces ambient occlusion, soft shadows, and specular occlusion against per-object voxel volumes with an 8-bit material palette and layered acceleration structures, all in compute shaders without RT hardware (blog.voxagon.se). Indirect diffuse bounce light is approximated, not simulated. This is the quality bar to beat at the same hardware cost.

**DDGI / RTXGI (Majercik et al. 2019, "Dynamic Diffuse Global Illumination with Ray-Traced Irradiance Fields").** A world-space grid of probes storing octahedral irradiance plus mean/mean² ray-hit distance, with a Chebyshev visibility test to suppress leaking, probe-to-probe feedback for multibounce, and perception-based hysteresis. Stable and shipped in commercial engines, but probes float in empty space (residual leaking and over-blur near geometry), placement/density is a tuning problem, and the whole grid must re-converge through temporal hysteresis after world edits.

**Radiance Cascades (Alexander Sannikov, Grinding Gear Games; used in Path of Exile 2).** A hierarchy of probe cascades exchanging spatial for angular resolution with distance, giving noiseless GI at constant cost (github.com/Raikiri/RadianceCascadesPaper; follow-up "Holographic Radiance Cascades", arXiv 2505.02041). Extremely promising, but mature results are 2D/screen-space; full 3D world-space cascades at 6.25 cm voxel scale have large memory/bandwidth costs, and known artifacts (ringing around small lights) remain an active research area.

**ReSTIR GI (Ouyang et al. 2021).** Reservoir-based spatiotemporal resampling delivers excellent quality per ray but is built around per-pixel many-light resampling workloads that assume RT-core-class throughput; it is not a fit for the low-end target.

**Voxel acceleration structures.** Brickmaps (two-level: coarse grid pointing to 8³ bricks) and sparse 64-trees with 64-bit occupancy masks + popcount traversal are today's best structures for *editable* voxel scenes — near-O(1) updates and cache-friendly, low-divergence DDA (dubiousconst282.github.io/2024/10/03/voxel-ray-tracing, NanoVDB's HDDA). Recent academic work (Cosin Ayerbe & Patow 2025, *Dynamic Voxel-Based Global Illumination*, CGF) confirms interest in dynamic voxel GI but covers single-bounce with rigid dynamic objects, not free-form destruction.

**The gap:** nothing combines surface-anchored caching (no probe leak), cheap multibounce, and O(1) response to arbitrary edits at low-end cost. The structure of voxel worlds offers an unexploited invariant that makes this possible.

## 3. The technique: FaceCache-GI (FCGI)

**Core observation.** In a voxel world every surface normal is one of exactly 6 axis-aligned directions. Diffuse irradiance is a function of position and normal only — so instead of probes encoding full directional irradiance (octahedral maps, SH), the entire GI solution collapses to **one RGB irradiance value per exposed voxel face**. This single fact removes the encoding cost, removes probe placement entirely, and anchors the cache *on* surfaces, where leaking is structurally impossible.

The system has five components:

**(1) Per-face irradiance cache.** Each exposed face stores irradiance E and a sample count n in a brick-aligned atlas. Shading is `albedo/π · (E_direct + E_cache)`. On GPU this lives as a sparse brick atlas (only bricks containing surface voxels allocate face storage), the same residency structure as the geometry brickmap, so it inherits the brickmap's O(1) edit behavior.

**(2) Cache-feedback multibounce.** Face-update rays that hit another voxel face *read that face's cached irradiance* to compute reflected radiance, rather than spawning further bounces. Each frame of feedback adds one bounce, so the cache converges to infinite-bounce GI at strictly one-bounce ray cost — the same fixed-point principle as Lumen's surface cache and neural radiance caching, but with a trivially exact lookup because the hit point's face is the cache key. Each face sample uses 4 cosine-weighted hemisphere rays + 4 next-event-estimation shadow rays.

**(3) Two-level cascade (L0 fine + L1 brick).** A coarse L1 cache stores irradiance per face of each 4³-voxel brick (~4 k entries vs ~60 k fine faces in the test scene). L1 updates round-robin over the whole world with ~40 % of the ray budget and converges in a few frames; it serves both as the feedback source early on and as a fallback at shading when a fine face's sample weight is still low. The fine L0 cache spends ~60 % of the budget, prioritized toward camera-visible faces. This is the cascade idea from Radiance Cascades applied to surface caches rather than free-space probes.

**(4) O(1) destruction invalidation.** When voxels are edited, only the bricks within the edit radius (+1 voxel ring) have their face sample counts clamped to n≤2 — a few hundred bytes of writes. Invalidated faces re-converge with a high blend rate while the rest of the world is untouched. Probe grids cannot do this selectively because each probe integrates light from arbitrarily far away.

**(5) Adaptive temporal blending + cache-space filtering.** Blend α = max(1/(n+1), 0.08) gives fast initial convergence and stable steady state; at shading, a 3×3 cache-space Gaussian with bilinear face interpolation removes residual per-face variance (filtering in cache space is far cheaper and more stable than screen-space denoising of per-pixel noise).

## 4. Benchmark methodology

Because the claim "beats existing methods" must be tested, all three methods were implemented in one C++ CPU framework (`bench.cpp`, ~1,200 lines) sharing the same Amanatides–Woo DDA traversal, the same scene, the same converged direct-lighting pass (excluded from all budgets, so PSNR differences isolate the indirect solution), and instrumented ray/step counters.

**Scene.** A 96×64×96-voxel two-room "Cornell bunker": a main room with red and green walls and three occluders, joined through a doorway to a light room with an emissive ceiling panel — so most of the camera's view is lit *only* indirectly. At frame 75 of 150 an ellipsoid hole is blasted through the dividing wall, suddenly flooding new indirect light into the main room. Static 128×80 camera.

**Baselines.** *PT+temporal* — 1-spp NEE path tracing (depth 8, Russian roulette) with temporal accumulation and an edge-aware 5×5 spatial filter; this is the Teardown-class "ray-trace per pixel and filter" approach upgraded to actual GI. *DDGI* — a faithful 12×8×12 probe grid implementation with 64 spherical-Fibonacci directions/probe, Chebyshev mean/variance visibility, probe feedback multibounce, and perception-based hysteresis.

**Protocol.** Each method ran at 4 ray budgets (0.5×–4× of 40,960 rays/frame) for 150 frames. Quality is tone-mapped PSNR against 768-spp path-traced references (depth 24) computed separately for pre- and post-edit states. Reported: converged PSNR before/after the edit, PSNR drop at the edit, frames to recover, and single-core wall time per frame.

## 5. Results

### Quality at equal ray budget (converged PSNR, dB)

| Budget | PT pre/post | DDGI pre/post | **FCGI pre/post** | FCGI ms vs PT ms |
|---|---|---|---|---|
| 0.5× | 25.6 / 26.4 | 23.2 / 27.2 | **27.3 / 27.6** | 8.6 vs 12.0 |
| 1× | 26.8 / 26.7 | 26.9 / 29.5 | **29.9 / 31.1** | 16.6 vs 31.8 |
| 2× | 27.6 / 28.6 | 29.8 / 26.3 | **32.3 / 32.4** | 32.0 vs 49.9 |
| 4× | 28.8 / 28.9 | 30.0 / 28.4 | **31.5 / 31.8** | 62.8 vs 98.3 |

FaceCache-GI achieves the highest quality at **every** budget — +3.1 dB over PT and +3.0 dB over DDGI at 1× — while also being the cheapest in wall time per ray, because face updates are coherent and feedback replaces deep bounce chains. The Pareto chart (`figures/chart_pareto.png`) shows FCGI strictly dominating both baselines.

### Quality at equal wall time (the metric that matters for a frame budget)

FCGI at 2× costs the same wall time as PT at 1× (~32 ms here, single CPU core): **32.3 dB vs 26.8 dB, a +5.5 dB advantage at identical cost**. FCGI at 0.5× (8.6 ms) already exceeds the quality PT reaches at 31.8 ms — roughly **3.7× cheaper for equal quality**. Visually (`figures/comparison.png`): PT is splotchy and dark, DDGI loses the red/green color bleed to probe blur and shows leak-driven washout, while FCGI closely matches the reference including saturated wall bleeding.

### Response to destruction

All methods drop ~6–8 dB the frame the wall explodes (the lighting genuinely changes). At equal wall time FCGI dominates recovery: comparing the ~32 ms pair, FCGI(2×) is back to 25 dB in 1 frame and 28 dB in 12, vs 4 and 27 frames for PT(1×) — and DDGI at its cheap settings takes 50+ frames because hysteresis must wash out every affected probe. FCGI's brick-local invalidation means only the blast region re-converges; pre-edit quality elsewhere is preserved instantly. At equal *ray* budget, PT recovers fastest to its (much lower) quality ceiling, which is expected — every PT sample is fresh — while FCGI recovers at DDGI's rate but to ~1.5–4 dB higher final quality (`figures/chart_response.png`).

### Anomalies, reported honestly

DDGI's post-edit dip at 2× (26.3 dB) is a hysteresis/Chebyshev interaction after the edit on a single seed; treat per-config numbers as ±0.5 dB. FCGI at 4× converges slightly below its own 2× (31.5 vs 32.3 dB) — the current scheduler spends extra budget on more rays per face rather than better coverage; a tuned scheduler should be monotonic. Neither anomaly affects the ordering of the methods.

## 6. Caveats — what this benchmark does and does not prove

This is a **single-core CPU algorithmic benchmark with equal-ray-budget and equal-wall-time framing, using my own implementations of the baselines**. It demonstrates that FCGI's quality-per-ray and edit-response advantages are real and structural (surface anchoring, 6-normal collapse, feedback multibounce, local invalidation). It does **not** prove GPU FPS superiority over production-tuned Teardown or RTXGI: GPU memory bandwidth, divergence, and atlas residency management could shift the constants, and the test scene is one (deliberately adversarial) indoor scenario. A static camera also flatters all temporal methods equally. Claiming definitive SOTA superiority requires a GPU implementation tested across scenes — the numbers here justify building one.

## 7. GPU implementation notes

Store the world as a brickmap (4³ or 8³ bricks, 64-bit occupancy masks, popcount traversal per the sparse-64-tree literature); allocate face-cache pages in the same atlas only for surface bricks (the dense CPU cache here would be ~56 MB; sparse residency cuts this by 10–20× in real scenes). One compute pass updates L1 round-robin, one updates visibility-prioritized L0 faces (prioritization from a G-buffer face-ID histogram), one shades with the 3×3 cache filter. Edits write the brick + clamp face counts in the same upload — no separate GI maintenance pass. Everything is compute-shader-only; no RT cores required, matching the Teardown hardware envelope.

---

**Files:** `bench.cpp` (full benchmark source), `analyze.py`, `results.csv` (per-frame data, all 12 configs), `figures/` (Pareto, response, timing charts; reference renders; method comparison montage).
