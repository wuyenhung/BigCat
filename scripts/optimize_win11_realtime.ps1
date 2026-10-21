#Requires -Version 5.1
<#
.SYNOPSIS
    Win11 real-time optimization for EtherCAT / SOEM DC (2ms cycle).
    One-click system hardening: power plan, IRQ affinity, NIC tuning, services.

.DESCRIPTION
    Applies:
      1. Power plan (Ultimate Performance, Core Parking, USB suspend, disk timeout)
      2. Registry tweaks (NetworkThrottlingIndex, SystemResponsiveness, DisablePagingExecutive,
         LargeSystemCache, Win32PrioritySeparation, TCP chimney/timestamps)
      3. PCIe ASPM power management OFF
      4. MSI/MSI-X IRQ affinity (EtherCAT NIC exclusive cores, other devices → safe cores)
      5. Processor power management (C-States, throttling, perf thresholds)
      6. NIC RSS core binding (ndis.sys DPC pinned away from RT core)
      7. NIC advanced properties (Interrupt Moderation, Buffers, Offloads, EEE, VMQ, SR-IOV, etc.)
      8. NIC device power saving OFF
      9. NIC protocol isolation (disable IPv6, MS Client, File Sharing, QoS, LLTD, NetBIOS)
     10. Background services management (SysMain, Search, Defender, Update, DiagTrack)
     11. Verification summary + BIOS recommendations

.PARAMETER AdapterName
    Filter NIC by name (wildcard supported). Default: all wired Ethernet.

.PARAMETER EthercatAdapterName
    Specific EtherCAT NIC name for IRQ affinity, RSS binding & protocol isolation.
    If omitted, uses first Ethernet adapter or -AdapterName match.

.PARAMETER RtCore
    Physical core number reserved for SOEM cyclic thread (default 4).
    IRQ affinity and RSS will avoid this core and its HT sibling.

.PARAMETER SkipRegistry
    Skip registry changes.

.PARAMETER SkipPowerPlan
    Skip power plan changes.

.PARAMETER SkipServices
    Skip background services management.

.PARAMETER PreviewOnly
    Preview mode, no actual changes.

.EXAMPLE
    .\optimize_win11_realtime.ps1
    Auto-detect all Ethernet NICs and apply all optimizations.

.EXAMPLE
    .\optimize_win11_realtime.ps1 -EthercatAdapterName "乙太網路 4" -RtCore 4
    Full optimization with explicit EtherCAT NIC and RT core 4.

.EXAMPLE
    .\optimize_win11_realtime.ps1 -PreviewOnly
    Preview all changes without applying.
#>

[CmdletBinding()]
param(
    [string]$AdapterName = '',
    [string]$EthercatAdapterName = '',
    [int]$RtCore = 4,
    [switch]$SkipRegistry,
    [switch]$SkipPowerPlan,
    [switch]$SkipServices,
    [switch]$PreviewOnly
)

$Script:IsWhatIf = $PreviewOnly.IsPresent
$Script:RestartNeeded = $false
$Script:Changes = @()
$Script:EthercatNic = $null
$Script:RtPhysicalCore = $RtCore
$Script:AllPhysicalCores = @()
$Script:PrimaryLps = @()  # primary logical processors only (no HT siblings)

function Write-Step { param([string]$M, [string]$C='Cyan')  Write-Host "`n=== $M ===" -ForegroundColor $C }
function Write-OK   { param([string]$M) Write-Host "  [OK] $M" -ForegroundColor Green }
function Write-FAIL { param([string]$M) Write-Host "  [FAIL] $M" -ForegroundColor Red }
function Write-SKIP { param([string]$M) Write-Host "  [SKIP] $M" -ForegroundColor Yellow }
function Write-INFO { param([string]$M) Write-Host "  [INFO] $M" -ForegroundColor Gray }

# --- Admin check ---
if (-NOT ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole] "Administrator")) {
    Write-Host "ERROR: Must run as Administrator." -ForegroundColor Red
    exit 1
}

Write-Host "Win11 Real-Time Ethernet Optimization" -ForegroundColor Magenta
Write-Host "=====================================" -ForegroundColor Magenta
Write-Host "Host : $env:COMPUTERNAME"
Write-Host "OS   : $(Get-CimInstance Win32_OperatingSystem | Select-Object -ExpandProperty Caption)"
Write-Host "Mode : $(if ($Script:IsWhatIt) { 'Preview (WhatIf)' } else { 'Apply' })"
Write-Host "RT Core: Physical core $RtCore (SOEM cyclic thread)"

# --- CPU topology detection (needed for IRQ affinity masks) ---
function Get-CpuTopology {
    $physicalCores = @{}
    $lpToPhysical = @{}
    try {
        $cpuInfo = Get-CimInstance -ClassName Win32_Processor -ErrorAction SilentlyContinue
        if ($cpuInfo) {
            $cpu = $cpuInfo | Select-Object -First 1
            $cores = $cpu.NumberOfCores
            $logical = $cpu.NumberOfLogicalProcessors
            $htEnabled = $logical -gt $cores
            # Build physical → logical mapping
            for ($i = 0; $i -lt $cores; $i++) {
                $physicalCores[$i] = @()
            }
            # Fallback: heuristic mapping for SMT systems (Win32_Processor gives core/logical count)
            if ($htEnabled) {
                for ($i = 0; $i -lt $cores; $i++) {
                    $lp0 = $i
                    $lp1 = $i + $cores
                    $physicalCores[$i] = @($lp0, $lp1)
                    $lpToPhysical[$lp0] = $i
                    $lpToPhysical[$lp1] = $i
                }
            } else {
                for ($i = 0; $i -lt $cores; $i++) {
                    $physicalCores[$i] = @($i)
                    $lpToPhysical[$i] = $i
                }
            }
            return @{
                PhysicalCores = $physicalCores
                LpToPhysical   = $lpToPhysical
                CoreCount      = $cores
                LogicalCount   = $logical
                HyperThreading = $htEnabled
            }
        }
    } catch { }
    return $null
}

$topo = Get-CpuTopology
if ($topo) {
    $Script:AllPhysicalCores = 0..($topo.CoreCount - 1)
    # Primary logical processors only (LP 0 of each physical core, no HT siblings)
    $Script:PrimaryLps = @($Script:AllPhysicalCores | ForEach-Object { $_ })
    Write-Host "CPU   : $($topo.CoreCount)C/$($topo.LogicalCount)T $(if ($topo.HyperThreading) { '(SMT ON)' } else { '(SMT OFF)' })" -ForegroundColor Gray
    Write-Host "Primary LPs: $($Script:PrimaryLps -join ', ')" -ForegroundColor DarkGray
} else {
    Write-Host "CPU   : (detection failed, using defaults)" -ForegroundColor Yellow
    $Script:AllPhysicalCores = @(0,1,2,3,4,5,6,7)
    $Script:PrimaryLps = @(0,1,2,3,4,5,6,7)
}

