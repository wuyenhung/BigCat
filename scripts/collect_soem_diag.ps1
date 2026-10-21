#Requires -Version 5.1
<#
.SYNOPSIS
    Collect SOEM/EtherCAT-relevant system diagnostics on Win10/Win11.
    Outputs a log file for comparing Win11 vs Win10 network performance.

.DESCRIPTION
    Collects:
      - OS version, build, VBS/HVCI status
      - Npcap version and driver mode
      - NIC hardware, driver, advanced properties
      - IRQ / MSI-X affinity and routing
      - CPU topology (P-core / E-core detection)
      - Power plan and related settings
      - Network throttling, RSS, timer resolution
      - NIC energy settings (EEE, Green Ethernet, Power Mgmt)

.PARAMETER OutputDir
    Output directory, default: baseline_results\etw\

.PARAMETER AdapterFilter
    NIC name filter (supports wildcard). Default: all active Ethernet adapters.

.EXAMPLE
    .\scripts\collect_soem_diag.ps1
    .\scripts\collect_soem_diag.ps1 -AdapterFilter "Intel*"
#>

param(
    [string]$OutputDir = $null,
    [string]$AdapterFilter = "*"
)

$ErrorActionPreference = "Continue"
$script:ScriptRoot = $PSScriptRoot
$script:ProjectRoot = Resolve-Path (Join-Path $ScriptRoot "..")

if (-not $OutputDir) {
    $OutputDir = Join-Path $ProjectRoot "baseline_results\etw"
}
if (-not (Test-Path $OutputDir)) {
    New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
}

$timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
$computerName = $env:COMPUTERNAME
$logFile = Join-Path $OutputDir "soem_diag_${computerName}_${timestamp}.log"

# ============================================================================
# Output helpers
# ============================================================================

$script:LogStream = [System.IO.StreamWriter]::new($logFile, $false, [System.Text.Encoding]::UTF8)

function Write-Log {
    param([string]$Text)
    $line = "[{0:yyyy-MM-dd HH:mm:ss}] {1}" -f (Get-Date), $Text
    $script:LogStream.WriteLine($line)
    Write-Host $line
}

function Write-Section {
    param([string]$Title)
    $sep = "=" * 72
    $script:LogStream.WriteLine("")
    $script:LogStream.WriteLine($sep)
    $script:LogStream.WriteLine("  $Title")
    $script:LogStream.WriteLine($sep)
    $script:LogStream.WriteLine("")
    Write-Host ""
    Write-Host $sep -ForegroundColor Cyan
    Write-Host "  $Title" -ForegroundColor Cyan
    Write-Host $sep -ForegroundColor Cyan
    Write-Host ""
}

function Write-KVP {
    param([string]$Key, [string]$Value, [string]$Status = "")
    $statusStr = if ($Status) { "  [$Status]" } else { "" }
    $line = "  {0,-40} : {1}{2}" -f $Key, $Value, $statusStr
    $script:LogStream.WriteLine($line)
    switch ($Status) {
        "OK"   { Write-Host $line -ForegroundColor Green }
        "WARN" { Write-Host $line -ForegroundColor Yellow }
        "FAIL" { Write-Host $line -ForegroundColor Red }
        default { Write-Host $line }
    }
}

# ============================================================================
# Section 1: OS Info
# ============================================================================

Write-Section "1. Operating System"

$os = Get-CimInstance Win32_OperatingSystem
$cs = Get-CimInstance Win32_ComputerSystem

Write-KVP "OS Name"       $os.Caption
Write-KVP "Version"       $os.Version
Write-KVP "Build"         $os.BuildNumber
Write-KVP "Architecture"  $os.OSArchitecture
Write-KVP "Install Date"  $os.InstallDate
Write-KVP "Last Boot"     $os.LastBootUpTime
Write-KVP "Uptime (days)" ([math]::Round(((Get-Date) - $os.LastBootUpTime).TotalDays, 2))
Write-KVP "Total RAM (GB)" ([math]::Round($cs.TotalPhysicalMemory / 1GB, 1))
Write-KVP "System Model"  "$($cs.Manufacturer) $($cs.Model)"

