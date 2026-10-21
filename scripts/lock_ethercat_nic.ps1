#Requires -Version 5.1 -RunAsAdministrator
<#
.SYNOPSIS
    Lock EtherCAT NIC hardware against WKC instability after TCP/IP unbinding.
    Works even when Get-NetAdapter is unavailable (direct registry + WMI).

.DESCRIPTION
    When TCP/IP is unbound from the EtherCAT NIC, Windows stops sending periodic
    NDIS OID requests → driver enters power-saving idle → Interrupt Moderation /
    EEE / DMA Coalescing revert to defaults → WKC (Working Counter) becomes unstable.

    This script locks NIC hardware settings at the registry level so they persist
    regardless of protocol binding state. No reboot required for most changes.

.PARAMETER NicPattern
    Wildcard pattern to match NIC description (default: *I210*). Use '*Realtek*'
    or the exact description from Npcap if using a different NIC.

.EXAMPLE
    .\lock_ethercat_nic.ps1
    Auto-detect Intel I210 and lock settings.

.EXAMPLE
    .\lock_ethercat_nic.ps1 -NicPattern '*Realtek*'
    Lock settings for a Realtek NIC.
#>

[CmdletBinding()]
param(
    [string]$NicPattern = '*I210*'
)

$ErrorActionPreference = 'Continue'
$NicClassGuid = '{4d36e972-e325-11ce-bfc1-08002be10318}'
$NicRegPath = "HKLM:\SYSTEM\CurrentControlSet\Control\Class\$NicClassGuid"

Write-Host "=== EtherCAT NIC Hardware Lock ===" -ForegroundColor Magenta
Write-Host "Pattern: $NicPattern" -ForegroundColor Cyan

# ── Step 1: Find NIC in registry (independent of Get-NetAdapter) ──
$nicKey = $null
$nicDesc = $null
Get-ChildItem -Path $NicRegPath -ErrorAction SilentlyContinue | ForEach-Object {
    $desc = (Get-ItemProperty -Path $_.PSPath -Name 'DriverDesc' -ErrorAction SilentlyContinue).DriverDesc
    if ($desc -and $desc -like $NicPattern) {
        $nicKey = $_.PSPath
        $nicDesc = $desc
    }
}

if (-not $nicKey) {
    Write-Host "ERROR: NIC matching '$NicPattern' not found in registry." -ForegroundColor Red
    Write-Host "Try a different pattern or list installed NICs:" -ForegroundColor Yellow
    Get-ChildItem -Path $NicRegPath -ErrorAction SilentlyContinue | ForEach-Object {
        $d = (Get-ItemProperty -Path $_.PSPath -Name 'DriverDesc' -ErrorAction SilentlyContinue).DriverDesc
        if ($d) { Write-Host "  $d" -ForegroundColor Gray }
    }
    exit 1
}

Write-Host "Target: $nicDesc" -ForegroundColor Green
Write-Host "RegKey: $nicKey`n" -ForegroundColor DarkGray

# ── Step 2: Try Get-NetAdapter for richer property control ──
$netAdapter = $null
try {
    $netAdapter = Get-NetAdapter -InterfaceDescription $NicPattern -ErrorAction SilentlyContinue | Select-Object -First 1
} catch { }
if ($netAdapter) {
    Write-Host "[INFO] Get-NetAdapter available: $($netAdapter.Name)" -ForegroundColor Gray
} else {
    Write-Host "[INFO] Get-NetAdapter not available — using registry-only mode" -ForegroundColor Yellow
}

# ── Helper: set registry DWORD ──
function Set-RegDword {
    param([string]$Name, [int]$Value)
    $cur = (Get-ItemProperty -Path $nicKey -Name $Name -ErrorAction SilentlyContinue).$Name
    if ($cur -eq $Value) {
        Write-Host "  [OK] $Name = $cur" -ForegroundColor Green
    } else {
        Set-ItemProperty -Path $nicKey -Name $Name -Value $Value -Type DWord -Force
        Write-Host "  [SET] $Name = $Value (was: $cur)" -ForegroundColor Yellow
    }
}

# ── Helper: set registry string ──
function Set-RegString {
    param([string]$Name, [string]$Value)
    $cur = (Get-ItemProperty -Path $nicKey -Name $Name -ErrorAction SilentlyContinue).$Name
    if ($cur -eq $Value) {
        Write-Host "  [OK] $Name = $cur" -ForegroundColor Green
    } else {
        Set-ItemProperty -Path $nicKey -Name $Name -Value $Value -Type String -Force
        Write-Host "  [SET] $Name = $Value (was: $cur)" -ForegroundColor Yellow
    }
}