# ============================================================================
# Step 0: Power Plan Optimization
# ============================================================================
if (-not $SkipPowerPlan) {
    Write-Step "Power Plan Optimization"

    # 0a. Set Ultimate Performance power scheme (or High Performance fallback)
    $upGuid = 'e9a42b02-d5df-448d-aa00-03f14749eb61'
    $hpGuid = '8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c'
    try {
        $activeScheme = powercfg /getactivescheme 2>$null
        $currentGuid = if ($activeScheme -match '([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})') { $Matches[1] } else { '' }
        if ($currentGuid -ne $upGuid) {
            $upExists = powercfg /list 2>$null | Select-String $upGuid
            if ($upExists) {
                if (-not $Script:IsWhatIt) { powercfg /setactive $upGuid 2>$null }
                Write-OK "Power plan -> Ultimate Performance"
                $Script:Changes += "Power plan -> Ultimate Performance"
            } else {
                if ($currentGuid -ne $hpGuid) {
                    if (-not $Script:IsWhatIt) { powercfg /setactive $hpGuid 2>$null }
                    Write-OK "Power plan -> High Performance (Ultimate not available)"
                    $Script:Changes += "Power plan -> High Performance"
                } else { Write-OK "Power plan already High Performance" }
            }
        } else { Write-OK "Power plan already Ultimate Performance" }
    } catch { Write-FAIL "Power plan: $_" }

    # 0b. Disable Core Parking (min cores = 100%)
    $cpGuid = '0cc5b647-c1df-4637-891a-dec35c318583'
    try {
        if (-not $Script:IsWhatIt) {
            powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR $cpGuid 100 2>$null
        }
        Write-OK "Core Parking -> min 100% (all cores awake)"
        $Script:Changes += "Core Parking -> min 100%"
    } catch { Write-FAIL "Core Parking: $_" }

    # 0c. Disable USB selective suspend
    try {
        if (-not $Script:IsWhatIt) {
            powercfg /setacvalueindex SCHEME_CURRENT 2a737441-1930-4402-8d77-b2bebba308a3 48e6b7a6-50f5-4782-a5d4-53bb8f07e226 0 2>$null
        }
        Write-OK "USB Selective Suspend -> Disabled"
        $Script:Changes += "USB Selective Suspend -> Disabled"
    } catch { Write-FAIL "USB Suspend: $_" }

    # 0d. Disable disk idle timeout (never turn off)
    $diskGuid = '0012ee47-9041-4b5d-9b77-535fba8b1442'
    $diskTimeoutGuid = '6738e2c4-e8a5-4a42-b16a-e040e769756e'
    try {
        if (-not $Script:IsWhatIt) {
            powercfg /setacvalueindex SCHEME_CURRENT $diskGuid $diskTimeoutGuid 0 2>$null
        }
        Write-OK "Disk timeout -> Never"
        $Script:Changes += "Disk timeout -> Never"
    } catch { Write-FAIL "Disk timeout: $_" }

    # Activate modified scheme
    if (-not $Script:IsWhatIt) { powercfg /setactive SCHEME_CURRENT 2>$null }
}

