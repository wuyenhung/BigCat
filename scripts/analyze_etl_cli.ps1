#Requires -Version 5.1
<#
.SYNOPSIS
    CLI ETL trace analysis for EtherCAT cycle-stability diagnosis.
    Uses Windows ADK xperf.exe to extract DPC/ISR statistics from WPR kernel traces.

.DESCRIPTION
    Wraps xperf.exe -a dpcisr to produce a structured summary:
      - DPC/ISR latency distribution
      - Top DPC drivers by CPU time
      - Drivers exceeding latency thresholds (100us default)

.PARAMETER EtlPath
    Path to the .etl file to analyze (required).

.PARAMETER OutDir
    Output directory. Default: same directory as ETL.

.PARAMETER XperfPath
    Path to xperf.exe. Default: auto-detect from ADK.

.PARAMETER ThresholdUs
    Latency threshold in microseconds for flagging. Default: 100.

.EXAMPLE
    .\scripts\analyze_etl_cli.ps1 -EtlPath baseline_results\etw\etw_trace.etl
#>

param(
    [Parameter(Mandatory=$true)]
    [string]$EtlPath,
    [string]$OutDir,
    [string]$XperfPath,
    [int]$ThresholdUs = 100
)

$ErrorActionPreference = "Stop"

# ============================================================================
# Find xperf
# ============================================================================
function Find-XperfExe {
    if ($XperfPath -and (Test-Path $XperfPath)) { return $XperfPath }
    $candidates = @(
        "${env:ProgramFiles(x86)}\Windows Kits\10\Windows Performance Toolkit\xperf.exe",
        "${env:ProgramFiles}\Windows Kits\10\Windows Performance Toolkit\xperf.exe"
    )
    foreach ($p in $candidates) { if (Test-Path $p) { return $p } }
    $fp = Get-Command xperf.exe -ErrorAction SilentlyContinue
    if ($fp) { return $fp.Source }
    return $null
}

# ============================================================================
# Run xperf
# ============================================================================
function Invoke-XperfDpcIsr {
    param([string]$XperfExe, [string]$EtlFile)
    Write-Host ">>> Running xperf -a dpcisr ..." -ForegroundColor Yellow
    $raw = & $XperfExe -i $EtlFile -a dpcisr 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "xperf exited with code $LASTEXITCODE" }
    return $raw
}

# ============================================================================
# Parse
# ============================================================================
function Parse-DpcIsrOutput {
    param([string]$Raw)

    $result = [PSCustomObject]@{
        TraceDurationUs = 0; TotalDpcCount = 0
        TopDrivers = @(); LatencyBuckets = @()
    }
    $lines = $Raw -split "`r`n|`n"

    if ($Raw -match 'CPU Usage from \d+ us to (\d+) us') {
        $result.TraceDurationUs = [long]$Matches[1]
    }
    if ($Raw -match 'Total = (\d+)\s') {
        $result.TotalDpcCount = [int]$Matches[1]
    }

    # --- Parse driver table ---
    $driverList = [System.Collections.ArrayList]::new()
    $inSection = $false; $pastHeader = $false
    foreach ($line in $lines) {
        if ($line -match 'CPU Usage Summing By Module') { $inSection = $true; continue }
        if (-not $inSection) { continue }
        if (-not $pastHeader) { if ($line -match 'Module$') { $pastHeader = $true }; continue }
        if ($line -match '^Total = (\d+)$') { break }
        if ($line -match '^\s+\d+\s+[\d.]+,\s+\d+') {
            $parts = $line -split ','
            if ($parts.Count -ge 2) {
                $module = $parts[-1].Trim()
                $total = 0
                for ($c = 0; $c -lt [Math]::Min(16, $parts.Count - 1); $c++) {
                    $usec = ($parts[$c].Trim() -split '\s+')[0] -as [long]
                    if ($usec) { $total += $usec }
                }
                if ($total -gt 0) {
                    [void]$driverList.Add([PSCustomObject]@{ Module = $module; TotalUs = $total })
                }
            }
        }
    }
    $result.TopDrivers = $driverList | Sort-Object TotalUs -Descending

    # --- Parse latency buckets (global only) ---
    $bucketList = [System.Collections.ArrayList]::new()
    $pat = 'Elapsed Time, >\s+(\d+) usecs AND <=\s+(\d+) usecs,\s+(\d+), or\s+([\d.]+)%'
    foreach ($line in $lines) {
        if ($line -match '^Total = \d+ for module') { break }
        if ($line -match $pat) {
            [void]$bucketList.Add([PSCustomObject]@{
                FromUs = [int]$Matches[1]; ToUs = [int]$Matches[2]
                Count = [int]$Matches[3]; Pct = [double]$Matches[4]
            })
        }
    }
    $result.LatencyBuckets = $bucketList
    return $result
}

