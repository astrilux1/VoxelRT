# GPU Trace both S64 traversal variants + brickmap on the native bench.
# Purpose: confirm the S64 promotion's mechanistic claim (docs/S64.md §8.2 —
# variant 1's stack spills to local memory; stackless stays register-resident)
# with hardware counters: SM occupancy, register/local-memory traffic, warp
# stall reasons.
#
# REQUIREMENT: GPU performance counters. Either
#   (a) run this script from an ELEVATED PowerShell (counters are available
#       to admin processes without any driver change), or
#   (b) one-time global toggle: NVIDIA Control Panel -> Developer -> Manage
#       GPU Performance Counters -> "Allow access to ... all users", then
#       reboot — after which this runs unelevated.
#
# Prereqs already in place (see docs/PROFILING.md "Native path" section):
# Nsight Graphics 2026.2 administratively extracted to C:\Users\saege\tools,
# GPU Trace + nomad Vulkan layers registered per-user in HKCU.

$ErrorActionPreference = "Stop"
$ngfx = "C:\Users\saege\tools\nsight-graphics\ProgramFiles64Folder\NVIDIA Corporation\Nsight Graphics 2026.2.0\host\windows-desktop-nomad-x64\ngfx.exe"
$exe  = Join-Path $PSScriptRoot "target\release\voxelrt-native.exe"
$outRoot = Join-Path $PSScriptRoot "..\test\eval\gputrace"

foreach ($run in @(
    @{ name = "s64-stack";     args = "--backend s64 --traversal stack --scene sparse1024 --frames 5000" },
    @{ name = "s64-stackless"; args = "--backend s64 --traversal stackless --scene sparse1024 --frames 5000" },
    @{ name = "brickmap";      args = "--backend brickmap --scene sparse1024 --frames 5000" }
)) {
    $out = Join-Path $outRoot $run.name
    New-Item -ItemType Directory -Force $out | Out-Null
    Write-Host "=== GPU Trace: $($run.name) ==="
    & $ngfx --activity="GPU Trace Profiler" --exe=$exe --dir=$PSScriptRoot `
        --args=$run.args --start-after-ms=3000 --limit-to-submits=8 `
        --auto-export --output-dir=$out 2>&1 | Select-Object -Last 4
}
Write-Host "Exports in $outRoot\<name>\ (TSV metric bundles next to the .ngfx-gputrace)."
Write-Host "Compare per-dispatch: SM occupancy, LSU/local-memory throughput, warp stall reasons for primary/bounce/shadow."