# ============================================================================
# Step 1: Registry tweaks
# ============================================================================
if (-not $SkipRegistry) {
    Write-Step "Registry System-Level Tweaks"

    # 1a. NetworkThrottlingIndex -> MAX (disable throttling)
    $ntPath  = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Multimedia\SystemProfile'
    $ntName  = 'NetworkThrottlingIndex'
    $ntValue = 0xFFFFFFFF
    try {
        if (-not (Test-Path $ntPath)) { New-Item -Path $ntPath -Force | Out-Null }
        $curNt = (Get-ItemProperty -Path $ntPath -Name $ntName -ErrorAction SilentlyContinue).$ntName
        if ($curNt -ne $ntValue) {
            if (-not $Script:IsWhatIt) { Set-ItemProperty -Path $ntPath -Name $ntName -Value $ntValue -Type DWord -Force }
            Write-OK "NetworkThrottlingIndex = 0xFFFFFFFF (was: $curNt)"
            $Script:Changes += "NetworkThrottlingIndex: $curNt -> 0xFFFFFFFF"
            $Script:RestartNeeded = $true
        } else { Write-OK "NetworkThrottlingIndex already 0xFFFFFFFF" }
    } catch { Write-FAIL "NetworkThrottlingIndex: $_" }

    # 1b. TCP Autotuning -> normal (default, avoid breaking TCP throughput)
    try {
        $tcpAuto = netsh int tcp show global | Select-String 'Receive Window Auto-Tuning'
        if ($tcpAuto -notmatch 'normal') {
            if (-not $Script:IsWhatIt) { netsh int tcp set global autotuninglevel=normal | Out-Null }
            Write-OK "TCP Autotuning -> normal"
            $Script:Changes += "TCP Autotuning -> normal"
            $Script:RestartNeeded = $true
        } else { Write-OK "TCP Autotuning already normal" }
    } catch { Write-FAIL "TCP Autotuning: $_" }

    # 1c. RSS -> enabled (system-wide) — required for NIC IRQ distribution
    # NOTE: NIC hardware RSS (*RSS adapter property) is handled in Step 3.
    #       System TCP RSS must stay enabled for hardware IRQ routing to work.
    try {
        $rssState = netsh int tcp show global | Select-String 'Receive-Side Scaling'
        if ($rssState -notmatch 'enabled') {
            if (-not $Script:IsWhatIt) { netsh int tcp set global rss=enabled | Out-Null }
            Write-OK "TCP RSS -> enabled (system-wide)"
            $Script:Changes += "TCP RSS -> enabled"
            $Script:RestartNeeded = $true
        } else { Write-OK "TCP RSS already enabled" }
    } catch { Write-FAIL "TCP RSS: $_" }

    # 1d. VBS / HVCI detection (informational)
    try {
        $vbs = Get-CimInstance -ClassName Win32_DeviceGuard -Namespace root\Microsoft\Windows\DeviceGuard -ErrorAction SilentlyContinue
        if ($vbs -and $vbs.VirtualizationBasedSecurityStatus -eq 2) {
            Write-Host "  [WARN] VBS (Virtualization-Based Security) is ENABLED!" -ForegroundColor Yellow
            Write-Host "         This is the #1 cause of NIC latency on Win11." -ForegroundColor Yellow
            Write-Host "         Run as Admin then reboot:" -ForegroundColor Yellow
            Write-Host "           bcdedit /set hypervisorlaunchtype off" -ForegroundColor Yellow
            Write-Host "         Or disable Memory Integrity in Windows Security." -ForegroundColor Yellow
            $Script:Changes += "VBS: needs manual disable (bcdedit /set hypervisorlaunchtype off)"
            $Script:RestartNeeded = $true
        } elseif ($vbs -and $vbs.VirtualizationBasedSecurityStatus -eq 0) {
            Write-OK "VBS is OFF"
        } else { Write-INFO "VBS status: $($vbs.VirtualizationBasedSecurityStatus)" }
    } catch { Write-INFO "Cannot query VBS status" }

    # 1e. SystemResponsiveness -> 0 (reduces MMCSS throttling on network/CPU)
    $srName = 'SystemResponsiveness'
    try {
        if (-not (Test-Path $ntPath)) { New-Item -Path $ntPath -Force | Out-Null }
        $curSr = (Get-ItemProperty -Path $ntPath -Name $srName -ErrorAction SilentlyContinue).$srName
        if ($curSr -ne 0) {
            if (-not $Script:IsWhatIt) { Set-ItemProperty -Path $ntPath -Name $srName -Value 0 -Type DWord -Force }
            Write-OK "SystemResponsiveness = 0 (was: $curSr) — reduce MMCSS throttling"
            $Script:Changes += "SystemResponsiveness: $curSr -> 0"
            $Script:RestartNeeded = $true
        } else { Write-OK "SystemResponsiveness already 0" }
    } catch { Write-FAIL "SystemResponsiveness: $_" }

    # 1f. DisablePagingExecutive -> 1 (kernel code resident, no page-fault I/O)
    $mmPath = 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management'
    $dpeName = 'DisablePagingExecutive'
    try {
        if (-not (Test-Path $mmPath)) { New-Item -Path $mmPath -Force | Out-Null }
        $curDpe = (Get-ItemProperty -Path $mmPath -Name $dpeName -ErrorAction SilentlyContinue).$dpeName
        if ($curDpe -ne 1) {
            if (-not $Script:IsWhatIt) { Set-ItemProperty -Path $mmPath -Name $dpeName -Value 1 -Type DWord -Force }
            Write-OK "DisablePagingExecutive = 1 (was: $curDpe) — kernel stays in RAM"
            $Script:Changes += "DisablePagingExecutive: $curDpe -> 1"
            $Script:RestartNeeded = $true
        } else { Write-OK "DisablePagingExecutive already 1" }
    } catch { Write-FAIL "DisablePagingExecutive: $_" }

    # 1g. LargeSystemCache -> 0 (workstation: avoid large cache flush I/O spikes)
    $lscName = 'LargeSystemCache'
    try {
        $curLsc = (Get-ItemProperty -Path $mmPath -Name $lscName -ErrorAction SilentlyContinue).$lscName
        if ($curLsc -ne 0) {
            if (-not $Script:IsWhatIt) { Set-ItemProperty -Path $mmPath -Name $lscName -Value 0 -Type DWord -Force }
            Write-OK "LargeSystemCache = 0 (was: $curLsc)"
            $Script:Changes += "LargeSystemCache: $curLsc -> 0"
            $Script:RestartNeeded = $true
        } else { Write-OK "LargeSystemCache already 0" }
    } catch { Write-FAIL "LargeSystemCache: $_" }

    # 1h. Win32PrioritySeparation -> 0x26 (short quanta, variable, foreground boost max)
    $wpsPath = 'HKLM:\SYSTEM\CurrentControlSet\Control\PriorityControl'
    $wpsName = 'Win32PrioritySeparation'
    try {
        if (-not (Test-Path $wpsPath)) { New-Item -Path $wpsPath -Force | Out-Null }
        $curWps = (Get-ItemProperty -Path $wpsPath -Name $wpsName -ErrorAction SilentlyContinue).$wpsName
        if ($curWps -ne 0x26) {
            if (-not $Script:IsWhatIt) { Set-ItemProperty -Path $wpsPath -Name $wpsName -Value 0x26 -Type DWord -Force }
            Write-OK "Win32PrioritySeparation = 0x26 (was: $curWps) — short quanta + fg boost"
            $Script:Changes += "Win32PrioritySeparation: $curWps -> 0x26"
            $Script:RestartNeeded = $true
        } else { Write-OK "Win32PrioritySeparation already 0x26" }
    } catch { Write-FAIL "Win32PrioritySeparation: $_" }

    # 1i. TCP Chimney Offload -> disabled (avoids DMA contention with EtherCAT frames)
    try {
        $chimneyState = netsh int tcp show global | Select-String 'Chimney Offload'
        if ($chimneyState -notmatch 'disabled') {
            if (-not $Script:IsWhatIt) { netsh int tcp set global chimney=disabled | Out-Null }
            Write-OK "TCP Chimney Offload -> disabled"
            $Script:Changes += "TCP Chimney Offload -> disabled"
        } else { Write-OK "TCP Chimney Offload already disabled" }
    } catch { Write-FAIL "TCP Chimney: $_" }

    # 1j. TCP Timestamps -> disabled (reduces per-packet processing overhead)
    try {
        $tsState = netsh int tcp show global | Select-String 'Timestamps'
        if ($tsState -notmatch 'disabled') {
            if (-not $Script:IsWhatIt) { netsh int tcp set global timestamps=disabled | Out-Null }
            Write-OK "TCP Timestamps -> disabled"
            $Script:Changes += "TCP Timestamps -> disabled"
        } else { Write-OK "TCP Timestamps already disabled" }
    } catch { Write-FAIL "TCP Timestamps: $_" }

    # 1k. HPET platform clock detection (informational — no change applied)
    try {
        $bcd = bcdedit /enum 2>$null | Out-String
        if ($bcd -match 'useplatformclock\s+Yes') {
            Write-Host "  [INFO] HPET platform clock ENABLED — stable QPC, +1-5us timer latency" -ForegroundColor Gray
        } elseif ($bcd -match 'useplatformclock\s+No') {
            Write-Host "  [INFO] HPET platform clock DISABLED — TSC-based QPC, lower latency" -ForegroundColor Gray
        } else { Write-INFO "HPET: useplatformclock not set (OS default)" }
    } catch { Write-INFO "Cannot query HPET state" }
}

# ============================================================================
# Step 2: PCIe ASPM -> OFF
# ============================================================================
Write-Step "PCIe Power Management (ASPM)"

$pcieGuid = '501a4d13-42b7-4429-a178-159e3af5501d'
$aspmGuid = 'ee9c1b29-9f53-4b28-b52e-9e7e0cf0a1a2'
try {
    if (-not $Script:IsWhatIt) {
        powercfg /setacvalueindex SCHEME_CURRENT $pcieGuid $aspmGuid 0 2>$null
        powercfg /setactive SCHEME_CURRENT 2>$null
    }
    Write-OK "PCIe ASPM -> Off"
    $Script:Changes += "PCIe ASPM -> Off"
} catch { Write-FAIL "PCIe ASPM: $_" }

# ============================================================================
# Step 3: MSI/MSI-X IRQ Affinity (registry — requires reboot)
# ============================================================================
Write-Step "MSI/MSI-X IRQ Affinity (DevicePolicy + AssignmentSetOverride)"