$isWin11 = $os.BuildNumber -ge 22000
$winGenStr = if ($isWin11) { "Windows 11 (Build >= 22000)" } else { "Windows 10" }
$statusWinGen = if ($isWin11) { "WARN" } else { "OK" }
Write-KVP "Detected Gen" $winGenStr $statusWinGen

# ============================================================================
# Section 2: VBS / HVCI (Win11 Critical)
# ============================================================================

Write-Section "2. VBS / HVCI Virtualization Security"

$vbsRunning = $false
$hvciRunning = $false

try {
    $vbsReg = Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Control\DeviceGuard" -ErrorAction SilentlyContinue
    if ($vbsReg) {
        Write-KVP "DeviceGuard: EnableVirtualizationBasedSecurity" "$($vbsReg.EnableVirtualizationBasedSecurity)"
        Write-KVP "DeviceGuard: RequirePlatformSecurityFeatures" "$($vbsReg.RequirePlatformSecurityFeatures)"
        if ($vbsReg.EnableVirtualizationBasedSecurity -eq 1) { $vbsRunning = $true }
    }
} catch { Write-KVP "DeviceGuard Registry" "Not found" }

try {
    $hvciReg = Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity" -ErrorAction SilentlyContinue
    if ($hvciReg) {
        Write-KVP "HVCI: Enabled" "$($hvciReg.Enabled)"
        if ($hvciReg.Enabled -eq 1) { $hvciRunning = $true }
    }
} catch { Write-KVP "HVCI Registry" "Not found" }

try {
    $msinfoPath = "$env:TEMP\msinfo_soem_diag.txt"
    & msinfo32 /report $msinfoPath /categories +SystemSummary 2>&1 | Out-Null
    Start-Sleep -Seconds 3
    if (Test-Path $msinfoPath) {
        $c = Get-Content $msinfoPath -Raw -ErrorAction SilentlyContinue
        if ($c -match "Virtualization-based security\s+(Running|Not enabled)") {
            Write-KVP "VBS (msinfo32)" $Matches[1] $(if ($Matches[1] -eq "Running") { "FAIL" } else { "OK" })
        }
        if ($c -match "Hypervisor-protected Code Integrity\s+(Running|Not enabled)") {
            Write-KVP "HVCI (msinfo32)" $Matches[1] $(if ($Matches[1] -eq "Running") { "FAIL" } else { "OK" })
        }
        Remove-Item $msinfoPath -Force -ErrorAction SilentlyContinue
    }
} catch { }

try {
    $hyperv = Get-WindowsOptionalFeature -Online -FeatureName Microsoft-Hyper-V-Hypervisor -ErrorAction SilentlyContinue
    if ($hyperv) { Write-KVP "Hyper-V Hypervisor" $hyperv.State }
} catch { }

$vbsStatus = if ($vbsRunning -or $hvciRunning) { "FAIL" } else { "OK" }
Write-KVP ">>> VBS/HVCI Summary" $(if ($vbsRunning -or $hvciRunning) { "ACTIVE - may cause latency" } else { "Not active" }) $vbsStatus

# ============================================================================
# Section 3: Npcap
# ============================================================================

Write-Section "3. Npcap Installation"

try {
    $npcapWmi = Get-WmiObject Win32_Product -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like "*Npcap*" }
    if ($npcapWmi) {
        foreach ($np in $npcapWmi) {
            Write-KVP "Product Name" $np.Name
            Write-KVP "Version" $np.Version
            Write-KVP "Install Date" $np.InstallDate
            Write-KVP "Vendor" $np.Vendor
        }
    } else {
        Write-KVP "Npcap (WMI)" "Not found in Win32_Product"
    }
} catch {
    Write-KVP "Npcap (WMI)" "Query failed"
}

$npcapPaths = @(
    "$env:SystemRoot\System32\Npcap\wpcap.dll",
    "$env:SystemRoot\SysWOW64\Npcap\wpcap.dll",
    "$env:SystemRoot\System32\wpcap.dll",
    "$env:SystemRoot\SysWOW64\wpcap.dll"
)
foreach ($p in $npcapPaths) {
    if (Test-Path $p) {
        $ver = (Get-Item $p).VersionInfo
        Write-KVP "DLL: $p" "$($ver.FileVersion)"
    }
}

