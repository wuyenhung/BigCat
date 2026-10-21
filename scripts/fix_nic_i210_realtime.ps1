#Requires -Version 5.1 -RunAsAdministrator
<#
.SYNOPSIS
    Fix I210 NIC settings broken by optimize_win11_realtime.ps1 on zh-TW Windows.
    Main issue: NIC-level *RSS was disabled -> all IRQs flood single CPU -> EtherCAT fails.
#>

$ErrorActionPreference = 'Stop'
Write-Host "=== I210 NIC Emergency Fix ===" -ForegroundColor Magenta

$nic = Get-NetAdapter -InterfaceDescription "*I210*" | Select-Object -First 1
if (-not $nic) { Write-Host "ERROR: Intel I210 not found" -ForegroundColor Red; exit 1 }

$n = $nic.Name
Write-Host "Target: $n ($($nic.InterfaceDescription))" -ForegroundColor Cyan
$props = Get-NetAdapterAdvancedProperty -Name $n

function Set-PropFix {
    param([string]$Keyword, [string]$EnglishValue)
    $p = $props | Where-Object { $_.RegistryKeyword -eq $Keyword }
    if (-not $p) { Write-Host "  SKIP: $Keyword" -ForegroundColor DarkGray; return }
    $cur = $p.DisplayValue

    # Try English DisplayValue first
    try {
        Set-NetAdapterAdvancedProperty -Name $n -RegistryKeyword $Keyword -DisplayValue $EnglishValue -ErrorAction Stop
        if ($cur -ne $EnglishValue) {
            Write-Host "  SET: $Keyword = $EnglishValue (was: $cur)" -ForegroundColor Yellow
        } else {
            Write-Host "  OK: $Keyword = $cur" -ForegroundColor Green
        }
        return
    } catch { }

    # English failed - find locale match by intent
    $valid = $p.ValidDisplayValues
    $wantOff = $EnglishValue -match '^(Disabled?|Off|None)$'
    $wantOn  = $EnglishValue -match '^(Enabled?|On)$'
    $wantMaster = $EnglishValue -match 'Master'

    $target = $null
    if ($wantOff) {
        # Match: Disabled, Off, None, zh-TW: 停用, 關閉
        $target = $valid -match '^(Disabled?|Off|None)$|^\u505C\u7528$|^\u95DC\u9589$' | Select-Object -First 1
        if (-not $target) { $target = $valid[0] }
    } elseif ($wantOn) {
        # Match: Enabled, On, zh-TW: 啟用
        $target = $valid -match '^(Enabled?|On)$|^\u555F\u7528$' | Select-Object -First 1
        if (-not $target) { $target = $valid[-1] }
    } elseif ($wantMaster) {
        $target = $valid -match 'Master|\u4E3B\u8981\u6A21\u5F0F' | Select-Object -First 1
        if (-not $target) { $target = $valid[0] }
    } else {
        $target = $valid | Where-Object { $_ -eq $EnglishValue } | Select-Object -First 1
        if (-not $target) { $target = $valid[0] }
    }

    if (-not $target) {
        Write-Host "  FAIL: $Keyword - no match for '$EnglishValue'" -ForegroundColor Red
        return
    }

    try {
        Set-NetAdapterAdvancedProperty -Name $n -RegistryKeyword $Keyword -DisplayValue $target -ErrorAction Stop
        Write-Host "  SET: $Keyword = $target (was: $cur)" -ForegroundColor Yellow
    } catch {
        Write-Host "  FAIL: $Keyword - $($_.Exception.Message)" -ForegroundColor Red
    }
}

Write-Host "`n--- Critical Fixes ---" -ForegroundColor White
# 1. Enable NIC-level RSS (distributes IRQs across CPUs)
Set-PropFix -Keyword '*RSS' -EnglishValue 'Enabled'
# 2. Disable Interrupt Moderation Rate (no batching)
Set-PropFix -Keyword 'ITR' -EnglishValue 'Off'
# 3. Disable speed reduction on power down
Set-PropFix -Keyword 'ReduceSpeedOnPowerDown' -EnglishValue 'Disabled'
# 4. Disable link state event logging
Set-PropFix -Keyword 'LogLinkStateEvent' -EnglishValue 'Disabled'
# 5. Force Master mode (avoids auto-neg latency)
Set-PropFix -Keyword 'PrimarySecondary' -EnglishValue 'Force Master Mode'

Write-Host "`n--- Confirm Optimal ---" -ForegroundColor White
Set-PropFix -Keyword '*InterruptModeration' -EnglishValue 'Disabled'
Set-PropFix -Keyword '*FlowControl' -EnglishValue 'Disabled'
Set-PropFix -Keyword 'EEELinkAdvertisement' -EnglishValue 'Disabled'
Set-PropFix -Keyword 'DMACoalescing' -EnglishValue 'Disabled'
Set-PropFix -Keyword '*JumboPacket' -EnglishValue 'Disabled'

Write-Host "`n--- TCP Global ---" -ForegroundColor White
netsh int tcp set global rss=enabled
Write-Host "  TCP RSS: enabled"

$ntPath = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Multimedia\SystemProfile'
if (-not (Test-Path $ntPath)) { New-Item -Path $ntPath -Force | Out-Null }
Set-ItemProperty -Path $ntPath -Name 'NetworkThrottlingIndex' -Value 0xFFFFFFFF -Type DWord -Force
Write-Host "  NetworkThrottlingIndex: 0xFFFFFFFF"

Write-Host "`n=== Done - Please REBOOT ===" -ForegroundColor Green