# Detect EtherCAT NIC
if ($EthercatAdapterName) {
    $Script:EthercatNic = Get-NetAdapter -Name $EthercatAdapterName -ErrorAction SilentlyContinue
    if (-not $Script:EthercatNic) {
        $Script:EthercatNic = Get-NetAdapter | Where-Object {
            $_.InterfaceDescription -like "*$EthercatAdapterName*" -or $_.Name -like "*$EthercatAdapterName*"
        } | Select-Object -First 1
    }
} else {
    # Auto-detect first Ethernet adapter (exclude Bluetooth, WiFi, WLAN in both EN and zh-TW)
    $Script:EthercatNic = Get-NetAdapter | Where-Object {
        $_.MediaType -eq '802.3' -and
        $_.Name -notlike '*Bluetooth*' -and $_.Name -notlike '*藍牙*' -and $_.Name -notlike '*藍芽*' -and
        $_.Name -notlike '*Wi-Fi*' -and $_.Name -notlike '*WLAN*' -and $_.Name -notlike '*無線*' -and
        $_.InterfaceDescription -notlike '*Bluetooth*' -and $_.InterfaceDescription -notlike '*Wireless*'
    } | Select-Object -First 1
}

# Compute IRQ affinity masks based on CPU topology
function Get-IrqAffinityMasks {
    param([int]$PhysicalCoreCount, [int]$RtCore, [int[]]$PrimaryLps)

    # Remove RT core from available cores
    $safe = $PrimaryLps | Where-Object { $_ -ne $RtCore }
    if ($safe.Count -eq 0) { $safe = @(0) }

    # Map categories to core ranges based on core count
    if ($PhysicalCoreCount -ge 8) {
        # 8+ cores: EtherCAT NIC → 6-7, GPU/USB/GameNic → 0-3, NVMe → 4-5
        $rt = $RtCore
        $ecatStart = 6; $ecatEnd = 7
        $nvmeStart = 4; $nvmeEnd = 5
        $otherStart = 0; $otherEnd = 3
    } elseif ($PhysicalCoreCount -ge 4) {
        $ecatStart = 2; $ecatEnd = 3
        $nvmeStart = 0; $nvmeEnd = 1
        $otherStart = 0; $otherEnd = 1
    } else {
        $ecatStart = 1; $ecatEnd = 1
        $nvmeStart = 0; $nvmeEnd = 0
        $otherStart = 0; $otherEnd = 0
    }

    # Build KAFFINITY masks (ULONG64)
    function MakeMask { param([int[]]$Cores) $mask = 0; foreach ($c in $Cores) { if ($c -ne $rt) { $mask = $mask -bor [Math]::Pow(2, $c) } }; return $mask }

    $ecatMask = MakeMask ($ecatStart..$ecatEnd)
    $nvmeMask = MakeMask ($nvmeStart..$nvmeEnd)
    $otherMask = MakeMask ($otherStart..$otherEnd)

    return @{
        Ethercat = @{ Cores=@($ecatStart..$ecatEnd); Mask=$ecatMask }
        NVMe     = @{ Cores=@($nvmeStart..$nvmeEnd); Mask=$nvmeMask }
        Other    = @{ Cores=@($otherStart..$otherEnd); Mask=$otherMask }
    }
}

$masks = Get-IrqAffinityMasks -PhysicalCoreCount ($topo.CoreCount) -RtCore $RtCore -PrimaryLps $Script:PrimaryLps
Write-INFO "IRQ masks: EtherCAT cores=$($masks.Ethercat.Cores -join ',')  NVMe cores=$($masks.NVMe.Cores -join ',')  Other cores=$($masks.Other.Cores -join ',')  [RT core $RtCore excluded]"

# Write DevicePolicy + AssignmentSetOverride to registry for a device instance
function Set-DeviceIrqAffinity {
    param([string]$DeviceInstanceId, [uint64]$AffinityMask, [string]$DeviceLabel)

    $regPath = "HKLM:\SYSTEM\CurrentControlSet\Enum\$DeviceInstanceId\Device Parameters\Interrupt Management\Affinity Policy"
    try {
        if (-not $Script:IsWhatIt) {
            if (-not (Test-Path $regPath)) { New-Item -Path $regPath -Force | Out-Null }
            Set-ItemProperty -Path $regPath -Name 'DevicePolicy' -Value 4 -Type DWord -Force
            $maskBytes = [BitConverter]::GetBytes($AffinityMask)
            Set-ItemProperty -Path $regPath -Name 'AssignmentSetOverride' -Value $maskBytes -Type Binary -Force
        }
        Write-Host "    IRQ affinity: $DeviceLabel -> cores $([Convert]::ToString($AffinityMask,2)) (mask=0x$($AffinityMask.ToString('X')))" -ForegroundColor Green
        $Script:Changes += "IRQ affinity: $DeviceLabel -> mask 0x$($AffinityMask.ToString('X'))"
        $Script:RestartNeeded = $true
    } catch {
        Write-Host "    IRQ affinity SKIP: $DeviceLabel — $($_.Exception.Message)" -ForegroundColor DarkGray
    }
}

# Discover devices by class and write IRQ affinity
function Set-DeviceClassIrqAffinity {
    param([string]$ClassGuid, [uint64]$AffinityMask, [string]$ClassLabel, [string[]]$ExcludeInstanceIds=@())

    try {
        $devices = Get-PnpDevice -ErrorAction SilentlyContinue |
            Where-Object { $_.ClassGuid -eq $ClassGuid -and $_.Status -ne 'Unknown' -and $_.InstanceId -notmatch '^ROOT\\' }
        foreach ($dev in $devices) {
            $skip = $false
            foreach ($excl in $ExcludeInstanceIds) {
                if ($dev.InstanceId -like "*$excl*") { $skip = $true; break }
            }
            if (-not $skip) {
                Set-DeviceIrqAffinity -DeviceInstanceId $dev.InstanceId -AffinityMask $AffinityMask -DeviceLabel "$ClassLabel :: $($dev.FriendlyName)"
            }
        }
    } catch { Write-INFO "IRQ class $ClassLabel : $_" }
}

if ($Script:EthercatNic) {
    Write-INFO "EtherCAT NIC: $($Script:EthercatNic.Name) [$($Script:EthercatNic.InterfaceDescription)]"

    # Get device instance ID for EtherCAT NIC
    try {
        $ecatPnp = Get-PnpDevice | Where-Object {
            $_.FriendlyName -like "*$($Script:EthercatNic.InterfaceDescription)*" -or
            $_.FriendlyName -eq $Script:EthercatNic.Name
        } | Select-Object -First 1

        if ($ecatPnp) {
            # EtherCAT NIC → dedicated core range
            Set-DeviceIrqAffinity -DeviceInstanceId $ecatPnp.InstanceId -AffinityMask $masks.Ethercat.Mask -DeviceLabel "EtherCAT NIC"

            # GPU class → other cores
            Set-DeviceClassIrqAffinity -ClassGuid '{4d36e968-e325-11ce-bfc1-08002be10318}' -AffinityMask $masks.Other.Mask -ClassLabel 'GPU' -ExcludeInstanceIds @()

            # NVMe class → NVMe cores
            Set-DeviceClassIrqAffinity -ClassGuid '{4d36e97b-e325-11ce-bfc1-08002be10318}' -AffinityMask $masks.NVMe.Mask -ClassLabel 'NVMe'

            # USB host controllers → other cores
            Set-DeviceClassIrqAffinity -ClassGuid '{36fc9e60-c465-11cf-8056-444553540000}' -AffinityMask $masks.Other.Mask -ClassLabel 'USB'

            # Other NICs (non-EtherCAT) → other cores
            $otherNics = Get-NetAdapter | Where-Object {
                $_.Name -ne $Script:EthercatNic.Name -and $_.Name -notlike '*Bluetooth*'
            }
            foreach ($nic in $otherNics) {
                $nicPnp = Get-PnpDevice | Where-Object {
                    $_.FriendlyName -like "*$($nic.InterfaceDescription)*" -or $_.FriendlyName -eq $nic.Name
                } | Select-Object -First 1
                if ($nicPnp) {
                    Set-DeviceIrqAffinity -DeviceInstanceId $nicPnp.InstanceId -AffinityMask $masks.Other.Mask -DeviceLabel "Other NIC: $($nic.Name)"
                }
            }
        } else {
            Write-INFO "Cannot find PnP device for EtherCAT NIC — skipping IRQ affinity"
        }
    } catch { Write-INFO "IRQ affinity enumeration: $_" }
} else {
    Write-WARN "No EtherCAT NIC identified. Use -EthercatAdapterName to specify."
}