$npcapService = Get-Service -Name "npcap" -ErrorAction SilentlyContinue
if ($npcapService) {
    Write-KVP "Npcap Service" "$($npcapService.Status) [$($npcapService.StartType)]"
}

try {
    $npcapReg = Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\NpcapInst" -ErrorAction SilentlyContinue
    if ($npcapReg) {
        Write-KVP "Npcap DisplayVersion" "$($npcapReg.DisplayVersion)"
    }
} catch { }

$npcapLoop = Get-NetAdapter -Name "*Npcap*" -ErrorAction SilentlyContinue
if ($npcapLoop) {
    Write-KVP "Npcap Loopback Adapter" "$($npcapLoop.Name) [$($npcapLoop.Status)]"
}

# ============================================================================
# Section 4: CPU Topology
# ============================================================================

Write-Section "4. CPU Topology"

$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
Write-KVP "CPU Name"        $cpu.Name
Write-KVP "Cores"           $cpu.NumberOfCores
Write-KVP "Logical CPUs"    $cpu.NumberOfLogicalProcessors
Write-KVP "Max Clock (MHz)" $cpu.MaxClockSpeed

$isHybrid = $false
if ($cpu.Name -match "(\d+)(\d{2})[a-zA-Z]") {
    $gen = [int]$Matches[1]
    if ($gen -ge 12) { $isHybrid = $true }
}
Write-KVP "Hybrid Arch (P/E cores)" $(if ($isHybrid) { "Likely YES" } else { "Not detected" }) $(if ($isHybrid) { "WARN" })

$totalLps = $cpu.NumberOfLogicalProcessors
Write-KVP "Total Logical CPUs" $totalLps $(if ($totalLps -gt 64) { "WARN" } else { "OK" })

# ============================================================================
# Section 5: Network Adapters
# ============================================================================

Write-Section "5. Network Adapter Hardware & Driver"

$adapters = Get-NetAdapter -Physical -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -like $AdapterFilter }

if (-not $adapters) {
    $adapters = Get-NetAdapter -ErrorAction SilentlyContinue
}

foreach ($adapter in $adapters) {
    Write-Log ""
    Write-Log "  -- $($adapter.Name) --"
    Write-KVP "Description"       $adapter.InterfaceDescription
    Write-KVP "Status"            $adapter.Status
    Write-KVP "Link Speed"        "$($adapter.LinkSpeed)"
    Write-KVP "MAC"               $adapter.MacAddress
    Write-KVP "Driver Version"    $adapter.DriverVersion
    Write-KVP "Driver Date"       $adapter.DriverDate
    Write-KVP "NDIS Version"      $adapter.NdisVersion
    Write-KVP "MTU"               $adapter.MtuSize
    Write-KVP "PnP DeviceID"      "$($adapter.PnPDeviceID)"

    try {
        $hw = Get-NetAdapterHardwareInfo -Name $adapter.Name -ErrorAction SilentlyContinue
        if ($hw) {
            Write-KVP "HW: Bus Type"          $hw.BusType
            Write-KVP "HW: NumaNode"          $hw.NumaNode
            Write-KVP "HW: InterruptMsgCount" $hw.InterruptMessageCount
            Write-KVP "HW: PciVenDevSubsys"   $hw.PciVenDevSubsys
        }
    } catch { }

    try {
        $pm = Get-NetAdapterPowerManagement -Name $adapter.Name -ErrorAction SilentlyContinue
        if ($pm) {
            Write-KVP "PM: AllowTurnOff"     $pm.AllowComputerToTurnOffDevice
            Write-KVP "PM: SelectiveSuspend" $pm.SelectiveSuspend
            $pmBad = $pm.AllowComputerToTurnOffDevice -eq 'Enabled'
            if ($pmBad) { Write-KVP ">>> PM" "NIC may sleep!" "FAIL" }
        }
    } catch { }

    try {
        $rss = Get-NetAdapterRss -Name $adapter.Name -ErrorAction SilentlyContinue
        if ($rss) {
            Write-KVP "RSS: Enabled"       $rss.Enabled
            Write-KVP "RSS: Queues"        $rss.NumberOfReceiveQueues
            Write-KVP "RSS: BaseProcessor" $rss.BaseProcessorNumber
            $rssBad = $rss.Enabled
            if ($rssBad) { Write-KVP ">>> RSS" "Enabled - may cause jitter" "WARN" }
        }
    } catch { }
}

