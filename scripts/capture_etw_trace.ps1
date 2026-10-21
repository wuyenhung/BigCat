#Requires -Version 5.1
<#
.SYNOPSIS
    ETW / WPR kernel trace capture for EtherCAT cycle-stability diagnosis.
    Captures ISR, DPC, context-switch, and CPU-migration events.

.DESCRIPTION
    Starts WPR trace for the requested duration, then stops and saves an ETL
    file. Open the ETL in Windows Performance Analyzer (WPA) for detailed
    latency analysis.

.PARAMETER DurationSec
    Trace duration in seconds (default 60).

.PARAMETER OutputDir
    Output directory for ETL files. Default: <project>/baseline_results/etw/

.PARAMETER WprProfile
    WPR recording profile. Options: CPU, GeneralProfile, DiskIO, Network, all.
    Default: CPU,GeneralProfile (covers CPU sampling + DPC/ISR via GeneralProfile).

.PARAMETER CheckOnly
    Only check if WPR is installed and available; do not start recording.

.PARAMETER SkipAnalyze
    Skip CLI DPC/ISR analysis after capture. By default analysis runs automatically.

.PARAMETER RtCore
    Physical core number used as the EtherCAT RT core (default 4).
    The script resolves this to all logical CPUs on that physical core
    (including HT siblings) for affinity-mask checks. Controls which core
    is highlighted in CPU affinity display and thread checks.

.EXAMPLE
    # Capture 30s, RT on physical core 4 (default)
    .\scripts\capture_etw_trace.ps1

.EXAMPLE
    # Capture 60s, RT on physical core 6
    .\scripts\capture_etw_trace.ps1 -DurationSec 60 -RtCore 6

.EXAMPLE
    # Capture 120-second trace
    .\scripts\capture_etw_trace.ps1 -DurationSec 120

.EXAMPLE
    # Check if WPR is installed without recording
    .\scripts\capture_etw_trace.ps1 -CheckOnly
#>

param(
    [int]$DurationSec = 30,
    [string]$OutputDir,
    [string]$WprProfile = "CPU,GeneralProfile",
    [switch]$CheckOnly,
    [switch]$SkipAnalyze,
    [int]$RtCore = 4
)

$ErrorActionPreference = "Stop"
$script:ScriptRoot = $PSScriptRoot
$script:ProjectRoot = Resolve-Path (Join-Path $ScriptRoot "..")

if (-not $OutputDir) {
    $OutputDir = Join-Path $ProjectRoot "baseline_results\etw"
}
if (-not (Test-Path $OutputDir)) {
    New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
}

# ============================================================================
# Helper functions
# ============================================================================

function Write-Banner {
    param([string]$Text)
    $line = "=" * 70
    Write-Host ""
    Write-Host $line -ForegroundColor Cyan
    Write-Host "  $Text" -ForegroundColor Cyan
    Write-Host $line -ForegroundColor Cyan
    Write-Host ""
}

function Write-Step {
    param([string]$Text)
    Write-Host ">>> $Text" -ForegroundColor Yellow
}

function Write-OK {
    param([string]$Text)
    Write-Host "  [OK] $Text" -ForegroundColor Green
}

function Write-Warn {
    param([string]$Text)
    Write-Host "  [WARN] $Text" -ForegroundColor Magenta
}

function Write-Info {
    param([string]$Text)
    Write-Host "  [INFO] $Text" -ForegroundColor Gray
}

# ============================================================================
# WPR detection
# ============================================================================

function Find-WprExe {
    $candidates = @(
        "${env:ProgramFiles(x86)}\Windows Kits\10\Windows Performance Toolkit\wpr.exe",
        "${env:ProgramFiles}\Windows Kits\10\Windows Performance Toolkit\wpr.exe",
        "C:\Program Files (x86)\Windows Kits\10\Windows Performance Toolkit\wpr.exe"
    )

    foreach ($path in $candidates) {
        if (Test-Path $path) {
            return $path
        }
    }
    return $null
}

# ============================================================================
# CPU Topology
# ============================================================================

