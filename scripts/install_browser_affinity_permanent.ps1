<#
.SYNOPSIS
    Permanently apply BigCat CPU affinity rules to browsers via Image File Execution Options.

.DESCRIPTION
    Writes CpuAffinityMask under HKLM\...\Image File Execution Options\<browser>.exe\PerfOptions.
    Once set, Windows automatically restricts the browser to non-game, non-RT, non-system cores
    at every launch -- no manual script or background monitor needed.

    Affinity mask = system mask \ {Core 0, game physical cores 2~5, RT physical core (highest)}.

.PARAMETER BrowserNames
    Browser executable names to configure. Default: "chrome","msedge","firefox".

.PARAMETER Remove
    Remove the PerfOptions affinity entries instead of installing them.

.PARAMETER Status
    Show current IFEO affinity settings for tracked browsers and exit.

.PARAMETER ListCores
    Show the physical-to-logical core mapping for this machine and exit.

.EXAMPLE
    .\scripts\install_browser_affinity_permanent.ps1
    Apply rules to Chrome, Edge, and Firefox.

.EXAMPLE
    .\scripts\install_browser_affinity_permanent.ps1 -Remove
    Remove the affinity settings.

.EXAMPLE
    .\scripts\install_browser_affinity_permanent.ps1 -Status
    Check which browsers have IFEO affinity configured.

.EXAMPLE
    .\scripts\install_browser_affinity_permanent.ps1 -ListCores
    Display the CPU topology map only.

.NOTES
    Must be run as Administrator unless -ListCores or -Status is used.
    The CpuAffinityMask value is read by Windows at process creation time;
    existing browser processes are NOT affected; restart them to apply.
#>

param(
    [string[]]$BrowserNames = @("chrome", "msedge", "firefox"),
    [switch]$Remove,
    [switch]$Status,
    [switch]$ListCores
)

# ============================================================
# Embedded C# helper for CPU topology via GetLogicalProcessorInformation
# ============================================================
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
        public ulong ProcessorMask;
        public int Relationship;
        public byte Flags;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 19)]
        byte[] Reserved;
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

Add-Type -TypeDefinition $csharpCode -Language CSharp -ErrorAction Stop | Out-Null

# ============================================================
# Functions
# ============================================================

function Get-PhysicalCoreMap {
    $raw = [TopoHelper]::GetCoreMap()
    $cores = @()
    foreach ($kv in $raw.GetEnumerator()) {
        $lps = [int[]]$kv.Value
        $cores += [PSCustomObject]@{
            PhysIdx = $kv.Key
            LPs     = $lps
            IsSmt   = $lps.Count -ge 2
        }
    }
    return $cores | Sort-Object PhysIdx
}

function Get-BrowserAffinityMask {
    param([array]$CoreMap)
    $systemMask = [UInt64]0
    $coreCount = $CoreMap.Count

    foreach ($c in $CoreMap) {
        foreach ($lp in $c.LPs) {
            $systemMask = $systemMask -bor ([UInt64]1 -shl $lp)
        }
    }

    $rtPhysIdx = $coreCount - 1
    $excludeMask = [UInt64]0

    # Core 0 primary LP
    $core0 = $CoreMap | Where-Object { $_.PhysIdx -eq 0 } | Select-Object -First 1
    if ($core0) {
        $excludeMask = $excludeMask -bor ([UInt64]1 -shl $core0.LPs[0])
    }

    # RT physical core - all LPs
    $rtCore = $CoreMap | Where-Object { $_.PhysIdx -eq $rtPhysIdx } | Select-Object -First 1
    if ($rtCore) {
        foreach ($lp in $rtCore.LPs) {
            $excludeMask = $excludeMask -bor ([UInt64]1 -shl $lp)
        }
    }

    # Game cores (physical 2 to 5) - all LPs
    foreach ($gc in $CoreMap) {
        if ($gc.PhysIdx -ge 2 -and $gc.PhysIdx -le 5) {
            foreach ($lp in $gc.LPs) {
                $excludeMask = $excludeMask -bor ([UInt64]1 -shl $lp)
            }
        }
    }

    $browserMask = $systemMask -band (-bnot $excludeMask)
    if ($browserMask -eq 0) { $browserMask = $systemMask }

    return @{
        SystemMask  = $systemMask
        ExcludeMask = $excludeMask
        BrowserMask = $browserMask
        PhysCount   = $coreCount
        RtPhysIdx   = $rtPhysIdx
    }
}

