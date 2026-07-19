# Hardware-counter profiling workflow (claim-v2 core instrument)

## Native path — INSTALLED AND OPERATIONAL 2026-07-19

The platform decision (native wgpu, Vulkan backend) obsoletes the browser
workarounds below for v2 work: Nsight hooks `voxelrt-native.exe` directly.
Setup that is live on the benchmark machine:

- **Nsight Systems 2026.3.1** and **Nsight Graphics 2026.2** downloaded
  from NVIDIA (no login gate on the direct MSI URLs) and **administratively
  extracted** (`msiexec /a`, no elevation) to `C:\Users\saege\tools\` —
  `nsys.exe` under `nsight-systems\...\target-windows-x64\`, `ngfx.exe`
  under `nsight-graphics\...\host\windows-desktop-nomad-x64\`.
- **Microsoft PIX** installed via winget (D3D12 only — relevant to the
  browser/v1 path or a future `WGPU_BACKEND=dx12` run, not the Vulkan
  native path).
- Nsight Graphics **Vulkan layers registered per-user** (HKCU
  `SOFTWARE\Khronos\Vulkan\ImplicitLayers` → the GPU_Trace and nomad layer
  JSONs in the extract's `target\windows-desktop-nomad-x64\`) — required
  because the administrative extract skips the MSI's HKLM registration.
- Headless capture solved: the native bench has no swapchain, so GPU Trace
  frame triggers never fire — use **`--limit-to-submits`** (submit-based
  boundaries). Verified: ngfx launches the app, layer engages, session
  establishes, submits are detected.
- Counter access: RESOLVED 2026-07-19 — GPU performance counters enabled
  for all users (NVIDIA Control Panel developer toggle + reboot), so
  captures run unelevated. Previously NVIDIA's `RmProfilingAdminOnly`
  default blocked the session at "GPU Performance Counters unavailable".
- Operational note: run ngfx captures **one at a time** — back-to-back
  scripted launches hit session contention ("Failed to connect"); single
  sequential invocations are reliable.
- First assignment complete (docs/S64.md §8.2): the S64 stack-vs-stackless
  occupancy mechanism, measured and corrected — L1-shared carveout for the
  dynamically-indexed stack, not register-file pressure. Raw exports in
  `test/eval/gputrace/`.

Unelevated `nsys` caveats on this machine: WDDM trace, CPU sampling, and
GPU-metrics sampling are disabled without admin; timeline + Vulkan API
trace still work once its layer question is revisited. Elevated runs get
the full set.

---

## Browser path (v1 record / demo) — research below, unchanged

Research round 2026-07-18 (sources at bottom). Goal: paper-grade mechanistic
evidence — SM warp occupancy, active threads/warp, warp latency, stall
reasons, memory throughput — per compute pass, like Lin 2026 §7.1's NSight
numbers. Chrome timestamp queries give per-pass ms; this is the layer below.

**Process rule (from RESEARCH_LOOP discipline): profiled runs never double as
timing runs.** Profiler overhead and relaxed sandboxing contaminate ms/frame;
counters come from dedicated captures, timings from the untouched bench path.

## The central fact

**Nsight Graphics does NOT support Chrome WebGPU** — NVIDIA has confirmed
non-support repeatedly through late 2025. Chrome runs Dawn on D3D12 but does
not present via D3D12, so frame-oriented profilers see no frame boundary and
fail to hook. Everything below is the verified state of the art around that.

## Recommended three-layer stack

| Layer | Tool | Gives | Scriptable |
|---|---|---|---|
| 1. Always-on | **Nsight Systems (`nsys` CLI)** | D3D12 API + GPU workload timeline of Chrome's GPU process, pass ranges named by our debug groups, **plus sampled SM-occupancy / DRAM-BW rows** (`--gpu-metrics-devices`) — time-resolved occupancy with zero hacks | Fully (CLI → SQLite/`nsys stats`) |
| 2. Per-dispatch counters | **PIX GPU Captures + NVIDIA counter plugin** (Ampere supported) | Per-dispatch NVIDIA HW counters: instruction issue, warp allocation, memory throughput; collected during capture *replay*, reproducible offline | Fully (`pixtool launch … --captureFromStart`; `pixtool … save-event-list events.csv --counter-groups=…`) |
| 3. Deepest (best-effort) | **Nsight Graphics GPU Trace via `d3d12_webgpu_shim`** (community Detours hook, v2.1 May 2026, actively maintained) | Full paper-grade view: SM occupancy timeline, warp stall reasons, ~10 µs shader profiler on Ampere | GUI proven; `ngfx --auto-export` TSV path exists but Chrome+shim+CLI combo is unverified |

Fallback if the shim breaks: **dawn.node / node-webgpu native replica** —
same Dawn+Tint+D3D12 stack as Chrome, hookable by Nsight with no hacks.
Per-kernel counter claims transfer (identical compiled shaders + dispatch
dims); end-to-end frame scheduling claims do not. Any native-harness numbers
must ship with a Chrome timestamp-query cross-validation table bounding
drift. RenderDoc (Chrome 144+ `enable_renderdoc_process_injection`) is a
debugging complement only — no NVIDIA counters.

## Common Chrome flag block (all tools; Playwright `args`)

```
--enable-unsafe-webgpu
--enable-webgpu-developer-features        # unquantized timestamp queries
--disable-gpu-sandbox --no-sandbox --disable-gpu-watchdog
--disable-direct-composition
--disable-features=RendererCodeIntegrity
--do-not-de-elevate                       # Chrome 142+: mandatory under elevated profilers
--enable-dawn-features=use_user_defined_labels_in_backend,emit_hlsl_debug_symbols,disable_symbol_renaming
```

Add `--gpu-startup-dialog` only for manual attach/inject. Playwright must not
inject conflicting defaults (`ignoreDefaultArgs` selectively) and the browser
build should be pinned; Chrome 138+ process-respawn and 142+ auto-de-elevation
have each broken these workflows once.

**Code prerequisite:** wrap every compute pass in
`pushDebugGroup('<passName>')`/`popDebugGroup` in `src/main.js` — Dawn
forwards them via the PIX Event Runtime on D3D12, and they become the named
ranges in nsys, PIX, and Nsight alike (requires `WinPixEventRuntime.dll`
loadable by the GPU process; if markers don't appear, route through the shim
launcher). Not yet implemented — deliberately deferred while the v1 baseline
campaign is mid-flight, since the campaign reloads `src/` from disk per run.

## Recipes

Layer 1, every profiling session:

```
nsys profile --trace=dx12-annotations,wddm --dx12-gpu-workload=individual ^
  --gpu-metrics-devices=all --duration=30 -o run.nsys-rep ^
  chrome.exe <flag block> http://localhost:PORT/?<bench query>