# ============================================================================
# Parse per-CPU DPC time
# ============================================================================
function Parse-DpcIsrPerCpu {
    param([string]$Raw)

    $perCpu = [System.Collections.ArrayList]::new()
    $lines = $Raw -split "`r`n|`n"
    $cpuNum = -1; $inSection = $false; $pastHeader = $false; $total = 0

    foreach ($line in $lines) {
        if ($line -match 'CPU Usage Summing By Module for (?:Processor|CPU)\s+(\d+)') {
            if ($cpuNum -ge 0) {
                [void]$perCpu.Add([PSCustomObject]@{ Cpu = $cpuNum; DpcTotalUs = $total })
            }
            $cpuNum = [int]$Matches[1]; $total = 0
            $inSection = $true; $pastHeader = $false
            continue
        }
        if ($cpuNum -lt 0) { continue }
        if (-not $inSection) { continue }
        if (-not $pastHeader) {
            if ($line -match 'Module$') { $pastHeader = $true }
            continue
        }
        if ($line -match '^Total = (\d+)$') {
            $total = [int]$Matches[1]
            [void]$perCpu.Add([PSCustomObject]@{ Cpu = $cpuNum; DpcTotalUs = $total })
            $cpuNum = -1; $inSection = $false
            continue
        }
    }
    if ($cpuNum -ge 0) {
        [void]$perCpu.Add([PSCustomObject]@{ Cpu = $cpuNum; DpcTotalUs = $total })
    }
    return ,$perCpu.ToArray()
}