function Show-CoreTable {
    param([array]$CoreMap)
    Write-Host "`nCPU Physical -> Logical Core Map:" -ForegroundColor Cyan
    Write-Host ("{0,-10} {1,-22} {2}" -f "PhysCore", "Logical CPUs (LP)", "SMT?")
    Write-Host ("{0,-10} {1,-22} {2}" -f ("-"*8), ("-"*20), ("-"*4))
    foreach ($c in $CoreMap) {
        $lpStr = $c.LPs -join ", "
        $smt = if ($c.IsSmt) { "Y" } else { "" }
        Write-Host ("{0,-10} {1,-22} {2}" -f $c.PhysIdx, $lpStr, $smt)
    }
}

# ============================================================
# Registry helpers
# ============================================================

$IFEO_BASE = "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options"

function Get-IfeoPath {
    param([string]$BrowserExe)
    return "$IFEO_BASE\$BrowserExe"
}

function Get-PerfOptPath {
    param([string]$BrowserExe)
    return "$IFEO_BASE\$BrowserExe\PerfOptions"
}

function Write-PerfOptionsAffinity {
    param([string]$BrowserExe, [UInt64]$Mask)

    $perfPath = Get-PerfOptPath $BrowserExe
    if (-not (Test-Path $perfPath)) {
        New-Item -Path $perfPath -Force -ItemType Directory | Out-Null
    }

    # Write as REG_BINARY (8-byte little-endian QWORD) for safe >32 core support
    $maskBytes = [BitConverter]::GetBytes($Mask)
    Set-ItemProperty -Path $perfPath -Name "CpuAffinityMask" -Value $maskBytes -Type Binary
}

function Remove-PerfOptionsAffinity {
    param([string]$BrowserExe)

    $perfPath = Get-PerfOptPath $BrowserExe
    if (Test-Path $perfPath) {
        $val = Get-ItemProperty -Path $perfPath -Name "CpuAffinityMask" -ErrorAction SilentlyContinue
        if ($val -ne $null) {
            Remove-ItemProperty -Path $perfPath -Name "CpuAffinityMask" -ErrorAction SilentlyContinue
        }
        # Clean up empty PerfOptions folder
        $props = Get-ItemProperty -Path $perfPath
        $userProps = $props.PSObject.Properties | Where-Object {
            $_.Name -ne 'PSPath' -and $_.Name -ne 'PSParentPath' -and
            $_.Name -ne 'PSChildName' -and $_.Name -ne 'PSDrive' -and
            $_.Name -ne 'PSProvider'
        }
        if (-not $userProps) {
            Remove-Item -Path $perfPath -Force -ErrorAction SilentlyContinue
        }
    }
}

function Show-IfeoStatus {
    param([string[]]$BrowserNames)
    Write-Host "`nCurrent IFEO CpuAffinityMask settings:" -ForegroundColor Cyan
    $foundAny = $false
    foreach ($name in $BrowserNames) {
        $exe = "$name.exe"
        $perfPath = Get-PerfOptPath $exe
        if (Test-Path $perfPath) {
            $val = Get-ItemProperty -Path $perfPath -Name "CpuAffinityMask" -ErrorAction SilentlyContinue
            if ($val -ne $null -and $val.CpuAffinityMask -ne $null) {
                $foundAny = $true
                $raw = $val.CpuAffinityMask
                $mask = [UInt64]0
                if ($raw -is [byte[]]) {
                    for ($i = 0; $i -lt $raw.Length -and $i -lt 8; $i++) {
                        $mask = $mask -bor ([UInt64]$raw[$i] -shl ($i * 8))
                    }
                } else {
                    $mask = [UInt64]$raw
                }
                $allowed = @()
                $m = $mask
                $idx = 0
                while ($m -gt 0) {
                    if ($m -band 1) { $allowed += $idx }
                    $m = $m -shr 1
                    $idx++
                }
                Write-Host ("  {0,-14} mask = 0x{1:X16}  allowed CPUs = {2}" -f $exe, $mask, ($allowed -join ', ')) -ForegroundColor Green
            }
        }
    }
    if (-not $foundAny) {
        Write-Host "  (none configured)" -ForegroundColor DarkGray
    }
}