# ============================================================================
# Section 6: NIC Advanced Properties
# ============================================================================

Write-Section "6. NIC Advanced Properties"

foreach ($adapter in $adapters) {
    Write-Log ""
    Write-Log "  -- $($adapter.Name) --"
    try {
        $props = Get-NetAdapterAdvancedProperty -Name $adapter.Name -ErrorAction SilentlyContinue
        if (-not $props) {
            Write-Log "  (no advanced properties)"
            continue
        }
        foreach ($p in $props) {
            $dv = if ([string]::IsNullOrWhiteSpace($p.DisplayValue)) { "(empty)" } else { $p.DisplayValue }
            $status = ""
            if ($p.DisplayName -match "Interrupt Moderation" -and $dv -notmatch "Disabled|Off|Disable") { $status = "FAIL" }
            elseif ($p.DisplayName -match "EEE|Energy Efficient|Green" -and $dv -notmatch "Disabled|Off|Disable") { $status = "FAIL" }
            elseif ($p.DisplayName -match "Flow Control" -and $dv -notmatch "Disabled|Off|Disable") { $status = "FAIL" }
            elseif ($p.DisplayName -match "ReduceSpeedOnPowerDown" -and $dv -notmatch "Disabled|Off|Disable") { $status = "FAIL" }
            Write-KVP $p.DisplayName $dv $status
        }
    } catch {
        Write-KVP "Error" $_.Exception.Message "FAIL"
    }
}

# ============================================================================
# Section 7: MSI/MSI-X IRQ Routing
# ============================================================================

Write-Section "7. MSI / MSI-X IRQ Routing"

Write-Log ""
Write-Log "  Interrupt Message Count per adapter:"
foreach ($adapter in $adapters) {
    try {
        $hw = Get-NetAdapterHardwareInfo -Name $adapter.Name -ErrorAction SilentlyContinue
        if ($hw) {
            Write-KVP $adapter.Name "InterruptMsgs=$($hw.InterruptMessageCount)"
        }
    } catch { }
}

Write-Log ""
Write-Log "  Registry MSI/MSI-X Affinity Policy (PCI devices):"
try {
    $enumPath = "HKLM:\SYSTEM\CurrentControlSet\Enum\PCI"
    $pciDevices = Get-ChildItem $enumPath -ErrorAction SilentlyContinue
    foreach ($dev in $pciDevices) {
        $subDevs = Get-ChildItem $dev.PSPath -ErrorAction SilentlyContinue
        foreach ($sd in $subDevs) {
            $affPath = Join-Path $sd.PSPath "Device Parameters\Interrupt Management\Affinity Policy"
            if (Test-Path $affPath) {
                $affProps = Get-ItemProperty $affPath -ErrorAction SilentlyContinue
                $devDesc = try {
                    (Get-ItemProperty $sd.PSPath -Name DeviceDesc -ErrorAction SilentlyContinue).DeviceDesc
                } catch { "Unknown" }
                Write-Log "  $devDesc"
                Write-Log "    DevicePolicy=$($affProps.DevicePolicy)  AssignmentSetOverride=$($affProps.AssignmentSetOverride)"
            }
        }
    }
} catch {
    Write-KVP "Registry IRQ" "Enumeration failed (need Admin)"
}

# ============================================================================
# Section 8: Power Plan
# ============================================================================

Write-Section "8. Power Plan & Performance"