function Get-CoreMap {
    <#
    .SYNOPSIS
        Enumerates CPU physical-to-logical mapping using GetLogicalProcessorInformation.
    .DESCRIPTION
        Uses C# P/Invoke to call GetLogicalProcessorInformation (non-Ex version),
        which returns a flat array of SYSTEM_LOGICAL_PROCESSOR_INFORMATION entries.
        Each RelationProcessorCore entry's ProcessorMask field is a KAFFINITY bitmask
        where bit positions are Windows Task Manager CPU numbers.
        Works correctly on Intel and AMD regardless of BIOS enumeration order.
    #>
    $coreMap = [System.Collections.Generic.Dictionary[int, int[]]]::new()

    $csharpCode = @'
    using System;
    using System.Collections.Generic;
    using System.Runtime.InteropServices;

    public static class TopoHelper
    {
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool GetLogicalProcessorInformation(
            IntPtr Buffer, ref uint ReturnedLength);

        const int RelationProcessorCore = 0;

        [StructLayout(LayoutKind.Sequential)]
        struct SLPI
        {
            public ulong ProcessorMask;   // 8 bytes, offset 0
            public int Relationship;       // 4 bytes, offset 8
            public byte Flags;             // 1 byte, offset 12
            // Union tail: CACHE_DESCRIPTOR=12 or ULONG64[2]=16 → pad to 20 (12+union)
            // Full struct = 8+4+20 = 32 bytes on x64 (aligned)
            [MarshalAs(UnmanagedType.ByValArray, SizeConst = 19)]
            byte[] Reserved;              // 19 bytes, offset 13 → total 32
        }

        public static Dictionary<int, int[]> GetCoreMap()
        {
            var map = new Dictionary<int, int[]>();
            uint len = 0;
            GetLogicalProcessorInformation(IntPtr.Zero, ref len);
            if (len == 0) return map;

            IntPtr buf = Marshal.AllocHGlobal((int)len);
            try
            {
                if (!GetLogicalProcessorInformation(buf, ref len))
                    return map;

                int entrySize = Marshal.SizeOf<SLPI>();
                int count = (int)len / entrySize;
                int coreIdx = 0;

                for (int i = 0; i < count; i++)
                {
                    IntPtr ptr = buf + i * entrySize;
                    var entry = Marshal.PtrToStructure<SLPI>(ptr);
                    if (entry.Relationship == RelationProcessorCore)
                    {
                        var lps = new List<int>();
                        ulong mask = entry.ProcessorMask;
                        int bit = 0;
                        while (mask != 0)
                        {
                            if ((mask & 1) != 0) lps.Add(bit);
                            mask >>= 1;
                            bit++;
                        }
                        lps.Sort();
                        if (lps.Count > 0)
                            map[coreIdx++] = lps.ToArray();
                    }
                }
            }
            finally { Marshal.FreeHGlobal(buf); }
            return map;
        }
    }
'@

    try {
        Add-Type -TypeDefinition $csharpCode -ErrorAction Stop | Out-Null
        $result = [TopoHelper]::GetCoreMap()
        if ($result.Count -gt 0) {
            foreach ($kv in $result.GetEnumerator()) {
                $coreMap[$kv.Key] = $kv.Value
            }
        }
    } catch {
        Write-Warn "P/Invoke GetLogicalProcessorInformation failed: $_"
    }

    # Fallback: WMI heuristic (assumes primary LPs first, SMT at fixed offset)
    if ($coreMap.Count -eq 0) {
        Write-Warn "Falling back to WMI heuristic"
        $cpus = Get-CimInstance Win32_Processor | Sort-Object DeviceID
        $totalCores = ($cpus | Measure-Object NumberOfCores -Sum).Sum
        $totalLogical = ($cpus | Measure-Object NumberOfLogicalProcessors -Sum).Sum
        $coreIdx = 0
        foreach ($cpu in $cpus) {
            for ($c = 0; $c -lt $cpu.NumberOfCores; $c++) {
                $logicalCores = @($coreIdx)
                if ($totalLogical -gt $totalCores) {
                    $sibling = $coreIdx + $totalCores
                    if ($sibling -lt $totalLogical) { $logicalCores += $sibling }
                }
                $coreMap[$coreIdx] = $logicalCores
                $coreIdx++
            }
        }
    }

    return @{
        CoreMap      = $coreMap
        TotalCores   = $coreMap.Count
        TotalLogical = ($coreMap.Values | ForEach-Object { $_.Count } | Measure-Object -Sum).Sum
    }
}

# ============================================================================
# CPU Affinity Display
# ============================================================================

