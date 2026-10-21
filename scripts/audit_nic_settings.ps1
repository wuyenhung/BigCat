#Requires -Version 5.1
<#
.SYNOPSIS
    P3 NIC driver settings audit - reads current NIC advanced properties
    relevant to EtherCAT real-time stability (Interrupt Moderation, EEE,
    Flow Control, RSS, Jumbo Packet, VLAN, power management).

.DESCRIPTION
    Read-only audit script for NIC real-time tuning.
    Lists current NIC settings against the recommended values from
    docs/windows-realtime-optimization.md. Does NOT modify any settings
    unless -Apply is explicitly specified.

    Recommended NIC settings for EtherCAT:
      - Interrupt Moderation : Disabled
      - Energy Efficient Ethernet (EEE) : Disabled
      - Flow Control : Disabled
      - RSS BaseProcessor : first non-RT primary LP
                            (see CoreHelpers::OptimizeNicIrqAffinity)
      - RSS MaxProcessor  : same as BaseProcessorNumber (single LP)
      - RSS Queues         : 1
      - Jumbo Packet       : Disabled (no benefit for EtherCAT)
      - VLAN / Priority    : Disabled (bare-metal EtherCAT)

.PARAMETER AdapterName
    Filter to a specific adapter by name (supports wildcard). Default: all 'Up' adapters.

.PARAMETER OutputFile
    Save audit results to a text file.

.PARAMETER JsonOutput
    Save audit results as JSON in addition to console output.

.PARAMETER Apply
    DANGER: Apply recommended settings to matching adapters. Requires admin.
    Will prompt for confirmation before each change.

.PARAMETER WhatIf
    Show what would be changed without actually applying.

.EXAMPLE
    # Audit all active adapters
    .\scripts\audit_nic_settings.ps1

.EXAMPLE
    # Audit a specific adapter
    .\scripts\audit_nic_settings.ps1 -AdapterName "Intel*"

.EXAMPLE
    # Save audit to file
    .\scripts\audit_nic_settings.ps1 -OutputFile .\baseline_results\nic_audit.txt

.EXAMPLE
    # Preview changes without applying
    .\scripts\audit_nic_settings.ps1 -WhatIf
#>

param(
    [string]$AdapterName = "*",
    [string]$OutputFile,
    [switch]$JsonOutput,
    [switch]$Apply,
    [switch]$WhatIf
)

$ErrorActionPreference = "Continue"
$script:ScriptRoot = $PSScriptRoot
$script:ProjectRoot = Resolve-Path (Join-Path $ScriptRoot "..")
$OutputDir = Join-Path $ProjectRoot "baseline_results"

if (-not (Test-Path $OutputDir)) {
    New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
}

# ============================================================================
# Recommended settings per docs/windows-realtime-optimization.md
# ============================================================================