try {
    $activePlan = Get-CimInstance -Namespace "root\cimv2\power" -ClassName Win32_PowerPlan -ErrorAction SilentlyContinue |
        Where-Object { $_.IsActive -eq $true }
    if ($activePlan) {
        Write-KVP "Active Plan" $activePlan.ElementName
        Write-KVP "Plan GUID"   $activePlan.InstanceID
        $planOk = $activePlan.ElementName -match "Ultimate|High performance"
        Write-KVP ">>> Plan Rating" $(if ($planOk) { "Good" } else { "NOT optimal" }) $(if ($planOk) { "OK" } else { "FAIL" })
    }
} catch {
    Write-KVP "Power Plan" "WMI query failed"
}

try {
    $planList = & powercfg /list 2>&1
    Write-Log ""
    Write-Log "  powercfg /list:"
    foreach ($line in $planList) { Write-Log "    $line" }
} catch { }

# ============================================================================
# Section 9: Timer Resolution & Network Throttling
# ============================================================================

Write-Section "9. Timer Resolution & Network Throttling"

try {
    $mmcss = Get-Service -Name "MMCSS" -ErrorAction SilentlyContinue
    if ($mmcss) {
        Write-KVP "MMCSS Service" "$($mmcss.Status) [$($mmcss.StartType)]"
    }
} catch { }

try {
    $nti = Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Multimedia\SystemProfile" -Name "NetworkThrottlingIndex" -ErrorAction SilentlyContinue
    if ($nti) {
        $ntiVal = $nti.NetworkThrottlingIndex
        $ntiOk = $ntiVal -eq 0xFFFFFFFF
        Write-KVP "NetworkThrottlingIndex" "0x{0:X8}" -f $ntiVal
        if (-not $ntiOk) { Write-KVP ">>> Throttling" "NOT set to FFFFFFFF" "FAIL" }
    } else {
        Write-KVP "NetworkThrottlingIndex" "Not set (use default)" "WARN"
    }
} catch {
    Write-KVP "NetworkThrottlingIndex" "Registry query failed"
}

try {
    $sysResp = Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Multimedia\SystemProfile" -Name "SystemResponsiveness" -ErrorAction SilentlyContinue
    if ($sysResp) { Write-KVP "SystemResponsiveness" $sysResp.SystemResponsiveness }
} catch { }

# ============================================================================
# Section 10: NIC Energy Settings
# ============================================================================

Write-Section "10. NIC Energy Saving Details"

foreach ($adapter in $adapters) {
    try {
        $props = Get-NetAdapterAdvancedProperty -Name $adapter.Name -ErrorAction SilentlyContinue
        $eeeProps = $props | Where-Object {
            $_.DisplayName -match "EEE|Energy Efficient|Green Ethernet|Power Saving|ReduceSpeedOnPowerDown|Auto Disable Gigabit"
        }
        if ($eeeProps) {
            Write-Log ""
            Write-Log "  $($adapter.Name):"
            foreach ($ep in $eeeProps) {
                $ok = $ep.DisplayValue -match "Disabled|Off|Disable"
                Write-KVP $ep.DisplayName $ep.DisplayValue $(if ($ok) { "OK" } else { "FAIL" })
            }
        }
    } catch { }
}

# ============================================================================
# Section 11: NDIS & Network Stack
# ============================================================================

Write-Section "11. NDIS & Network Stack"

try {
    $ndis = Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Services\NDIS\Parameters" -ErrorAction SilentlyContinue
    if ($ndis) {
        Write-KVP "NDIS Version" "$($ndis.MajorVersion).$($ndis.MinorVersion)"
    }
} catch { }

foreach ($adapter in $adapters) {
    Write-Log ""
    Write-Log "  $($adapter.Name) - Bindings:"
    try {
        $bindings = Get-NetAdapterBinding -Name $adapter.Name -ErrorAction SilentlyContinue
        if ($bindings) {
            foreach ($b in $bindings) {
                $s = ""
                if ($b.Enabled -and $b.DisplayName -match "QoS|Packet Scheduler|VMQ") { $s = "WARN" }
                Write-KVP $b.DisplayName "$($b.Enabled)" $s
            }
        }
    } catch { Write-KVP "Bindings" "Query failed" }
}

try {
    $tcp = Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Services\Tcpip\Parameters" -ErrorAction SilentlyContinue
    Write-KVP "TCP EnableRSS"          "$($tcp.EnableRSS)"
    Write-KVP "TCP DisableTaskOffload" "$($tcp.DisableTaskOffload)"
} catch { }