# ============================================================
# Main
# ============================================================

# Detect topology (needed for all modes)
try {
    $coreMap = Get-PhysicalCoreMap
} catch {
    Write-Error "Cannot detect CPU topology: $_"
    exit 1
}

# Compute mask
$maskInfo = Get-BrowserAffinityMask $coreMap
$browserMask = $maskInfo.BrowserMask

$allowedCpus = @()
$m = $browserMask
$idx = 0
while ($m -gt 0) {
    if ($m -band 1) { $allowedCpus += $idx }
    $m = $m -shr 1
    $idx++
}

# Show topology once
Show-CoreTable $coreMap

Write-Host "`n  Computed affinity mask for browsers: 0x$($browserMask.ToString('X16'))" -ForegroundColor Green
Write-Host "  Allowed CPUs: $($allowedCpus -join ', ')" -ForegroundColor Green
Write-Host "  Excluded    : Core 0 (OS), physical cores 2-5 (DCS game), physical core $($maskInfo.RtPhysIdx) (EtherCAT RT)`n" -ForegroundColor DarkGray

# --- ListCores mode ---
if ($ListCores) {
    exit 0
}

# --- Status mode (no admin needed) ---
if ($Status) {
    Show-IfeoStatus $BrowserNames
    exit 0
}

# For admin-only operations, check elevation
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Warning "Must be run as Administrator (writes to HKLM)."
    exit 1
}

# --- Remove mode ---
if ($Remove) {
    Write-Host "Removing IFEO CpuAffinityMask for tracked browsers..." -ForegroundColor Yellow
    foreach ($name in $BrowserNames) {
        Remove-PerfOptionsAffinity "$name.exe"
        Write-Host "  Removed: $name.exe" -ForegroundColor Yellow
    }
    Write-Host "`nDone. Browsers will now use default CPU affinity." -ForegroundColor Cyan
    exit 0
}

# --- Install mode ---
Write-Host "Installing permanent CPU affinity rules via Image File Execution Options..." -ForegroundColor Cyan

foreach ($name in $BrowserNames) {
    $exe = "$name.exe"
    $ifeoPath = Get-IfeoPath $exe
    if (-not (Test-Path $ifeoPath)) {
        New-Item -Path $ifeoPath -Force -ItemType Directory | Out-Null
    }
    Write-PerfOptionsAffinity $exe $browserMask
    Write-Host ("  OK {0,-14} CpuAffinityMask = 0x{1:X16}" -f $exe, $browserMask) -ForegroundColor Green
}

Write-Host "`nDone. From now on, Windows will automatically restrict these browsers:" -ForegroundColor Cyan
foreach ($name in $BrowserNames) {
    Write-Host "  o $name.exe -> allowed CPUs = $($allowedCpus -join ', ')" -ForegroundColor Cyan
}
Write-Host ""
Write-Host "Notes:" -ForegroundColor Yellow
Write-Host "  1. Already-running browsers are NOT affected; restart them to pick up the rule." -ForegroundColor Yellow
Write-Host "  2. To undo: run with -Remove as Administrator." -ForegroundColor Yellow
Write-Host "  3. If CPU topology changes (e.g. CPU upgrade), re-run this script to update the mask." -ForegroundColor Yellow