function Show-CpuAffinity {
    param([int]$RtCore)

    Write-Banner "CPU Affinity Assignment"

    $topo = Get-CoreMap
    $coreMap = $topo.CoreMap
    $totalCores = $topo.TotalCores
    $totalLogical = $topo.TotalLogical

    $cpus = Get-CimInstance Win32_Processor | Sort-Object DeviceID

    Write-Host "  --- CPU Topology ---" -ForegroundColor Cyan
    Write-Host "  CPU Packages     : $(@($cpus).Count)"
    Write-Host "  Physical Cores   : $totalCores"
    Write-Host "  Logical Cores    : $totalLogical"
    if ($totalLogical -gt $totalCores) {
        $htRatio = $totalLogical / $totalCores
        Write-Host "  HT/SMT Ratio     : ${htRatio}x (Hyper-Threading enabled)" -ForegroundColor Yellow
    } else {
        Write-Host "  HT/SMT           : disabled (1C1T)" -ForegroundColor Green
    }

    Write-Host ""
    Write-Host "  --- Core Map (Physical -> Logical) ---" -ForegroundColor Cyan
    foreach ($kv in $coreMap.GetEnumerator() | Sort-Object Key) {
        $lCores = $kv.Value -join ', '
        $marker = ''
        if ($kv.Key -eq $RtCore) { $marker = "  <-- RT Core (EtherCAT)" }
        Write-Host ("  PhysCore {0,-2} -> Logical: [{1}]{2}" -f $kv.Key, $lCores, $marker)
    }

    # Resolve RT physical core → logical CPUs + combined mask
    $rtLogicalCores = @()
    $rtCoreMask = [uint64]0
    if ($coreMap.ContainsKey($RtCore)) {
        $rtLogicalCores = $coreMap[$RtCore]
        foreach ($lc in $rtLogicalCores) {
            $rtCoreMask = $rtCoreMask -bor ([uint64]1 -shl $lc)
        }
    } else {
        Write-Warn "Physical core $RtCore not found in topology! Max PhysCore is $($coreMap.Count - 1)."
        $rtLogicalCores = @($RtCore)
        $rtCoreMask = ([uint64]1 -shl $RtCore)
    }

    Write-Host ""
    Write-Host "  --- RT Core Mapping ---" -ForegroundColor Cyan
    Write-Host "  Physical Core $RtCore -> Logical CPU(s): [$($rtLogicalCores -join ', ')]"
    Write-Host "  Combined Affinity Mask: 0x$($rtCoreMask.ToString('X'))"

    # Process Affinity
    Write-Host ""
    Write-Host "  --- Process CPU Affinity ---" -ForegroundColor Cyan

    $totalProcesses = 0
    $rtIncluded = @()

    try {
        $procs = Get-Process -ErrorAction SilentlyContinue
        foreach ($proc in $procs) {
            try {
                $aff = $proc.ProcessorAffinity
                if ($aff -eq $null -or $aff -eq 0) { continue }
                $affInt = [uint64]$aff.ToInt64()
                $totalProcesses++
                if (($affInt -band $rtCoreMask) -ne 0) {
                    $rtIncluded += [PSCustomObject]@{
                        Name = $proc.ProcessName
                        Id   = $proc.Id
                        Affinity = $affInt
                        Priority = $proc.PriorityClass
                    }
                }
            } catch { }
        }
    } catch { }

    Write-Host "  Total processes   : $totalProcesses (with readable affinity)"
    Write-Host "  Include PhysCore $RtCore : $($rtIncluded.Count)"

    if ($rtIncluded.Count -gt 0) {
        Write-Host ""
        Write-Host "  --- Processes with PhysCore $RtCore in affinity ---" -ForegroundColor DarkGray
        foreach ($p in $rtIncluded) {
            Write-Host ("    {0,-28} PID {1,6}  Priority: {2,-11}  Mask: 0x{3:X}" -f $p.Name, $p.Id, $p.Priority, $p.Affinity)
        }
    }

    # BigCat self diagnostics (find by process name, not $pid which is PowerShell)
    Write-Host ""
    Write-Host "  --- BigCat Process ---" -ForegroundColor Cyan
    try {
        $bigcat = Get-Process -Name "BigCat" -ErrorAction SilentlyContinue | Select-Object -First 1
        if (-not $bigcat) {
            Write-Info "BigCat.exe not running (this is normal during script-only execution)"
        } else {
            # Safely convert IntPtr affinity to uint64 (handle negative sign-extension)
            function ConvertTo-UInt64Affinity([IntPtr]$ptr) {
                $bytes = [System.BitConverter]::GetBytes($ptr.ToInt64())
                return [System.BitConverter]::ToUInt64($bytes, 0)
            }

            $selfAff = ConvertTo-UInt64Affinity $bigcat.ProcessorAffinity

            Write-Host "  PID: $($bigcat.Id), Threads: $($bigcat.Threads.Count), Priority: $($bigcat.PriorityClass)"
            Write-Host "    Affinity Mask: 0x$($selfAff.ToString('X'))"
            $allowedCores = @()
            for ($b = 0; $b -lt 64; $b++) {
                if (($selfAff -band ([uint64]1 -shl $b)) -ne 0) { $allowedCores += $b }
            }
            Write-Host "    Allowed cores: [$($allowedCores -join ', ')]"

            if (($selfAff -band $rtCoreMask) -eq 0) {
                Write-Host "    [OK] Process-level excludes RT physical core"
            } else {
                Write-Host "    [WARN] Process-level INCLUDES RT physical core"
            }

            $rtThreadOnCore = $false
            $threadsOnRt = @()
            foreach ($th in $bigcat.Threads) {
                try {
                    $thAff = ConvertTo-UInt64Affinity $th.ProcessorAffinity
                    if (($thAff -band $rtCoreMask) -ne 0) {
                        $rtThreadOnCore = $true
                        $threadsOnRt += [PSCustomObject]@{ Id = $th.Id; Affinity = $thAff }
                    }
                } catch { }
            }
            if ($rtThreadOnCore) {
                Write-Host "    [i] Thread(s) pinned to RT physical core:"
                foreach ($t in $threadsOnRt) {
                    Write-Host "      Thread ID $($t.Id), Mask: 0x$($t.Affinity.ToString('X'))"
                }
            } else {
                Write-Host "    [i] No thread pinned to logical CPU(s) [$($rtLogicalCores -join ', ')] of PhysCore $RtCore"
            }
        }
    } catch {
        Write-Warn "Cannot query BigCat: $_"
    }

    # Summary
    Write-Host ""
    Write-Banner "Summary"
    Write-Host "  RT Physical Core   : PhysCore $RtCore"
    Write-Host "  RT Logical CPUs    : [$($rtLogicalCores -join ', ')]"
    Write-Host "  RT Core Mask (all) : 0x$($rtCoreMask.ToString('X'))"
    Write-Host "  HT sibling policy  : Idle HT sibling for clean L1/L2 cache"
    if ($totalLogical -eq $totalCores) {
        Write-Host "  [i] HT is disabled, RT core is a single logical core"
    }
    if ($rtIncluded.Count -gt 0) {
        Write-Warn "  $($rtIncluded.Count) process(es) still include RT PhysCore $RtCore in affinity"
    }
    Write-Host "  [i] Most processes use all-core affinity; CpuIsolation thread-level pinning keeps them off PhysCore $RtCore"
}