# ============================================================================
# Step 4: Processor Power Management (C-States, throttling, perf thresholds)
# ============================================================================
Write-Step "Processor Power Management"

# 4a. Disable C-States via processor idle disable (aggressive)
$idleGuid = '5d76a2ca-e8c0-402f-a133-2158492d58ad'
try {
    if (-not $Script:IsWhatIt) {
        powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR $idleGuid 0 2>$null
    }
    Write-OK "Processor idle states -> disabled (C-States off)"
    $Script:Changes += "C-States -> disabled"
} catch { Write-FAIL "C-States: $_" }

# 4b. Performance increase threshold = 0% (immediate ramp to max frequency)
$perfIncGuid = '06cadf0e-64ed-448a-8927-ce7bf90eb35d'
try {
    if (-not $Script:IsWhatIt) {
        powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR $perfIncGuid 0 2>$null
    }
    Write-OK "Perf increase threshold -> 0% (instant ramp)"
    $Script:Changes += "Perf increase threshold -> 0%"
} catch { Write-FAIL "Perf increase: $_" }

# 4c. Performance decrease threshold = 100% (never ramp down)
$perfDecGuid = '12a0ab44-fe28-4fa9-b3bd-4b64f44960a6'
try {
    if (-not $Script:IsWhatIt) {
        powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR $perfDecGuid 100 2>$null
    }
    Write-OK "Perf decrease threshold -> 100% (never ramp down)"
    $Script:Changes += "Perf decrease threshold -> 100%"
} catch { Write-FAIL "Perf decrease: $_" }

# 4d. Processor boost mode: keep ENABLED (Turbo Boost available)
#     Setting PERFBOOSTMODE=0 locks CPU to base clock, losing boost (e.g. 4.4→3.5 GHz).
#     Real-time frequency stability is handled by min/max 100% + Ultimate Performance plan.
#     For truly deterministic clocks, disable Turbo/SpeedStep in BIOS instead.
$boostGuid = 'be337238-0d82-4146-a960-4f3749d470c7'
try {
    if (-not $Script:IsWhatIt) {
        powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR $boostGuid 1 2>$null
    }
    Write-OK "Processor boost mode -> Enabled (Turbo Boost available, 4d unchanged)"
} catch { Write-INFO "Boost mode: $_" }

# 4e. Minimum processor state = 100%
$minPerfGuid = '893dee8e-2bef-41e0-89c6-b55d0929964c'
try {
    if (-not $Script:IsWhatIt) {
        powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR $minPerfGuid 100 2>$null
    }
    Write-OK "Min processor state -> 100%"
    $Script:Changes += "Min processor state -> 100%"
} catch { Write-FAIL "Min perf: $_" }

# 4f. Maximum processor state = 100%
$maxPerfGuid = 'bc5038f7-23e0-4960-96da-33abaf5935ec'
try {
    if (-not $Script:IsWhatIt) {
        powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR $maxPerfGuid 100 2>$null
    }
    Write-OK "Max processor state -> 100%"
    $Script:Changes += "Max processor state -> 100%"
} catch { Write-FAIL "Max perf: $_" }

# Activate modified scheme
if (-not $Script:IsWhatIt) { powercfg /setactive SCHEME_CURRENT 2>$null }

# ============================================================================
# Step 5: NIC RSS Core Binding (pin ndis.sys DPC away from RT core)
# ============================================================================
Write-Step "NIC RSS Core Binding"

if ($Script:EthercatNic) {
    # Find a safe LP for RSS (not RT core, not HT sibling)
    $safeLps = $Script:PrimaryLps | Where-Object { $_ -ne $RtCore }
    $rssBase = if ($safeLps.Count -gt 0) { $safeLps | Select-Object -First 1 } else { 0 }

    try {
        $currentRss = Get-NetAdapterRss -Name $Script:EthercatNic.Name -ErrorAction SilentlyContinue
        Write-INFO "Current RSS: Base=$($currentRss.BaseProcessorNumber) Max=$($currentRss.MaxProcessorNumber) Queues=$($currentRss.NumberOfReceiveQueues)"

        if (-not $Script:IsWhatIt) {
            Set-NetAdapterRss -Name $Script:EthercatNic.Name -BaseProcessorNumber $rssBase -MaxProcessorNumber $rssBase -NumberOfReceiveQueues 1 -ErrorAction Stop
        }
        Write-OK "NIC RSS: BaseProcessor=$rssBase MaxProcessor=$rssBase Queues=1 (ndis.sys DPC pinned away from RT core $RtCore)"
        $Script:Changes += "NIC RSS -> core $rssBase (pinned)"
    } catch {
        Write-Host "  [INFO] RSS binding skipped: $($_.Exception.Message)" -ForegroundColor Gray
    }
} else {
    Write-INFO "No EtherCAT NIC for RSS binding"
}

# ============================================================================
# Step 6: NIC advanced properties
# ============================================================================
Write-Step "NIC Advanced Properties"

# Discover Ethernet adapters
if ($AdapterName) {
    $allNic = @(Get-NetAdapter -Name $AdapterName -ErrorAction SilentlyContinue)
    if (-not $allNic) {
        $allNic = @(Get-NetAdapter | Where-Object { $_.InterfaceDescription -like "*$AdapterName*" -or $_.Name -like "*$AdapterName*" })
    }
} else {
    $allNic = @(Get-NetAdapter | Where-Object {
        $_.MediaType -eq '802.3' -and
        $_.Name -notlike '*Bluetooth*' -and $_.Name -notlike '*Wi-Fi*' -and $_.Name -notlike '*WLAN*' -and
        $_.InterfaceDescription -notlike '*Wireless*' -and $_.InterfaceDescription -notlike '*Bluetooth*' -and $_.InterfaceDescription -notlike '*Wi-Fi*'
    })
}

if (-not $allNic -or $allNic.Count -eq 0) {
    Write-Warning "No Ethernet adapters found."
    exit 0
}