$Recommended = @{
    # DisplayName patterns and recommended values
    'Interrupt Moderation'   = @{ Rec='Disable'; Keywords=@('Disable','Disabled','Off'); Check='Interrupt Moderation' }
    'Moderation'             = @{ Rec='Disable'; Keywords=@('Disable','Disabled','Off'); Check='Interrupt Moderation' }
    'Energy Efficient'       = @{ Rec='Disable'; Keywords=@('Disable','Disabled','Off'); Check='EEE' }
    'Green Ethernet'         = @{ Rec='Disable'; Keywords=@('Disable','Disabled','Off'); Check='EEE/Green' }
    'Flow Control'           = @{ Rec='Disable'; Keywords=@('Disable','Disabled','Off','Rx & Tx Disabled'); Check='Flow Control' }
    'Jumbo Packet'           = @{ Rec='Disable'; Keywords=@('Disable','Disabled','1514','1518'); Check='Jumbo' }
    'Priority & VLAN'        = @{ Rec='Disable'; Keywords=@('Disable','Disabled','Priority & VLAN Disabled'); Check='VLAN' }
    'Packet Priority'        = @{ Rec='Disable'; Keywords=@('Disable','Disabled'); Check='VLAN/Priority' }
    'VLAN'                   = @{ Rec='Disable'; Keywords=@('Disable','Disabled'); Check='VLAN' }
    'Receive Side Scaling'   = @{ Rec='Enable'; Keywords=@('Enable','Enabled','On'); Check='RSS' }
    'RSS'                    = @{ Rec='Enable'; Keywords=@('Enable','Enabled','On'); Check='RSS' }
    'Wake on'                = @{ Rec='Disable'; Keywords=@('Disable','Disabled'); Check='Power Mgmt' }
    'ARP Offload'            = @{ Rec='Disable'; Keywords=@('Disable','Disabled'); Check='Offload' }
    'NS Offload'             = @{ Rec='Disable'; Keywords=@('Disable','Disabled'); Check='Offload' }
    'TCP Checksum Offload'   = @{ Rec='Disable'; Keywords=@('Disable','Disabled'); Check='Offload' }
    'UDP Checksum Offload'   = @{ Rec='Disable'; Keywords=@('Disable','Disabled'); Check='Offload' }
    'Large Send Offload'     = @{ Rec='Disable'; Keywords=@('Disable','Disabled'); Check='Offload' }
}

# ============================================================================
# Helper functions
# ============================================================================

function Write-Banner {
    param([string]$Text)
    Write-Host ""
    Write-Host ("=" * 70) -ForegroundColor Cyan
    Write-Host "  $Text" -ForegroundColor Cyan
    Write-Host ("=" * 70) -ForegroundColor Cyan
    Write-Host ""
}

function Write-OK   { param([string]$T) Write-Host "  [OK] $T" -ForegroundColor Green }
function Write-Warn { param([string]$T) Write-Host "  [WARN] $T" -ForegroundColor Magenta }
function Write-Fail { param([string]$T) Write-Host "  [FAIL] $T" -ForegroundColor Red }
function Write-Info { param([string]$T) Write-Host "  [INFO] $T" -ForegroundColor Gray }

function Get-ComplianceStatus {
    param([string]$DisplayName, [string]$CurrentValue, [hashtable]$RecEntry)

    if (-not $RecEntry) { return 'UNKNOWN' }

    $currentLower = $CurrentValue.ToLower()
    foreach ($kw in $RecEntry.Keywords) {
        if ($currentLower -match [regex]::Escape($kw.ToLower())) {
            return 'PASS'
        }
    }
    return 'FAIL'
}

function Test-IsEthercatAdapter {
    param($Adapter)
    # EtherCAT adapters typically use Intel i210/i225 or specific Realtek chips
    # but the most reliable check is: is it an Ethernet adapter with link?
    return ($Adapter.NdisPhysicalMedium -eq 0 -or                         # 802.3 Ethernet
            $Adapter.NdisPhysicalMedium -eq 14 -or                        # Native 802.3
            $Adapter.InterfaceDescription -match 'Ethernet|i210|i225|GBE|GbE|Gigabit|PRO/1000')
}

function Get-NicRssAudit {
    param($Adapter)
    try {
        $rss = Get-NetAdapterRss -Name $Adapter.Name -ErrorAction SilentlyContinue
        if (-not $rss) { return $null }
        return [PSCustomObject]@{
            Enabled          = $rss.Enabled
            NumberOfReceiveQueues = $rss.NumberOfReceiveQueues
            BaseProcessorNumber   = $rss.BaseProcessorNumber
            MaxProcessorNumber    = $rss.MaxProcessorNumber
            MaxProcessors         = $rss.MaxProcessors
            Profile               = $rss.Profile
        }
    } catch { return $null }
}