# ============================================================================
# WPR profile discovery
# ============================================================================

$script:availableProfilesCache = @{}

function Get-AvailableWprProfiles {
    if ($script:availableProfilesCache.Count -gt 0) {
        return $script:availableProfilesCache
    }

    $wpr = Find-WprExe
    if (-not $wpr) { return @{} }

    $lines = @(& $wpr -profiles 2>&1)
    $profiles = @{}
    foreach ($line in $lines) {
        if ($line -match '^\s+(\w[\w\-]+)\s+\-') {
            $profiles[$matches[1]] = $true
        }
    }
    $script:availableProfilesCache = $profiles
    return $profiles
}

# ============================================================================
# Main
# ============================================================================

Write-Banner "WuBigCat ETW/WPR Kernel Trace Capture"

$wprExe = Find-WprExe
if (-not $wprExe) {
    Write-Warn "WPR not found. Install Windows Performance Toolkit."
    if ($CheckOnly) { exit 1 }

    $choice = Read-Host "WPR not found. Proceed (skip tracing)? [Y/n]"
    if ($choice -eq 'n') { exit 1 }
    Write-Info "WPR unavailable; skipping trace capture."
} else {
    Write-OK "WPR: $wprExe"
}

if ($CheckOnly) {
    if ($wprExe) { Write-OK "WPR is installed and ready." }
    Write-Info "Run without -CheckOnly to start capturing traces."
    exit 0
}

# CPU topology selection
Write-Host ""
Write-Host "  --- Select RT Physical Core ---" -ForegroundColor Cyan
Write-Host ("  {0,-10} {1}" -f "PhysCore", "Logical CPUs")
Write-Host ("  {0,-10} {1}" -f "--------", "------------")
$topo = Get-CoreMap
$coreMap = $topo.CoreMap
foreach ($kv in $coreMap.GetEnumerator() | Sort-Object Key) {
    $lps = ($kv.Value | ForEach-Object { "$_" }) -join ', '
    $defaultMarker = if ($kv.Key -eq $RtCore) { " (default)" } else { "" }
    Write-Host ("  {0,-10} [{1}]{2}" -f $kv.Key, $lps, $defaultMarker)
}
Write-Host ""
Write-Host "  Note: Enter the PhysCore number (left column), not the Task Manager CPU number."
Write-Host "  For example: PhysCore 4 = logical CPUs [8,9] = Task Manager CPU 8 and 9."
$inputStr = Read-Host "Enter RT PhysCore number [$RtCore]"
if ($inputStr -match '^\d+$') {
    $RtCore = [int]$inputStr
}
Write-Host ""