Write-Host "Found $($allNic.Count) Ethernet adapter(s):" -ForegroundColor White
$allNic | ForEach-Object { Write-Host "  - $($_.Name)  [$($_.InterfaceDescription)]  Status=$($_.Status)" -ForegroundColor Gray }

# Set-NicProp helper (same logic as BigCat CoreHelpers.cpp)
function Set-NicProp {
    param([string]$N, [string[]]$P, [string]$V)

    foreach ($pn in $P) {
        $prop = $null
        $prop = Get-NetAdapterAdvancedProperty -Name $N -DisplayName $pn -ErrorAction SilentlyContinue
        if (-not $prop) { $prop = Get-NetAdapterAdvancedProperty -Name $N -RegistryKeyword $pn -ErrorAction SilentlyContinue }
        if (-not $prop -and $pn.StartsWith('*')) { $prop = Get-NetAdapterAdvancedProperty -Name $N -RegistryKeyword $pn.Substring(1) -ErrorAction SilentlyContinue }
        if (-not $prop -and -not $pn.StartsWith('*')) { $prop = Get-NetAdapterAdvancedProperty -Name $N -RegistryKeyword ('*' + $pn) -ErrorAction SilentlyContinue }

        if ($prop) {
            $cur = $prop.DisplayValue
            if ($cur -eq $V) { Write-Host "    $pn = $cur (already set)" -ForegroundColor DarkGray; return }

            try {
                if (-not $Script:IsWhatIt) { Set-NetAdapterAdvancedProperty -Name $N -RegistryKeyword $prop.RegistryKeyword -DisplayValue $V -ErrorAction Stop }
                Write-Host "    SET: $pn = $V (was: $cur)" -ForegroundColor Green
                $Script:Changes += "$N :: $pn : $cur -> $V"
            } catch {
                # English DisplayValue failed — determine semantic intent and
                # search ValidDisplayValues for a locale-appropriate match.
                if ($prop.ValidDisplayValues) {
                    $valid = $prop.ValidDisplayValues
                    $off = $null

                    # Classify the requested value by semantic category
                    $wantDisable = $V -match '^(Disabled?|Off|None)$'
                    $wantEnable  = $V -match '^(Enabled?|On)$'
                    $wantMaster  = $V -match 'Master'

                    if ($wantDisable) {
                        $off = $valid | Where-Object {
                            $_ -match '^停用$|^關閉$|^Disabled$|^Disable$|^Off$|^None$|封包優先順序與 VLAN 已停用'
                        } | Select-Object -First 1
                    } elseif ($wantEnable) {
                        $off = $valid | Where-Object {
                            $_ -match '^啟用$|^Enabled$|^Enable$|^On$'
                        } | Select-Object -First 1
                    } elseif ($wantMaster) {
                        $off = $valid | Where-Object {
                            $_ -match '主要模式|Master'
                        } | Select-Object -First 1
                    } else {
                        $off = $valid | Where-Object { $_ -eq $V } | Select-Object -First 1
                    }

                    if (-not $off) { $off = $prop.ValidDisplayValues[0] }
                    try {
                        if (-not $Script:IsWhatIt) { Set-NetAdapterAdvancedProperty -Name $N -RegistryKeyword $prop.RegistryKeyword -DisplayValue $off -ErrorAction Stop }
                        Write-Host "    SET: $pn = $off (localized, was: $cur)" -ForegroundColor Green
                        $Script:Changes += "$N :: $pn : $cur -> $off (localized)"
                    } catch { Write-Host "    FAIL: $pn - $($_.Exception.Message)" -ForegroundColor Red }
                } else { Write-Host "    FAIL: $pn - $($_.Exception.Message)" -ForegroundColor Red }
            }
            return
        }
    }
    Write-Host "    SKIP: no property: $([string]::Join(' | ', $P))" -ForegroundColor DarkGray
}

foreach ($nic in $allNic) {
    $n = $nic.Name
    Write-Host "`n  --- $n ($($nic.InterfaceDescription)) ---" -ForegroundColor Cyan

    Write-Host "  Interrupt Moderation:" -ForegroundColor White
    Set-NicProp -N $n -P @('Interrupt Moderation','*InterruptModeration') -V 'Disabled'
    Set-NicProp -N $n -P @('Interrupt Moderation Rate','ITR') -V 'Off'

    Write-Host "  Rx/Tx Buffers:" -ForegroundColor White
    Set-NicProp -N $n -P @('Receive Buffers','*ReceiveBuffers') -V '2048'
    Set-NicProp -N $n -P @('Transmit Buffers','*TransmitBuffers') -V '2048'

    Write-Host "  Flow Control:" -ForegroundColor White
    Set-NicProp -N $n -P @('Flow Control','*FlowControl') -V 'Disabled'

    Write-Host "  EEE:" -ForegroundColor White
    Set-NicProp -N $n -P @('Energy Efficient Ethernet','EEELinkAdvertisement','*EEEControl') -V 'Disabled'

    Write-Host "  Power Saving:" -ForegroundColor White
    Set-NicProp -N $n -P @('ReduceSpeedOnPowerDown') -V 'Disabled'

    Write-Host "  Offloads:" -ForegroundColor White
    Set-NicProp -N $n -P @('Large Send Offload (IPv4)','*LsoV1IPv4','Large Send Offload V1 (IPv4)') -V 'Disabled'
    Set-NicProp -N $n -P @('Large Send Offload V2 (IPv4)','*LsoV2IPv4') -V 'Disabled'
    Set-NicProp -N $n -P @('Large Send Offload V2 (IPv6)','*LsoV2IPv6') -V 'Disabled'
    Set-NicProp -N $n -P @('TCP Checksum Offload (IPv4)','*TCPChecksumOffloadIPv4','*TCPUDPChecksumOffloadIPv4') -V 'Disabled'
    Set-NicProp -N $n -P @('TCP Checksum Offload (IPv6)','*TCPChecksumOffloadIPv6','*TCPUDPChecksumOffloadIPv6') -V 'Disabled'
    Set-NicProp -N $n -P @('UDP Checksum Offload (IPv4)','*UDPChecksumOffloadIPv4') -V 'Disabled'
    Set-NicProp -N $n -P @('UDP Checksum Offload (IPv6)','*UDPChecksumOffloadIPv6') -V 'Disabled'
    Set-NicProp -N $n -P @('IPv4 Checksum Offload','*IPChecksumOffloadIPv4') -V 'Disabled'

    Write-Host "  ARP/NS Offload:" -ForegroundColor White
    Set-NicProp -N $n -P @('ARP Offload','*PMARPOffload') -V 'Disabled'
    Set-NicProp -N $n -P @('NS Offload','*PMNSOffload') -V 'Disabled'

    Write-Host "  Priority/VLAN:" -ForegroundColor White
    Set-NicProp -N $n -P @('Packet Priority & VLAN','Priority & VLAN','*PriorityVLANTag') -V 'Packet Priority & VLAN Disabled'

    Write-Host "  Wake-on-LAN:" -ForegroundColor White
    Set-NicProp -N $n -P @('Wake on Magic Packet','*WakeOnMagicPacket') -V 'Disabled'
    Set-NicProp -N $n -P @('Wake on Pattern Match','*WakeOnPattern') -V 'Disabled'

    Write-Host "  DMA Coalescing:" -ForegroundColor White
    Set-NicProp -N $n -P @('DMA Coalescing','DMACoalescing') -V 'Disabled'

    Write-Host "  Jumbo Packet:" -ForegroundColor White
    Set-NicProp -N $n -P @('Jumbo Packet','*JumboPacket') -V 'Disabled'

    Write-Host "  RSS (NIC-level):" -ForegroundColor White
    Set-NicProp -N $n -P @('Receive Side Scaling','*RSS') -V 'Enabled'

    Write-Host "  Gigabit Master Mode:" -ForegroundColor White
    Set-NicProp -N $n -P @('Gigabit Master Slave Mode','PrimarySecondary','*MasterSlave') -V 'Force Master Mode'

    Write-Host "  VMQ (Virtual Machine Queues):" -ForegroundColor White
    Set-NicProp -N $n -P @('VMQ','*VMQ','Virtual Machine Queues') -V 'Disabled'

    Write-Host "  SR-IOV:" -ForegroundColor White
    Set-NicProp -N $n -P @('SR-IOV','*SRIOV','Single Root I/O Virtualization') -V 'Disabled'

    Write-Host "  Encapsulated Task Offload:" -ForegroundColor White
    Set-NicProp -N $n -P @('Encapsulated Task Offload','*EncapsulatedTaskOffload') -V 'Disabled'

    Write-Host "  Speed & Duplex:" -ForegroundColor White
    Set-NicProp -N $n -P @('Speed & Duplex','*SpeedDuplex') -V '1 Gbps Full Duplex'

    Write-Host "  Link State Event Logging:" -ForegroundColor White
    Set-NicProp -N $n -P @('Log Link State Event','LogLinkStateEvent') -V 'Disabled'

    Write-Host "  Auto Disable Gigabit:" -ForegroundColor White
    Set-NicProp -N $n -P @('Auto Disable Gigabit','*AutoDisableGigabit') -V 'Disabled'

    Write-Host "  Selective Suspend:" -ForegroundColor White
    Set-NicProp -N $n -P @('Selective Suspend','*SelectiveSuspend') -V 'Disabled'

    Write-Host "  Wait for Link:" -ForegroundColor White
    Set-NicProp -N $n -P @('Wait for Link','*WaitForLink') -V 'Off'
}