# ============================================================================
# Report writer
# ============================================================================
function Write-Report {
    param($R, $PerCpu, [string]$EtlF, [string]$Path, [int]$Thr)

    $sw = [System.IO.StreamWriter]::new($Path, $false, [System.Text.Encoding]::UTF8)
    try {
        $durSec = [math]::Round($R.TraceDurationUs / 1000000.0, 2)
        $sw.WriteLine("=" * 72)
        $sw.WriteLine("  WuBigCat ETW DPC/ISR Analysis Report")
        $sw.WriteLine("=" * 72)
        $sw.WriteLine(""); $sw.WriteLine("  File     : $EtlF")
        $sw.WriteLine("  Duration : ${durSec}s")
        $sw.WriteLine("  DPCs     : $($R.TotalDpcCount) total")
        $sw.WriteLine("")

        $sw.WriteLine("--- DPC Latency Distribution ---"); $sw.WriteLine("")
        $sw.WriteLine("  Range(us)         Count       %")
        $sw.WriteLine("  -----------       -----    ----")
        foreach ($b in $R.LatencyBuckets) {
            $flag = if ($b.FromUs -ge $Thr) { " *** >${Thr}us" } else { "" }
            $rng = "$($b.FromUs)-$($b.ToUs)"
            $sw.WriteLine("  $($rng.PadRight(16)) $($b.Count.ToString().PadLeft(8)) $($b.Pct.ToString('0.00').PadLeft(7))%$flag")
        }
        $sw.WriteLine("")

        $sw.WriteLine("--- Top 15 DPC Drivers ---"); $sw.WriteLine("")
        $sw.WriteLine("  Time(us)  Driver")
        $sw.WriteLine("  --------  ------")
        foreach ($d in ($R.TopDrivers | Select -First 15)) {
            $pct = if ($R.TraceDurationUs -gt 0) { [math]::Round($d.TotalUs * 100.0 / $R.TraceDurationUs, 2) } else { 0 }
            $sw.WriteLine("  $($d.TotalUs.ToString().PadLeft(8))  $($d.Module) (${pct}%)")
        }
        $sw.WriteLine("")

        # Per-CPU DPC breakdown
        if ($PerCpu -and $PerCpu.Count -gt 0) {
            $sw.WriteLine("--- Per-CPU DPC Time ---"); $sw.WriteLine("")
            $sw.WriteLine("  CPU      DPC(us)    % of total")
            $sw.WriteLine("  ---      -------    ----------")
            $totalAll = ($PerCpu | Measure-Object DpcTotalUs -Sum).Sum
            foreach ($c in ($PerCpu | Sort-Object Cpu)) {
                $p = if ($totalAll -gt 0) { [math]::Round($c.DpcTotalUs * 100.0 / $totalAll, 1) } else { 0 }
                $marker = if ($c.DpcTotalUs -eq 0) { "  <-- CLEAN (no DPC)" } else { "" }
                $sw.WriteLine("  $($c.Cpu.ToString().PadLeft(3))    $($c.DpcTotalUs.ToString().PadLeft(8))  $($p.ToString().PadLeft(5))%$marker")
            }
            $sw.WriteLine("")
        }

        $sw.WriteLine("--- EtherCAT Cycle Stability ---"); $sw.WriteLine("")
        $hi = ($R.LatencyBuckets | ? { $_.FromUs -ge 100 }).Count
        $cr = ($R.LatencyBuckets | ? { $_.FromUs -ge 500 }).Count
        if ($cr -gt 0) { $sw.WriteLine("  [!] CRITICAL: DPCs > 500us - may disrupt 500us cycle!") }
        elseif ($hi -gt 0) { $sw.WriteLine("  [~] WARNING: DPCs > 100us - monitor for cycle jitter") }
        else { $sw.WriteLine("  [OK] No DPCs > 100us - PDO cycle should be stable") }
        $sw.WriteLine("")

        $nic = $R.TopDrivers | ? { $_.Module -match 'ndis|e1i|e1r|rt640|rtnic|rt286|rss|tcpip|afd' }
        if ($nic) {
            $sw.WriteLine("  NIC-related DPC drivers:")
            foreach ($n in $nic) { $sw.WriteLine("    $($n.Module): $($n.TotalUs) us") }
        }
        $sw.WriteLine(""); $sw.WriteLine("=" * 72)
    } finally { $sw.Close() }
}

# ============================================================================
# Main
# ============================================================================
if (-not (Test-Path $EtlPath)) {
    Write-Host "ERROR: ETL not found: $EtlPath" -ForegroundColor Red; exit 1
}
$xperfExe = Find-XperfExe
if (-not $xperfExe) {
    Write-Host "ERROR: xperf.exe not found. Install Windows ADK." -ForegroundColor Red; exit 2
}
Write-Host "  [OK] xperf: $xperfExe" -ForegroundColor Green

if (-not $OutDir) { $OutDir = Split-Path $EtlPath -Parent }
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory $OutDir -Force | Out-Null }

$base = [System.IO.Path]::GetFileNameWithoutExtension($EtlPath)
$reportPath = Join-Path $OutDir "${base}_dpcisr.txt"
$rawPath    = Join-Path $OutDir "${base}_xperf_raw.txt"

$raw = Invoke-XperfDpcIsr -XperfExe $xperfExe -EtlFile $EtlPath
$raw | Out-File $rawPath -Encoding UTF8
Write-Host "  [OK] Raw xperf: $rawPath" -ForegroundColor Green

$result = Parse-DpcIsrOutput -Raw $raw
$perCpu = Parse-DpcIsrPerCpu -Raw $raw
Write-Report -R $result -PerCpu $perCpu -EtlF $EtlPath -Path $reportPath -Thr $ThresholdUs
Write-Host "  [OK] Report: $reportPath" -ForegroundColor Green

Write-Host ""
Write-Host "=== Quick Summary ===" -ForegroundColor Cyan
$dur = [math]::Round($result.TraceDurationUs / 1000000.0, 2)
Write-Host "  Duration : ${dur}s"
Write-Host "  DPCs     : $($result.TotalDpcCount)"
if ($result.TopDrivers -and $result.TopDrivers.Count -gt 0) {
    Write-Host "  Top DPC   : $($result.TopDrivers[0].Module) ($($result.TopDrivers[0].TotalUs) us)"
}
Write-Host ""
Write-Host "  Report: $reportPath"
