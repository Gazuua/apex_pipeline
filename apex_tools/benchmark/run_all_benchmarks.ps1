# Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.
# NUMA+Affinity Benchmark Suite — One-click runner
# Usage: powershell -ExecutionPolicy Bypass -File run_all_benchmarks.ps1

param(
    [string]$Version = "v0.6.5.0",
    [string]$Hardware = "i7-14700-20C28T"
)

$ErrorActionPreference = "Stop"

# --- Paths ---
$Root = (git rev-parse --show-toplevel).Trim()
$Agent = "$env:LOCALAPPDATA\apex-agent\apex-agent.exe"
$BinDir = "$Root\apex_core\bin\release"
$OutDir = "$Root\docs\apex_core\benchmark\$Version\$Hardware\data"

if (-not (Test-Path $Agent)) {
    Write-Host "[ERROR] apex-agent.exe not found at $Agent" -ForegroundColor Red
    exit 1
}

# --- Release Build ---
Write-Host "`n=== Release Build ===" -ForegroundColor Cyan
& $Agent queue build release
if ($LASTEXITCODE -ne 0) { Write-Host "[ERROR] Build failed" -ForegroundColor Red; exit 1 }

# --- Output Directory ---
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
Write-Host "`nOutput: $OutDir`n" -ForegroundColor Green

# --- Benchmark Lists ---
$Micro = @(
    "mpsc_queue", "spsc_queue", "allocators", "ring_buffer",
    "frame_codec", "dispatcher", "timing_wheel", "serialization",
    "session_lifecycle"
)

$IntegrationOff = @(
    "architecture_comparison", "cross_core_latency",
    "cross_core_message_passing", "frame_pipeline", "session_throughput"
)

$IntegrationOn = @(
    "architecture_comparison", "cross_core_latency",
    "cross_core_message_passing"
)

$Total = $Micro.Count + $IntegrationOff.Count + $IntegrationOn.Count
$Current = 0

function Run-Benchmark {
    param([string]$Name, [string]$OutFile, [string[]]$ExtraArgs)
    $script:Current++
    $Exe = "$BinDir\bench_$Name.exe"
    $Args = @("queue", "benchmark", $Exe) + $ExtraArgs + @(
        "--benchmark_format=json",
        "--benchmark_out=$OutFile"
    )
    Write-Host "[$script:Current/$Total] $Name $(if ($ExtraArgs -contains '--affinity=on') {'(affinity ON)'} else {''})" -ForegroundColor Yellow
    & $Agent @Args
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  [WARN] $Name failed (exit $LASTEXITCODE)" -ForegroundColor Red
    } else {
        Write-Host "  [OK]" -ForegroundColor Green
    }
}

# --- Micro Benchmarks ---
Write-Host "=== Micro Benchmarks (${($Micro.Count)}) ===" -ForegroundColor Cyan
foreach ($name in $Micro) {
    Run-Benchmark -Name $name -OutFile "$OutDir\$name.json"
}

# --- Integration OFF ---
Write-Host "`n=== Integration Benchmarks - Affinity OFF (${($IntegrationOff.Count)}) ===" -ForegroundColor Cyan
foreach ($name in $IntegrationOff) {
    Run-Benchmark -Name $name -OutFile "$OutDir\$name.json"
}

# --- Integration ON ---
Write-Host "`n=== Integration Benchmarks - Affinity ON (${($IntegrationOn.Count)}) ===" -ForegroundColor Cyan
foreach ($name in $IntegrationOn) {
    Run-Benchmark -Name $name -OutFile "$OutDir\${name}_affinity.json" -ExtraArgs @("--affinity=on")
}

# --- Summary ---
$Files = Get-ChildItem -Path $OutDir -Filter "*.json" | Measure-Object
Write-Host "`n=== Complete ===" -ForegroundColor Green
Write-Host "Generated $($Files.Count) files in $OutDir"
Write-Host "Next: commit and push this data folder."