# ============================================================================
# Step 7: NIC device power management
# ============================================================================
Write-Step "NIC Device Power Management"

foreach ($nic in $allNic) {
    try {
        $wmiAdapter = Get-CimInstance -ClassName Win32_NetworkAdapter |
            Where-Object { $_.Name -like "*$($nic.InterfaceDescription)*" -or $_.NetConnectionID -eq $nic.Name } |
            Select-Object -First 1
        if ($wmiAdapter) {
            $pmSupported = $wmiAdapter.PSObject.Properties['PowerManagementSupported']
            if ($pmSupported -and $pmSupported.Value) {
                if (-not $Script:IsWhatIt) {
                    $wmiAdapter | Invoke-CimMethod -MethodName 'SetPowerManagement' -Arguments @{
                        PowerManagementCapabilities = @()
                    } -ErrorAction SilentlyContinue | Out-Null
                }
                Write-OK "$($nic.Name) :: device power saving -> OFF"
                $Script:Changes += "$($nic.Name) :: device power saving -> OFF"
            } else { Write-INFO "$($nic.Name) :: power management not supported" }
        }
    } catch { Write-INFO "$($nic.Name) :: WMI power mgmt skipped: $_" }
}

# ============================================================================
# Step 8: NIC Protocol Isolation (EtherCAT adapter only)
# ============================================================================
Write-Step "NIC Protocol Isolation"

if ($Script:EthercatNic) {
    $en = $Script:EthercatNic.Name
    Write-INFO "Isolating EtherCAT adapter: $en"

    # Disable IPv6 binding
    try {
        $ipv6 = Get-NetAdapterBinding -Name $en -ComponentID ms_tcpip6 -ErrorAction SilentlyContinue
        if ($ipv6 -and $ipv6.Enabled) {
            if (-not $Script:IsWhatIt) { Disable-NetAdapterBinding -Name $en -ComponentID ms_tcpip6 -ErrorAction Stop }
            Write-OK "IPv6 -> disabled on $en"
            $Script:Changes += "$en :: IPv6 disabled"
        } else { Write-OK "IPv6 already disabled on $en" }
    } catch { Write-INFO "IPv6 binding: $_" }

    # Disable MS Client for Networks
    try {
        $msClient = Get-NetAdapterBinding -Name $en -ComponentID ms_msclient -ErrorAction SilentlyContinue
        if ($msClient -and $msClient.Enabled) {
            if (-not $Script:IsWhatIt) { Disable-NetAdapterBinding -Name $en -ComponentID ms_msclient -ErrorAction Stop }
            Write-OK "MS Client -> disabled on $en"
            $Script:Changes += "$en :: MS Client disabled"
        } else { Write-OK "MS Client already disabled on $en" }
    } catch { Write-INFO "MS Client: $_" }

    # Disable File and Printer Sharing
    try {
        $fps = Get-NetAdapterBinding -Name $en -ComponentID ms_server -ErrorAction SilentlyContinue
        if ($fps -and $fps.Enabled) {
            if (-not $Script:IsWhatIt) { Disable-NetAdapterBinding -Name $en -ComponentID ms_server -ErrorAction Stop }
            Write-OK "File Sharing -> disabled on $en"
            $Script:Changes += "$en :: File Sharing disabled"
        } else { Write-OK "File Sharing already disabled on $en" }
    } catch { Write-INFO "File Sharing: $_" }

    # Disable QoS Packet Scheduler
    try {
        $qos = Get-NetAdapterBinding -Name $en -ComponentID ms_pacer -ErrorAction SilentlyContinue
        if ($qos -and $qos.Enabled) {
            if (-not $Script:IsWhatIt) { Disable-NetAdapterBinding -Name $en -ComponentID ms_pacer -ErrorAction Stop }
            Write-OK "QoS Scheduler -> disabled on $en"
            $Script:Changes += "$en :: QoS disabled"
        } else { Write-OK "QoS already disabled on $en" }
    } catch { Write-INFO "QoS: $_" }

    # Disable LLTD (Link Layer Topology Discovery)
    try {
        $lltd1 = Get-NetAdapterBinding -Name $en -ComponentID ms_lltdio -ErrorAction SilentlyContinue
        if ($lltd1 -and $lltd1.Enabled) {
            if (-not $Script:IsWhatIt) { Disable-NetAdapterBinding -Name $en -ComponentID ms_lltdio -ErrorAction Stop }
            Write-OK "LLTD IO -> disabled on $en"
            $Script:Changes += "$en :: LLTD IO disabled"
        }
        $lltd2 = Get-NetAdapterBinding -Name $en -ComponentID ms_rspndr -ErrorAction SilentlyContinue
        if ($lltd2 -and $lltd2.Enabled) {
            if (-not $Script:IsWhatIt) { Disable-NetAdapterBinding -Name $en -ComponentID ms_rspndr -ErrorAction Stop }
            Write-OK "LLTD Responder -> disabled on $en"
            $Script:Changes += "$en :: LLTD Responder disabled"
        }
    } catch { Write-INFO "LLTD: $_" }

    # Disable NetBIOS over TCP/IP
    try {
        $tcpipReg = Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Services\Tcpip\Parameters\Interfaces\*" -ErrorAction SilentlyContinue |
            Where-Object { $_.DhcpInterface -eq $en -or $_.PSChildName -match $en }
        if ($tcpipReg) {
            $netbiosReg = "HKLM:\SYSTEM\CurrentControlSet\Services\NetBT\Parameters\Interfaces\Tcpip_$($tcpipReg.PSChildName)"
            if (Test-Path $netbiosReg) {
                $curNb = (Get-ItemProperty -Path $netbiosReg -Name 'NetbiosOptions' -ErrorAction SilentlyContinue).NetbiosOptions
                if ($curNb -ne 2) {
                    if (-not $Script:IsWhatIt) { Set-ItemProperty -Path $netbiosReg -Name 'NetbiosOptions' -Value 2 -Type DWord -Force }
                    Write-OK "NetBIOS over TCP/IP -> disabled (was: $curNb)"
                    $Script:Changes += "$en :: NetBIOS disabled"
                } else { Write-OK "NetBIOS already disabled" }
            }
        }
    } catch { Write-INFO "NetBIOS: $_" }
} else {
    Write-INFO "No EtherCAT NIC for protocol isolation"
}