# ============================================================================
# Section 12: NIC Statistics Snapshot
# ============================================================================

Write-Section "12. NIC Statistics Snapshot"

if ($adapters) {
    try {
        $stats = Get-NetAdapterStatistics -Name $adapters[0].Name -ErrorAction SilentlyContinue
        if ($stats) {
            Write-KVP "Rx Packets"   $stats.ReceivedPackets
            Write-KVP "Tx Packets"   $stats.SentPackets
            Write-KVP "Rx Discarded" $stats.ReceivedPacketsDiscarded
            Write-KVP "Rx Errors"    $stats.ReceivedPacketErrors
        }
    } catch { Write-KVP "Stats" "Not available" }
}

# ============================================================================
# Section 13: Interfering Processes
# ============================================================================

Write-Section "13. Potentially Interfering Processes"

$interfering = @('*wireshark*','*tshark*','*dumpcap*','*twin*cat*','*hyper*v*','*vmwp*','*docker*','*vpn*','*defender*','*msmpeng*','*antimalware*')

try {
    $allProcs = Get-Process -ErrorAction SilentlyContinue
    $found = $false
    foreach ($pat in $interfering) {
        foreach ($m in ($allProcs | Where-Object { $_.ProcessName -like $pat })) {
            $found = $true
            Write-KVP "Process" "$($m.ProcessName) PID=$($m.Id)" "WARN"
        }
    }
    if (-not $found) { Write-KVP "Interfering processes" "None detected" "OK" }
} catch { Write-KVP "Process scan" "Failed" }

# ============================================================================
# Section 14: Summary
# ============================================================================

Write-Section "14. Diagnostic Summary"

$issues = @()
$warnings = @()

if ($vbsRunning)  { $issues += "VBS ACTIVE - adds hypervisor latency. Run: bcdedit /set hypervisorlaunchtype off" }
if ($hvciRunning) { $issues += "HVCI ACTIVE - kernel code runs in VM. Disable in Windows Security" }
if ($isWin11)     { $warnings += "Win11 detected - ensure Npcap runtime >= 1.79" }
if ($isHybrid)    { $warnings += "Hybrid CPU - pin EtherCAT NIC IRQ to P-core only" }

if ($issues.Count -eq 0 -and $warnings.Count -eq 0) {
    Write-KVP "Result" "No critical issues detected" "OK"
} else {
    foreach ($i in $issues)   { Write-KVP "CRITICAL" $i "FAIL" }
    foreach ($w in $warnings) { Write-KVP "WARNING" $w "WARN" }
}

Write-Log ""
Write-Log "  Win11 SOEM Optimization Checklist:"
Write-Log "    1. Disable VBS/HVCI: bcdedit /set hypervisorlaunchtype off (reboot)"
Write-Log "    2. Upgrade Npcap runtime to latest (1.88)"
Write-Log "    3. Disable Network Throttling: NetworkThrottlingIndex=0xFFFFFFFF"
Write-Log "    4. Set Power Plan to Ultimate Performance"
Write-Log "    5. NIC Interrupt Moderation = Disabled"
Write-Log "    6. NIC RSS = Disabled for EtherCAT adapter"
Write-Log "    7. NIC EEE / Green Ethernet = Disabled"
Write-Log "    8. Pin NIC IRQ to P-core (not E-core) for hybrid CPUs"

# ============================================================================
# Finalize
# ============================================================================

$script:LogStream.WriteLine("")
$script:LogStream.WriteLine("=" * 72)
$script:LogStream.WriteLine("  Complete: $timestamp")
$script:LogStream.WriteLine("  Log: $logFile")
$script:LogStream.WriteLine("=" * 72)
$script:LogStream.Close()

Write-Host ""
Write-Host ("=" * 72) -ForegroundColor Green
Write-Host "  Diagnostic collection complete" -ForegroundColor Green
Write-Host "  Log file: $logFile" -ForegroundColor Green
Write-Host ("=" * 72) -ForegroundColor Green
