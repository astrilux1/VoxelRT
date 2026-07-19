# Claim-v2 test scenes: research grounding and specs

Research round 2026-07-18 (paper text + ReSTIR ecosystem sources, cited at
bottom). These are **proposals**: each scene enters the framework with its
stage's pre-registration (PLAN §3 staged rollout), and its acceptance signals
below become that pre-registration's machine-checkable criteria. Nothing here
is frozen until the stage's experiment doc locks it.

Paper parameter facts the scenes are designed around (all from Lin 2026):
single-vertex roughness threshold **σ_min = 0.2**; footprint constant
**k = 0.02**; baseline distance threshold = **2% of shortest scene dimension**
(the convention the voxel scenes deliberately break); cCap 20, 3 neighbors,
30-px radius (Gaussian σ = 16); duplication 17×17, score = count/288,
C_min = 1, γ = 0.1; 1 spp initial path tree; Fig. 4 sanity roughnesses 0.3
and 0.7.

## What each paper scene exercises (regime table)

| Scene | Figure(s) | Regime | Why chosen |
|---|---|---|---|
| Spaceship | Fig. 1 | Glossy ground + glass canopy; correlation streaks; color noise | Aggregate showcase; "surfaces inside the glass" test reconnection through dielectrics |
| Kitchen | Fig. 5, 10, 15 | Glossy floor under camera slide → dense correlation; near-vs-far of same glossy surface | Worst-case correlation (3.25% decorrelation bias, concentrated on glossy pixels); fixed thresholds tuned near fail far |
| San Miguel | Fig. 11 | Near-delta distant highlight → caustic via replay only | Published failure case; footprint "adjusts roughness by distance" |
| Veach Ajar | Fig. 12, T1 | Slit-lit indirect; diffuse control ties, metallic r=0.3 walls discriminate | r=0.3 sits just above σ_min=0.2 — straddles the reconnection decision |
| Opera House | Fig. 8, T1 | Glossy interior; reuse-kernel ablation; warp profiling | Neighbor compatibility matters; σ = 8ρ/(9π) equivalence |
| Watercolor | Fig. 13-15 | Glossy + upward motion → disocclusion trails | Moderate-correlation convergence control vs Kitchen |
| Zero Day | Fig. 13 | Many animated saturated emitters, glossy floors, dark env | Many-light NEE + §6.3 chroma noise; highest FLIP in paper |
| Crown | Fig. 13 | Multi-bounce glossy metal chains | Reconnection nearly never valid under old rules; biggest footprint win |
| Tower Bridge | Fig. 9 | Poor initial sampling → correlation blobs | Decorrelation-vs-boiling-filter arena |
| Carousel / Bathroom | T1 | Animated geometry / mirror interior | Cost-averaging set |