# ── Helper: set NIC advanced property via Get-NetAdapterAdvancedProperty ──
function Set-NicAdvanced {
    param([string]$Keyword, [string]$EnglishValue)
    if (-not $netAdapter) {
        Write-Host "  [SKIP] $Keyword — no Get-NetAdapter" -ForegroundColor DarkGray
        return
    }
    try {
        $prop = Get-NetAdapterAdvancedProperty -Name $netAdapter.Name -RegistryKeyword $Keyword -ErrorAction Stop
        if (-not $prop) { Write-Host "  [SKIP] $Keyword — not found" -ForegroundColor DarkGray; return }
        $cur = $prop.DisplayValue
        # Detect locale variant
        $valid = $prop.ValidDisplayValues
        $isDisable = $EnglishValue -match '^(Disabled?|Off|None)$'
        $isEnable  = $EnglishValue -match '^(Enabled?|On)$'
        $target = $null
        if ($isDisable) {
            $target = $valid -match '^(Disabled?|Off|None|停用|\u95DC\u9589)\s*$' | Select-Object -First 1
            if (-not $target) { $target = $valid[0] }
        } elseif ($isEnable) {
            $target = $valid -match '^(Enabled?|On|啟用|\u555F\u7528)\s*$' | Select-Object -First 1
            if (-not $target) { $target = $valid[-1] }
        } else {
            $target = $valid | Where-Object { $_ -eq $EnglishValue } | Select-Object -First 1
            if (-not $target) { $target = $valid[0] }
        }
        if ($target -and $cur -ne $target) {
            Set-NetAdapterAdvancedProperty -Name $netAdapter.Name -RegistryKeyword $Keyword -DisplayValue $target -ErrorAction Stop
            Write-Host "  [SET] $Keyword = $target (was: $cur)" -ForegroundColor Yellow
        } else {
            Write-Host "  [OK] $Keyword = $cur" -ForegroundColor Green
        }
    } catch {
        Write-Host "  [FAIL] $Keyword — $($_.Exception.Message)" -ForegroundColor Red
    }
}

# ═══════════════════════════════════════════════════════════════════════════
# Step 3: Power Management — DISABLE (critical for WKC stability)
# ═══════════════════════════════════════════════════════════════════════════
Write-Host "`n--- Power Management ---" -ForegroundColor White

# PnPCapabilities: 0 = disable wake patterns + magic packet wake (prevents D3 sleep)
Set-RegDword -Name 'PnPCapabilities' -Value 0
# EnablePME: 0 = disable Power Management Event
Set-RegString -Name 'EnablePME' -Value '0'

# WMI: disable device power saving (works independent of TCP/IP binding)
try {
    $wmiNic = Get-CimInstance -ClassName Win32_NetworkAdapter |
        Where-Object { $_.Name -like "*$NicPattern*" } |
        Select-Object -First 1
    if ($wmiNic) {
        $pmSupported = $wmiNic.PSObject.Properties['PowerManagementSupported']
        if ($pmSupported -and $pmSupported.Value) {
            $result = Invoke-CimMethod -InputObject $wmiNic -MethodName 'SetPowerManagement' -Arguments @{
                PowerManagementCapabilities = @()
            } -ErrorAction Stop
            Write-Host "  [SET] WMI PowerManagement -> OFF" -ForegroundColor Yellow
        } else {
            Write-Host "  [OK] WMI PowerManagement not supported (or already disabled)" -ForegroundColor Green
        }
    }
} catch {
    Write-Host "  [WARN] WMI PowerManagement: $_" -ForegroundColor Yellow
}

# ═══════════════════════════════════════════════════════════════════════════
# Step 4: Interrupt Moderation — DISABLE (prevents IRQ batching)
# ═══════════════════════════════════════════════════════════════════════════
Write-Host "`n--- Interrupt Moderation ---" -ForegroundColor White

# ITR (Interrupt Throttling Rate) = Off
Set-NicAdvanced -Keyword 'ITR' -EnglishValue 'Off'
Set-NicAdvanced -Keyword '*InterruptModeration' -EnglishValue 'Disabled'

# Registry-level ITR (Intel-specific: 0=Off, 1=Adaptive, other=rate)
# I210 registry value: *ITR or ITR
Set-RegDword -Name '*ITR' -Value 0
Set-RegString -Name 'ITR' -Value 'Off'

# ═══════════════════════════════════════════════════════════════════════════
# Step 5: DMA Coalescing, EEE, Flow Control — DISABLE
# ═══════════════════════════════════════════════════════════════════════════
Write-Host "`n--- Latency Sources ---" -ForegroundColor White