# Recompute rtLogicalCores and rtCoreMask at script scope from Show-CpuAffinity output
$script:scriptRtCoreMask = [uint64]0
$script:scriptRtLogicalCores = @()
$topo2 = Get-CoreMap
$cm2 = $topo2.CoreMap
if ($cm2.ContainsKey($RtCore)) {
    $script:scriptRtLogicalCores = $cm2[$RtCore]
    foreach ($lc in $script:scriptRtLogicalCores) {
        $script:scriptRtCoreMask = $script:scriptRtCoreMask -bor ([uint64]1 -shl $lc)
    }
}

Show-CpuAffinity -RtCore $RtCore

# WPR trace
if (-not $wprExe) {
    Write-Warn "WPR not available; skipping trace capture."
    exit 0
}

Write-Banner "Capture Plan"
Write-Host "  Duration  : ${DurationSec}s"
Write-Host "  Profiles  : $WprProfile"
$outputFile = Join-Path $OutputDir ("etw_trace_{0:yyyyMMdd_HHmmss}.etl" -f (Get-Date))
Write-Host "  Output    : $outputFile"

$profiles = $WprProfile -split ',' | ForEach-Object { $_.Trim() }
$availableProfiles = Get-AvailableWprProfiles

Write-Step "Starting WPR trace ..."
$startProfiles = @()
foreach ($p in $profiles) {
    if ($availableProfiles.Count -eq 0 -or $availableProfiles.ContainsKey($p)) {
        $startProfiles += $p
    } else {
        Write-Warn "Profile '$p' not available; skipping"
    }
}

if ($startProfiles.Count -eq 0) {
    Write-Warn "No WPR profiles available."
    exit 0
}

Write-Info "Active profiles: $($startProfiles -join ', ')"

# WPR start: suppress ErrorActionPreference so non-zero exit/stderr won't abort
$wprArgs = @(); foreach ($p in $startProfiles) { $wprArgs += @("-start", $p) }; $wprArgs += @("-filemode", "-recordtempto", "$env:TEMP")
$prevEAP = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    & $wprExe $wprArgs 2>$null
    $wprExit = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $prevEAP
}
if ($wprExit -ne 0) {
    Write-Warn "WPR start returned exit $wprExit (trace may still have started)."
}
Write-OK "WPR trace started."
Write-Info "Recording for ${DurationSec}s ..."

Start-Sleep -Seconds $DurationSec

Write-Step "Stopping WPR trace ..."
$ErrorActionPreference = 'Continue'
try {
    & $wprExe "-stop", "$outputFile" 2>$null
    $wprExit = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $prevEAP
}
if ($LASTEXITCODE -eq 0) {
    Write-OK "Trace saved: $outputFile"
} else {
    Write-Warn "WPR stop returned exit code $LASTEXITCODE."
}

if (Test-Path $outputFile) {
    $sizeMB = [math]::Round((Get-Item $outputFile).Length / 1MB, 1)
    Write-OK "ETL file size: ${sizeMB} MB"
}

if (-not $SkipAnalyze) {
    Write-Step "CLI DPC/ISR analysis ..."
    $analyzeScript = Join-Path $ScriptRoot "analyze_etl_cli.ps1"
    if (Test-Path $analyzeScript) {
        $prevAnalyzeEAP = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            & powershell -NoLogo -NoProfile -ExecutionPolicy Bypass -File $analyzeScript -EtlPath $outputFile -OutDir (Split-Path $outputFile)
        } catch {
            Write-Warn "CLI analysis failed: $_"
        } finally {
            $ErrorActionPreference = $prevAnalyzeEAP
        }
    } else {
        Write-Info "CLI analysis script not found at: $analyzeScript"
        Write-Info "Use WPA to open $outputFile for detailed analysis."
    }
}

Write-Banner "Capture Complete"
Write-Info "ETL file: $outputFile"
Write-Info "Open in Windows Performance Analyzer (WPA)."
Write-Info "Filter by logical CPU(s) $($script:scriptRtLogicalCores -join ', ') to isolate RT core activity."