Ecosystem findings: the ReSTIR PT repo ships exactly one scene — animated
Veach Ajar — as its canonical test; its offline ground-truth convention is
temporal reuse **off**, 32 candidates, 3 spatial rounds × 6 neighbors r=10
(VoxelRT's reference mode already matches the temporal-off principle). RTXDI
docs name the canonical shift failures: glossy reconnection vertices, and
**cross-material neighbor pairs inside the spatial kernel**. Teardown-class
voxel engines do jittered single-ray reflections + separate denoise, no
resampled specular transport — these scenes are first-of-kind for voxel PT.

## Recommended scenes, rollout order

### 1. `garage` — Stage 1 (GGX conductors): threshold-straddle room
Fig. 12 analog. Veach-Ajar topology: 6×3×4 m room + antechamber behind a
0.25×2.0 m door-ajar slit; single hot 0.5 m panel (e=255) in the antechamber
so the room is slit-lit and indirect-dominant. ±z walls conductor r=0.3
metal=1 (the Fig. 12 config); +x wall diffuse white (control — criteria must
tie there); floor = six 1 m conductor strips at r = 0.05/0.12/0.18/0.22/0.35/
0.70 (three each side of σ_min, 0.7 = Fig. 4 safe anchor). Shortest dimension
3 m → old distance rule = 6 cm ≈ 1 voxel: every corner interaction sits at
the singular end. `_static`: (5.2, 1.6, 2.0) facing the slit with all strips
in frame. `_move`: 20 s lateral strafe z 0.7→3.3 m — highlight crosses
roughness boundaries between neighbors inside the 30-px kernel.
**Correct:** noise varies smoothly across strips; r=0.3 walls converge like
the diffuse wall; per-strip means match reference. **Failures:** sub-0.2
strips darker (energy loss); blotch noise only on 0.3 walls
(impoverishment); strafe-aligned streaks (correlation); flickering highlight
islands on 0.05/0.12 (boiling).

### 2. `gallery` — Stage 1: viewing-distance / footprint scaling
Fig. 10 analog. 14×3×3 m corridor; floor conductor r=0.25; four 1×1 m wall
panels r=0.10 at 3 m spacing (same highlight geometry at four distances in
one frame); diffuse pillar at 10 m for a disocclusion event; ceiling panel
every 3.5 m (e=210) + far-wall hot lamp (e=255) running a distant highlight
down the floor. `_static`: (1.0, 1.6, 1.5) looking down the corridor.
`_move`: 20 s dolly x 1.0→12.5 m and back (footprint on far panels shrinks
~10×; panels transition far/safe ↔ near/unsafe). **Correct:** per-panel
FLIP/variance ≈ equal, no quality cliff at any dolly position. **Failure:**
far panels noisier/smeared while near ones clean, flipping as the camera
moves (Fig. 10b vs 10f).

### 3. `lamps_glossy` — Stage 1: correlation & many-emitter glossy
Fig. 9 / Fig. 13 analog by upgrading the existing `lamps` scene's receivers
only: checker floor → conductor r=0.3; pillars → r=0.15; shelf underside →
r=0.05. Geometry, 1161 emitters, and both camera paths unchanged — clean A/B
against the pre-materials baseline on identical trajectories. **Correct:**
correlation artifacts transient (< cap frames); mean within ~3% of reference
(paper's own decorrelation bias anchor: 3.25% in a harder scene); chroma
variance comparable across the four saturated panels. **Failures:**
blobs/streaks on glossy floor beneath hot lamps; darkening bias > several %;
persistent color speckle near panels.

### 4. `beacon` — Stage 2 (mirrors): near-delta highlight
Fig. 11 analog. 10×4×10 m hall; 2.0×1.5 m mirror panel (r=0.02, metal=1) on
the far wall; 12.5 cm hot lamp (e=255) high on the near wall; diffuse baffle
so a 2×2 m floor patch receives the lamp **only via the mirror**; patch floor
conductor r=0.25 (reconnection legal at the floor, contested at the mirror);
dim ceiling strips (e=45, ~8.5× power ratio) so dropout measures as
darkening, not black frames. `_static`: (2.0, 1.6, 2.0) framing patch +
mirror at grazing angle. `_move`: 20 s 40° orbit at 4 m radius — the lamp's
virtual image sweeps and the patch translates. **Correct:** patch at
reference position/energy, noise decays normally. **Failures:** patch
missing/dim (caustic dropout, the Fig. 11a arrow); boiling disks from rare
replay successes spread by reuse; patch trailing its true position under
motion (stale reservoirs).

### 5. `vitrine` — Stage 3 (glass): slab refraction
Fig. 1 inset-row-2 analog. 8×4×8 m room; (a) glass divider 12.5 cm thick
(IOR 1.5, r=0) with colored boxes + hot lamp behind it; (b) 1 m glass display
case over an emissive block (e=200) whose light reaches the room only through
glass; (c) floor before the divider conductor r=0.2 — dead on the threshold,
reflection and refraction lobes contest the same reconnection. Dim ambient
strip for measurability. `_static`: (1.5, 1.6, 1.5), ~45° incidence.
`_move`: 20 s strafe parallel to the divider — refraction parallax +
Fresnel slide, grazing/TIR positions crossed. **Correct:** behind-glass
objects at reference brightness (flat slab → image translated, analytically
checkable); case light pool at reference energy. **Failures:** dim/black
interior (energy loss inside glass); flickering light pool (transmitted-path
boiling); grazing fireflies; double-image smear (lobes cross-contaminating
reservoirs).

## Coverage the claim honestly cannot make (voxel constraints)

1. **Focusing caustics** (Crown, San Miguel lamp glass): six axis-aligned
   normals → no curvature → no converging-ray singularities. `beacon` covers
   the *flat-specular near-delta* mechanism; claim wording must say
   "flat-specular chains", not "caustics".
2. **Curved/lens refraction**: slabs translate rays; no magnification or
   curved-boundary TIR. `vitrine` covers slab transmission + Fresnel only.
3. **Curvature term of footprint criteria** (§4.2): untestable with quantized
   normals — explicit caveat on any "matches paper robustness" claim.
4. **Sub-voxel/thin geometry** (hair, foliage, wires): minimum feature
   6.25 cm.
5. **Animated geometry** (Carousel, animated Ajar door): camera-only motion
   exercises only half of dual-motion-vector disocclusion; a re-voxelized
   sliding door slab in `garage` could close part of this later.
6. **Perf transfer**: Table 1 speedups depend on mesh warp-divergence;
   brick-map traversal divergence differs — perf comparisons transfer
   directionally, never numerically.
7. **Defocus/AA-domain effects** (Area ReSTIR's bokeh/subpixel): out of
   scope for a pinhole voxel tracer.

## Sources

ReSTIR_PT repo (github.com/DQLin/ReSTIR_PT) · RTXDI RestirPT.md
(github.com/NVIDIA-RTX/RTXDI) · ReSTIR PT Enhanced (dl.acm.org/10.1145/3804494)
· NVIDIA RTR project page · ReSTIR GI (research.nvidia.com) · Area ReSTIR
(graphics.cs.utah.edu; github.com/guiqi134/Area-ReSTIR) · MCMC decorrelation
(dl.acm.org/10.1145/3629166) · RTXDI boiling filter (developer.nvidia.com
blog) · Teardown frame analyses (acko.net; juandiegomontoya.github.io) ·
intro-to-restir.cwyman.org