nsys export --type sqlite run.nsys-rep && nsys stats run.nsys-rep
```

Layer 2, per-config deep run: `pixtool launch` Chrome with the flag block,
capture 2–4 frames at steady state, then
`pixtool open-capture bench.pix3 save-event-list events.csv
--counter-groups=<NVIDIA groups>` → per-dispatch counter CSV.

Layer 3, mechanistic figures: Nsight GPU Trace activity launches Chrome with
the flag block → `webgpu_injector.exe chrome.exe` → reload tab → Collect GPU
Trace, Ampere metric set + shader profiler for stall sampling.

## Local-verification checklist (before any counter lands in RESULTS.md)

1. `nsys` GPU-metrics rows populate on Win11 + current driver and align with
   the DX12 workload rows of Chrome's GPU process.
2. `WinPixEventRuntime.dll` loads into the pinned Chrome (markers visible in
   PIX); else use the shim launcher.
3. PIX replay fidelity: replayed per-dispatch timings correlate with Chrome
   timestamp-query per-pass ms before any counter is cited.
4. ngfx CLI + shim + Chrome end-to-end (nobody has published this working;
   GUI path is the proven one).
5. Tool + browser versions recorded in the runtime context of every capture,
   like the bench already does for adapter/driver.

## Sources

frguthmann.github.io/posts/profiling_webgpu (Sep 2024, upd. May 2026) +
github.com/frguthmann/d3d12_webgpu_shim (v2.1) · toji.dev/webgpu-profiling
(PIX Jan 2024; RenderDoc upd. Nov 2025) · NVIDIA forums 277028 / 296805 /
349288 (official non-support; working flag sets, Oct–Nov 2025) ·
devblogs.microsoft.com/pix (hardware-counters, gpu-captures, pixtool) ·
docs.nvidia.com/nsight-systems + nsight-graphics (GPU Trace, --auto-export) ·
github.com/FrostyLeaves/nsight-graphics-analyzer (2026 ngfx automation) ·
dawn.googlesource.com docs (debugging.md, debug_markers.md) ·
developer.chrome.com/blog/new-in-webgpu-115 (--use-webgpu-adapter) ·
github.com/dawn-gpu/node-webgpu