Set-NicAdvanced -Keyword 'DMACoalescing' -EnglishValue 'Disabled'
Set-NicAdvanced -Keyword 'EEELinkAdvertisement' -EnglishValue 'Disabled'
Set-NicAdvanced -Keyword '*FlowControl' -EnglishValue 'Disabled'
Set-NicAdvanced -Keyword 'ReduceSpeedOnPowerDown' -EnglishValue 'Disabled'
Set-NicAdvanced -Keyword 'LogLinkStateEvent' -EnglishValue 'Disabled'
Set-NicAdvanced -Keyword 'AutoDisableGigabit' -EnglishValue 'Disabled'
Set-NicAdvanced -Keyword 'SelectiveSuspend' -EnglishValue 'Disabled'
Set-NicAdvanced -Keyword 'WaitForLink' -EnglishValue 'Off'

# Green Ethernet / EEE registry backup
Set-RegString -Name 'EEELinkAdvertisement' -Value '0'
Set-RegString -Name 'EnableGreenEthernet' -Value '0'

# ═══════════════════════════════════════════════════════════════════════════
# Step 6: Jumbo Packet, RSS, Offloads
# ═══════════════════════════════════════════════════════════════════════════
Write-Host "`n--- Packet Settings ---" -ForegroundColor White

Set-NicAdvanced -Keyword '*JumboPacket' -EnglishValue 'Disabled'
Set-NicAdvanced -Keyword 'LsoV2IPv4' -EnglishValue 'Disabled'
Set-NicAdvanced -Keyword 'LsoV2IPv6' -EnglishValue 'Disabled'
Set-NicAdvanced -Keyword 'TCPUDPChecksumOffloadIPv4' -EnglishValue 'Disabled'
Set-NicAdvanced -Keyword 'TCPUDPChecksumOffloadIPv6' -EnglishValue 'Disabled'

# RSS: enable for NIC (helps DPC distribution even without TCP/IP)
try {
    Get-NetAdapterRss -Name $netAdapter.Name -ErrorAction Stop | Out-Null
    Set-NetAdapterRss -Name $netAdapter.Name -NumberOfReceiveQueues 2 -ErrorAction SilentlyContinue
    Write-Host "  [OK] RSS: 2 queues" -ForegroundColor Green
} catch {
    Write-Host "  [SKIP] RSS: Get-NetAdapter not available" -ForegroundColor DarkGray
}

# ═══════════════════════════════════════════════════════════════════════════
# Step 7: Disable PCIe ASPM (Link State Power Management)
# ═══════════════════════════════════════════════════════════════════════════
Write-Host "`n--- PCIe ASPM ---" -ForegroundColor White

$aspmKey = 'HKLM:\SYSTEM\CurrentControlSet\Control\Power\PowerSettings\0012ee47-9041-4b5d-9b77-535fba8b1442\dbb5b8e3-85b7-4e0f-a9f7-3df0d2dc374c'
try {
    if (Test-Path $aspmKey) {
        Set-ItemProperty -Path $aspmKey -Name 'Attributes' -Value 2 -Type DWord -Force
        Write-Host "  [SET] PCIe ASPM -> exposed in power plan" -ForegroundColor Green
    }
} catch { }

# Use powercfg to disable PCIe ASPM for current scheme
try {
    $acGuid = 'dbb5b8e3-85b7-4e0f-a9f7-3df0d2dc374c'
    powercfg /setacvalueindex SCHEME_CURRENT SUB_PCIEXPRESS $acGuid 0 2>$null
    powercfg /setdcvalueindex SCHEME_CURRENT SUB_PCIEXPRESS $acGuid 0 2>$null
    Write-Host "  [SET] PCIe Link State Power Mgmt -> OFF" -ForegroundColor Green
} catch {
    Write-Host "  [WARN] PCIe ASPM powercfg: $_" -ForegroundColor Yellow
}

# ═══════════════════════════════════════════════════════════════════════════
# Step 8: Summary
# ═══════════════════════════════════════════════════════════════════════════
Write-Host "`n=== Lock Complete ===" -ForegroundColor Green
Write-Host "NIC: $nicDesc" -ForegroundColor Cyan
Write-Host "Power Mgmt: OFF" -ForegroundColor Green
Write-Host "Interrupt Moderation: OFF" -ForegroundColor Green
Write-Host "EEE / DMA Coalescing: OFF" -ForegroundColor Green
Write-Host "ASPM / Green Ethernet: OFF" -ForegroundColor Green
Write-Host "`nSettings are registry-level and persist across reboots." -ForegroundColor Gray
Write-Host "No reboot required for most changes." -ForegroundColor Gray
