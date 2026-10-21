<#
.SYNOPSIS
    Apply BigCat project CPU affinity rules to a manually opened browser process.

.DESCRIPTION
    Excludes the browser from Core 0 (OS system interrupts), physical cores 2-5
    (DCS game cores), and the EtherCAT RT physical core, matching the same rules
    used by LaunchUrlInChromiumAppMode().

    The browser is restricted to physical cores 1 (I/O + storage IRQ) and
    N-2 (near-RT / non-RT threads).

.PARAMETER ProcessName
    Browser process name(s) to target. Default: "chrome","msedge","firefox".
    Wildcards accepted.

.PARAMETER ListCores
    Show the physical-to-logical core mapping for this machine and exit.

.EXAMPLE
    .\scripts\set_browser_affinity.ps1
    Apply rules to all running Chrome / Edge / Firefox processes.

.EXAMPLE
    .\scripts\set_browser_affinity.ps1 -ProcessName chrome,firefox
    Apply only to Chrome and Firefox.

.EXAMPLE
    .\scripts\set_browser_affinity.ps1 -ListCores
    Display the CPU topology map and exit.

.NOTES
    Must be run as Administrator to set process affinity.
    Uses GetLogicalProcessorInformation (kernel32) via embedded C# for topology.
#>

param(
    [string[]]$ProcessName = @("chrome", "msedge", "firefox"),
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

# Compile the C# helper
$topoAsm = Add-Type -TypeDefinition $csharpCode -Language CSharp -PassThru -ErrorAction Stop

# ============================================================
# Functions
# ============================================================

function Get-PhysicalCoreMap {
    $raw = [TopoHelper]::GetCoreMap()
    $cores = @()
    foreach ($kv in $raw.GetEnumerator()) {
        $physIdx = $kv.Key
        $lps = [int[]]$kv.Value
        $cores += [PSCustomObject]@{
            PhysIdx = $physIdx
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

    Write-Host "  Physical cores detected: $coreCount"

    # RT core = highest physical core (matches main.cpp auto-detection)
    $rtPhysIdx = $coreCount - 1
    Write-Host "  RT physical core index: $rtPhysIdx"

    $excludeMask = [UInt64]0

    # 1) Core 0
    $core0 = $CoreMap | Where-Object { $_.PhysIdx -eq 0 } | Select-Object -First 1
    if ($core0) {
        $excludeMask = $excludeMask -bor ([UInt64]1 -shl $core0.LPs[0])
        Write-Host "  Excluding Core 0 -> LP $($core0.LPs[0]) (OS system)"
    }

    # 2) RT physical core - all LPs
    $rtCore = $CoreMap | Where-Object { $_.PhysIdx -eq $rtPhysIdx } | Select-Object -First 1
    if ($rtCore) {
        foreach ($lp in $rtCore.LPs) {
            $excludeMask = $excludeMask -bor ([UInt64]1 -shl $lp)
        }
        Write-Host "  Excluding RT physical core $rtPhysIdx -> LPs $($rtCore.LPs -join ',') (EtherCAT)"
    }

    # 3) Game cores (physical 2 to 5) - all LPs
    $gameCores = $CoreMap | Where-Object { $_.PhysIdx -ge 2 -and $_.PhysIdx -le 5 }
    foreach ($gc in $gameCores) {
        foreach ($lp in $gc.LPs) {
            $excludeMask = $excludeMask -bor ([UInt64]1 -shl $lp)
        }
        Write-Host "  Excluding game physical core $($gc.PhysIdx) -> LPs $($gc.LPs -join ',') (DCS)"
    }

    $browserMask = $systemMask -band (-bnot $excludeMask)
    if ($browserMask -eq 0) { $browserMask = $systemMask }

    return @{
        SystemMask  = $systemMask
        ExcludeMask = $excludeMask
        BrowserMask = $browserMask
    }
}

function Show-CoreTable {
    param([array]$CoreMap)
    Write-Host "`nCPU Physical -> Logical Core Map:" -ForegroundColor Cyan
    Write-Host ("{0,-10} {1,-20} {2}" -f "PhysCore", "Logical CPUs", "SMT?")
    Write-Host ("{0,-10} {1,-20} {2}" -f ("-"*8), ("-"*18), ("-"*4))
    foreach ($c in $CoreMap) {
        $lpStr = $c.LPs -join ", "
        $smt = if ($c.IsSmt) { "Y" } else { "" }
        Write-Host ("{0,-10} {1,-20} {2}" -f $c.PhysIdx, $lpStr, $smt)
    }
}

# ============================================================
# Main
# ============================================================

# Require elevation
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Warning "This script must be run as Administrator (to set process affinity)."
    exit 1
}

try {
    $coreMap = Get-PhysicalCoreMap
} catch {
    Write-Error "Failed to detect CPU topology: $_"
    exit 1
}

if ($ListCores) {
    Show-CoreTable $coreMap
    exit 0
}

# Show topology
Show-CoreTable $coreMap

# Compute mask
$maskInfo = Get-BrowserAffinityMask $coreMap
$browserMask = $maskInfo.BrowserMask
$browserMaskHex = "0x$($browserMask.ToString('X'))"

Write-Host "`n  System mask  : 0x$($maskInfo.SystemMask.ToString('X'))" -ForegroundColor DarkGray
Write-Host "  Exclude mask : 0x$($maskInfo.ExcludeMask.ToString('X'))" -ForegroundColor DarkGray
Write-Host "  Browser mask : $browserMaskHex" -ForegroundColor Green

# Allowed CPUs
$allowedCpus = @()
$m = $browserMask
$idx = 0
while ($m -gt 0) {
    if ($m -band 1) { $allowedCpus += $idx }
    $m = $m -shr 1
    $idx++
}
Write-Host "  Allowed CPUs : $($allowedCpus -join ', ')" -ForegroundColor Green

# Apply to browser processes
$foundAny = $false
foreach ($name in $ProcessName) {
    $procs = Get-Process -Name $name -ErrorAction SilentlyContinue
    if (-not $procs) { continue }
    $foundAny = $true
    foreach ($p in $procs) {
        try {
            $oldMask = $p.ProcessorAffinity
            $p.ProcessorAffinity = [IntPtr]::new($browserMask)
            $newMask = $p.ProcessorAffinity
            Write-Host "  OK $($p.ProcessName) (PID $($p.Id)): affinity 0x$($oldMask.ToString('X')) -> 0x$($newMask.ToString('X'))" -ForegroundColor Green
        } catch {
            Write-Warning "  FAIL $($p.ProcessName) (PID $($p.Id)): $_"
        }
    }
}

if (-not $foundAny) {
    Write-Host "`nNo running browser processes found:" -ForegroundColor Yellow
    foreach ($name in $ProcessName) {
        Write-Host "    $name.exe"
    }
    Write-Host "`nPlease open the browser first, or specify the correct -ProcessName." -ForegroundColor Yellow
} else {
    Write-Host "`nDone. Browser is now restricted to allowed CPUs." -ForegroundColor Cyan
    Write-Host "Allowed CPUs  : $($allowedCpus -join ', ')  ($browserMaskHex)" -ForegroundColor Cyan
    Write-Host "Excluded CPUs : Core 0 (system), physical cores 2-5 (DCS game), physical core $($coreMap.Count-1) (EtherCAT RT)" -ForegroundColor Cyan
}