# ============================================================================
# Step 9: Background Services Management
# ============================================================================
if (-not $SkipServices) {
    Write-Step "Background Services Management"

    $servicesToStop = @(
        @{Name='SysMain'; Display='SysMain (Superfetch)'},
        @{Name='WSearch'; Display='Windows Search'},
        @{Name='DiagTrack'; Display='Diagnostics Tracking'},
        @{Name='dmwappushservice'; Display='Device Management WAP Push'},
        @{Name='WaaSMedicSvc'; Display='Windows Update Medic'}
    )

    foreach ($svc in $servicesToStop) {
        try {
            $s = Get-Service -Name $svc.Name -ErrorAction SilentlyContinue
            if (-not $s) { continue }
            if ($s.Status -eq 'Running') {
                if (-not $Script:IsWhatIt) { Stop-Service -Name $svc.Name -Force -ErrorAction Stop }
                Write-OK "$($svc.Display) -> stopped"
                $Script:Changes += "Service stopped: $($svc.Display)"
            } else { Write-OK "$($svc.Display) already stopped" }
            if ($s.StartType -ne 'Disabled') {
                if (-not $Script:IsWhatIt) { Set-Service -Name $svc.Name -StartupType Disabled -ErrorAction Stop }
                Write-OK "$($svc.Display) -> disabled"
                $Script:Changes += "Service disabled: $($svc.Display)"
            } else { Write-OK "$($svc.Display) already disabled" }
        } catch { Write-INFO "$($svc.Display): $_" }
    }

    # Windows Update — pause only (don't disable permanently)
    try {
        $wuSvc = Get-Service -Name 'wuauserv' -ErrorAction SilentlyContinue
        if ($wuSvc -and $wuSvc.Status -eq 'Running') {
            if (-not $Script:IsWhatIt) { Stop-Service -Name 'wuauserv' -Force -ErrorAction Stop }
            Write-OK "Windows Update -> stopped (temporary)"
            $Script:Changes += "Service stopped: Windows Update"
        } else { Write-OK "Windows Update already stopped" }
    } catch { Write-INFO "Windows Update: $_" }

    # Windows Defender — add exclusion for project path (less intrusive than full disable)
    try {
        $mpPref = Get-MpPreference -ErrorAction SilentlyContinue
        if ($mpPref -and $mpPref.DisableRealtimeMonitoring -eq $false) {
            $projectPath = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
            $exclusions = $mpPref.ExclusionPath
            if ($exclusions -notcontains $projectPath) {
                if (-not $Script:IsWhatIt) { Add-MpPreference -ExclusionPath $projectPath -ErrorAction Stop }
                Write-OK "Defender exclusion added: $projectPath"
                $Script:Changes += "Defender exclusion: $projectPath"
            } else { Write-OK "Defender exclusion already present" }
        } elseif ($mpPref -and $mpPref.DisableRealtimeMonitoring -eq $true) {
            Write-OK "Defender real-time monitoring already disabled"
        }
    } catch { Write-INFO "Defender exclusion: $_" }
} else {
    Write-Step "Background Services (skipped via -SkipServices)"
}

# ============================================================================
# Step 10: Summary + BIOS Recommendations
# ============================================================================
Write-Step "Summary"

if ($Script:Changes.Count -eq 0 -and -not $Script:IsWhatIt) {
    Write-Host "  All settings already optimal, no changes needed." -ForegroundColor Green
} elseif ($Script:IsWhatIt) {
    Write-Host "  (Preview mode — no changes were applied)" -ForegroundColor Yellow
} else {
    Write-Host "  Applied $($Script:Changes.Count) change(s):" -ForegroundColor Green
    $Script:Changes | ForEach-Object { Write-Host "    * $_" -ForegroundColor White }
}

if ($Script:RestartNeeded) {
    Write-Host ""
    Write-Host "  [!] Registry IRQ affinity / system tweaks require a REBOOT to take full effect!" -ForegroundColor Yellow
    Write-Host "      建议重開機後再執行一次本腳本以確認所有設定已生效。" -ForegroundColor Yellow
}

Write-Host ""
Write-Step "BIOS Recommendations" "Magenta"
Write-Host @"
  請在 BIOS/UEFI 中手動確認以下設定（影響 DC 2ms jitter 最關鍵的項目）：

  🔴 必要 (P0):
    - C-States/C1E          → Disabled  （省電狀態引入 >100us 喚醒延遲）
    - SpeedStep/EIST        → Disabled  （動態頻率導致 timestamp 不穩定）
    - Turbo Boost            → Disabled  （頻率變化造成 cycle jitter）
    - CPU Frequency          → Fixed (base clock)

  🟡 建議 (P1):
    - VT-d / IOMMU           → Enabled   （降低 DMA remapping 延遲）
    - Hyper-Threading        → Enabled   （程式自動讓 HT sibling 閒置）
    - PCIe ASPM              → Disabled  （本腳本已透過 OS 關閉，BIOS 也關閉更徹底）

  🟢 選用 (P2):
    - HPET                   → 視平台而定（穩定 QPC 但增加 timer 延遲）
    - Spread Spectrum        → Disabled  （避免 BCLK 抖動）

  驗證方法:
    .\scripts\capture_etw_trace.ps1 -DurationSec 60
    .\scripts\measure_baseline.ps1
"@

Write-Host "Done. 建議重開機後執行 smoke test 驗證。" -ForegroundColor Magenta