function Get-NicPowerAudit {
    param($Adapter)
    try {
        $pm = Get-NetAdapterPowerManagement -Name $Adapter.Name -ErrorAction SilentlyContinue
        if (-not $pm) { return $null }
        $wakePatterns = @()
        if ($pm.WakeOnMagicPacket -eq 'Enabled') { $wakePatterns += 'MagicPacket' }
        if ($pm.WakeOnPattern -eq 'Enabled') { $wakePatterns += 'Pattern' }
        return [PSCustomObject]@{
            AllowComputerToTurnOffDevice = $pm.AllowComputerToTurnOffDevice
            WakeOnMagicPacket = $pm.WakeOnMagicPacket
            WakeOnPattern     = $pm.WakeOnPattern
            WakePatterns      = ($wakePatterns -join ',')
        }
    } catch { return $null }
}

# ============================================================================
# Main audit
# ============================================================================

function Invoke-NicAudit {
    param($Adapter)

    $results = @()

    # --- RSS settings ---
    $rss = Get-NicRssAudit -Adapter $Adapter
    if ($rss) {
        $rssStatus = if ($rss.Enabled) { 'PASS' } else { 'FAIL' }
        $results += [PSCustomObject]@{
            Adapter   = $Adapter.Name
            Category  = 'RSS'
            Setting   = 'RSS Enabled'
            Current   = $rss.Enabled.ToString()
            Expected  = 'True'
            Status    = $rssStatus
        }
        $results += [PSCustomObject]@{
            Adapter   = $Adapter.Name
            Category  = 'RSS'
            Setting   = 'RSS BaseProcessor'
            Current   = $rss.BaseProcessorNumber
            Expected  = '4'
            Status    = if ($rss.BaseProcessorNumber -eq 4) { 'PASS' } else { 'WARN' }
        }
        $results += [PSCustomObject]@{
            Adapter   = $Adapter.Name
            Category  = 'RSS'
            Setting   = 'RSS Queues'
            Current   = $rss.NumberOfReceiveQueues
            Expected  = '1'
            Status    = if ($rss.NumberOfReceiveQueues -le 2) { 'PASS' } else { 'WARN' }
        }
    }

    # --- Advanced properties ---
    try {
        $advanced = Get-NetAdapterAdvancedProperty -Name $Adapter.Name -ErrorAction SilentlyContinue
        foreach ($prop in $advanced) {
            $matched = $false
            foreach ($key in $Recommended.Keys) {
                if ($prop.DisplayName -match $key) {
                    $rec = $Recommended[$key]
                    $status = Get-ComplianceStatus -DisplayName $prop.DisplayName `
                        -CurrentValue $prop.DisplayValue -RecEntry $rec
                    $results += [PSCustomObject]@{
                        Adapter   = $Adapter.Name
                        Category  = $rec.Check
                        Setting   = $prop.DisplayName
                        Current   = $prop.DisplayValue
                        Expected  = $rec.Rec
                        Status    = $status
                    }
                    $matched = $true
                    break
                }
            }
            # Report ALL advanced properties for completeness, even if not in our checklist
            if (-not $matched) {
                $results += [PSCustomObject]@{
                    Adapter   = $Adapter.Name
                    Category  = 'Other'
                    Setting   = $prop.DisplayName
                    Current   = $prop.DisplayValue
                    Expected  = '-'
                    Status    = 'INFO'
                }
            }
        }
    } catch {
        Write-Warn "Cannot read advanced properties for $($Adapter.Name): $_"
    }

    # --- Power management ---
    $power = Get-NicPowerAudit -Adapter $Adapter
    if ($power) {
        $allowOffOk = if ($power.AllowComputerToTurnOffDevice -eq 'Disabled') { 'PASS' } else { 'WARN' }
        $results += [PSCustomObject]@{
            Adapter   = $Adapter.Name
            Category  = 'Power Mgmt'
            Setting   = 'Allow Turn Off'
            Current   = $power.AllowComputerToTurnOffDevice
            Expected  = 'Disabled'
            Status    = $allowOffOk
        }
    }

    return $results
}

function Format-AuditTable {
    param($Results)

    if (-not $Results -or $Results.Count -eq 0) {
        Write-Warn "No audit results to display."
        return
    }

    # Print per-adapter sections
    $adapters = $Results | Select-Object -ExpandProperty Adapter -Unique
    foreach ($adapterName in $adapters) {
        $adapterResults = $Results | Where-Object { $_.Adapter -eq $adapterName }
        Write-Host ""
        Write-Host "  --- $adapterName ---" -ForegroundColor Cyan

        # Summary counts
        $passCount = ($adapterResults | Where-Object { $_.Status -eq 'PASS' }).Count
        $failCount = ($adapterResults | Where-Object { $_.Status -eq 'FAIL' }).Count
        $warnCount = ($adapterResults | Where-Object { $_.Status -eq 'WARN' }).Count
        $infoCount = ($adapterResults | Where-Object { $_.Status -eq 'INFO' }).Count

        Write-Host "  PASS=$passCount  FAIL=$failCount  WARN=$warnCount  INFO=$infoCount" -ForegroundColor White
        Write-Host ""

        # Print FAIL items first (most important)
        $sortedResults = $adapterResults | Sort-Object @{E={switch($_.Status){'FAIL'{0};'WARN'{1};'INFO'{2};'PASS'{3};default{4}}}}
        foreach ($r in $sortedResults) {
            $color = switch ($r.Status) {
                'FAIL'  { 'Red' }
                'WARN'  { 'Magenta' }
                'PASS'  { 'Green' }
                'INFO'  { 'Gray' }
                default { 'White' }
            }
            $flag = switch ($r.Status) {
                'FAIL'  { '!!' }
                'WARN'  { '? ' }
                'PASS'  { 'OK' }
                'INFO'  { '  ' }
                default { '  ' }
            }
            Write-Host "    [$flag] $($r.Setting.PadRight(35)) : $($r.Current)" -ForegroundColor $color
        }
    }
}

# ============================================================================
# Main
# ============================================================================

function Main {
    Write-Banner "P3 NIC Driver Settings Audit"

    # Check admin
    $isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if (-not $isAdmin) {
        Write-Warn "Not running as Administrator. Some properties may be unavailable."
        Write-Info "Re-run as Administrator for complete audit."
        Write-Host ""
    }

    # Discover adapters
    try {
        $adapters = Get-NetAdapter -Name $AdapterName -ErrorAction SilentlyContinue |
            Where-Object { $_.Status -eq 'Up' -or $_.Status -eq 'Disconnected' }
    } catch {
        Write-Fail "Failed to enumerate network adapters: $_"
        Write-Info "Ensure you are running as Administrator for full access."
        return
    }

    if (-not $adapters) {
        Write-Warn "No matching adapters found for pattern: $AdapterName"
        return
    }

    Write-Info "Found $($adapters.Count) adapter(s)"
    foreach ($a in $adapters) {
        $isEth = Test-IsEthercatAdapter -Adapter $a
        $tag = if ($isEth) { "[Ethernet]" } else { "[Other]" }
        Write-Info "  $tag $($a.Name) : $($a.InterfaceDescription)"
    }
    Write-Host ""

    # Audit each adapter
    $allResults = @()
    foreach ($adapter in $adapters) {
        $results = Invoke-NicAudit -Adapter $adapter
        if ($results) {
            $allResults += $results
        }
    }

    # Display results
    Format-AuditTable -Results $allResults

    # Summary
    Write-Host ""
    Write-Banner "Audit Summary"
    $totalFail = ($allResults | Where-Object { $_.Status -eq 'FAIL' }).Count
    $totalWarn = ($allResults | Where-Object { $_.Status -eq 'WARN' }).Count
    $totalPass = ($allResults | Where-Object { $_.Status -eq 'PASS' }).Count

    Write-Host "  FAIL: $totalFail  WARN: $totalWarn  PASS: $totalPass" -ForegroundColor White
    Write-Host ""

    if ($totalFail -gt 0) {
        Write-Warn "The following settings do NOT match EtherCAT recommendations:"
        $allResults | Where-Object { $_.Status -eq 'FAIL' } | ForEach-Object {
            Write-Host "    - $($_.Adapter): $($_.Setting) = $($_.Current) (expected: $($_.Expected))" -ForegroundColor Red
        }
    } else {
        Write-OK "All checked settings match EtherCAT recommendations."
    }

    # Save to file
    if ($OutputFile -or $JsonOutput) {
        $timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
        $txtPath = if ($OutputFile) { $OutputFile } else { Join-Path $OutputDir "nic_audit_${timestamp}.txt" }
        $jsonPath = Join-Path $OutputDir "nic_audit_${timestamp}.json"

        # Text output
        $lines = @()
        $lines += "=== WuBigCat P3 NIC Driver Audit ==="
        $lines += "Timestamp: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
        $lines += "Hostname: $env:COMPUTERNAME"
        $lines += "OS: $([System.Environment]::OSVersion.VersionString)"
        $lines += ""
        foreach ($r in ($allResults | Sort-Object Adapter, Category, Setting)) {
            $lines += "[$($r.Status)] $($r.Adapter) | $($r.Category) | $($r.Setting) = $($r.Current) (expected: $($r.Expected))"
        }
        $utf8 = New-Object System.Text.UTF8Encoding($false)
        [System.IO.File]::WriteAllLines($txtPath, $lines, $utf8)
        Write-OK "Text report saved: $txtPath"

        # JSON output
        if ($JsonOutput) {
            $json = $allResults | Select-Object Adapter, Category, Setting, Current, Expected, Status |
                Sort-Object Adapter, Category, Setting | ConvertTo-Json -Depth 3
            [System.IO.File]::WriteAllText($jsonPath, $json, $utf8)
            Write-OK "JSON report saved: $jsonPath"
        }
    }

    # Apply recommendations if requested
    if ($Apply -or $WhatIf) {
        Write-Host ""
        Write-Banner "Apply Recommended Settings"
        $mode = if ($WhatIf) { "WhatIf (preview only)" } else { "APPLY" }
        Write-Warn "Mode: $mode"

        $failItems = $allResults | Where-Object { $_.Status -eq 'FAIL' }
        if ($failItems.Count -eq 0) {
            Write-OK "No FAIL items to fix."
            return
        }

        Write-Info "$($failItems.Count) item(s) to fix"
        foreach ($item in ($failItems | Sort-Object Adapter -Unique | Select-Object Adapter -ExpandProperty Adapter)) {
            $adapterFails = $failItems | Where-Object { $_.Adapter -eq $item }
            foreach ($af in $adapterFails) {
                # Map setting to Set-NetAdapterAdvancedProperty keyword
                $regKeyword = $null
                try {
                    $ap = Get-NetAdapterAdvancedProperty -Name $af.Adapter -DisplayName $af.Setting -ErrorAction SilentlyContinue
                    if ($ap) { $regKeyword = $ap.RegistryKeyword }
                } catch {}

                if ($regKeyword) {
                    $desiredValue = switch -Wildcard ($af.Expected) {
                        'Disable*' { 'Disabled' }
                        'Enable*'  { 'Enabled' }
                        default    { $af.Expected }
                    }
                    Write-Info "Would set: $($af.Adapter) '$($af.Setting)' = $desiredValue (keyword: $regKeyword)"
                    if ($Apply -and -not $WhatIf) {
                        try {
                            Set-NetAdapterAdvancedProperty -Name $af.Adapter -RegistryKeyword $regKeyword `
                                -RegistryValue $desiredValue -ErrorAction Stop
                            Write-OK "Applied: $($af.Setting) = $desiredValue"
                        } catch {
                            Write-Fail "Failed to set $($af.Setting): $_"
                        }
                    }
                } else {
                    Write-Warn "Cannot find registry keyword for: $($af.Setting) (driver may not expose this as settable)"
                }
            }
        }
    }
}

Main
