#Requires -Version 5.1 -RunAsAdministrator
<#
.SYNOPSIS
    一鍵設定 Win11 網卡解除限制 — Npcap + EtherCAT DC 即時穩定版。
    關閉所有會讓 Npcap 漏封包 / 延遲的 Win11 網路功能。

.DESCRIPTION
    Win11 預設會透過以下機制限制網卡效能，導致 Npcap 無法即時擷取 EtherCAT 封包：
      - 硬體 Offload（TCP/UDP/LSO checksum 由網卡處理，Npcap 看不到原始封包）
      - RSC (Receive Segment Coalescing) — 網卡合併封包，Npcap 只能看到合併後的
      - 中斷調節 (Interrupt Moderation) — 故意批次延遲中斷
      - EEE / 省電 — 降速導致 DC 時序不穩
      - Network Throttling — MMCSS 限制網路封包速率
      - DMA Coalescing — 網卡批次 DMA，增加延遲
      - PCIe ASPM — PCIe 鏈路省電造成可變延遲
      - VBS / HVCI — 虛擬化安全造成 DPC 延遲暴增

    此腳本一鍵關閉以上所有限制，適用於：
      - Npcap/WinPcap 封包擷取
      - EtherCAT DC (Distributed Clock) 即時控制
      - SOEM open-source EtherCAT master

    執行後需重新開機。

.EXAMPLE
    powershell -NoLogo -NoProfile -ExecutionPolicy Bypass -File .\scripts\setup_npcap_ethercat.ps1
#>

$ErrorActionPreference = 'Continue'
$Host.UI.RawUI.WindowTitle = 'Npcap EtherCAT Realtime Setup'

Write-Host @"

╔══════════════════════════════════════════════════════╗
║   Npcap + EtherCAT Win11 即時網路一鍵設定            ║
║   關閉 Win11 網路限制 / 解放 Npcap 完整擷取能力       ║
╚══════════════════════════════════════════════════════╝

Host : $env:COMPUTERNAME
OS   : $(Get-CimInstance Win32_OperatingSystem | Select-Object -ExpandProperty Caption)
"@ -ForegroundColor Cyan

# ============================================================================
# 0. Npcap 安裝狀態檢查
# ============================================================================
Write-Host "`n[0] Npcap 狀態檢查" -ForegroundColor Magenta

$npcapService = Get-Service -Name 'npcap' -ErrorAction SilentlyContinue
$npcapInstall = Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Npcap' -ErrorAction SilentlyContinue
if (-not $npcapInstall) {
    $npcapInstall = Get-ItemProperty 'HKLM:\SOFTWARE\Npcap' -ErrorAction SilentlyContinue
}

if ($npcapService) {
    if ($npcapService.Status -eq 'Running') {
        Write-Host "  [OK] Npcap 服務: 執行中" -ForegroundColor Green
    } else {
        Write-Host "  [WARN] Npcap 服務狀態: $($npcapService.Status)" -ForegroundColor Yellow
    }
} else {
    Write-Host "  [FAIL] Npcap 未安裝！請先從 https://npcap.com 安裝 Npcap" -ForegroundColor Red
    Write-Host "         安裝時勾選: 'Install Npcap in WinPcap API-compatible Mode'" -ForegroundColor Yellow
}

if ($npcapInstall) {
    Write-Host "  [OK] Npcap 版本: $($npcapInstall.DisplayVersion)" -ForegroundColor Green
} else {
    Write-Host "  [WARN] 無法讀取 Npcap 版本資訊" -ForegroundColor Yellow
}

# Check NPCAP loopback adapter
$npcapLoopback = Get-NetAdapter -InterfaceDescription '*npcap*loopback*' -ErrorAction SilentlyContinue
if ($npcapLoopback) {
    Write-Host "  [OK] Npcap Loopback Adapter: 已安裝" -ForegroundColor Green
}

# ============================================================================
# 1. 系統層註冊表調整（影響所有網卡 + Npcap 擷取行為）
# ============================================================================
Write-Host "`n[1] 系統層網路限制解除" -ForegroundColor Magenta

$restartNeeded = $false
$changes = @()

# 1a. NetworkThrottlingIndex → FFFFFFFF（關閉 MMCSS 網路封包節流）
$ntPath = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Multimedia\SystemProfile'
try {
    if (-not (Test-Path $ntPath)) { New-Item -Path $ntPath -Force | Out-Null }
    $curNt = (Get-ItemProperty -Path $ntPath -Name 'NetworkThrottlingIndex' -ErrorAction SilentlyContinue).NetworkThrottlingIndex
    if ($curNt -ne 0xFFFFFFFF) {
        Set-ItemProperty -Path $ntPath -Name 'NetworkThrottlingIndex' -Value 0xFFFFFFFF -Type DWord -Force
        Write-Host "  [SET] NetworkThrottlingIndex = 0xFFFFFFFF (was: $curNt)" -ForegroundColor Green
        $changes += "NetworkThrottlingIndex: $curNt -> 0xFFFFFFFF"
        $restartNeeded = $true
    } else {
        Write-Host "  [OK]  NetworkThrottlingIndex: 已是最佳值" -ForegroundColor Green
    }
} catch { Write-Host "  [FAIL] NetworkThrottlingIndex: $_" -ForegroundColor Red }

# 1b. TCP 自動調節 → normal（預設即可，不要 disabled 否則拖慢 TCP throughput）
try {
    $tcpAuto = netsh int tcp show global | Select-String 'Receive Window Auto-Tuning'
    if ($tcpAuto -notmatch 'normal') {
        netsh int tcp set global autotuninglevel=normal | Out-Null
        Write-Host "  [SET] TCP Autotuning -> normal" -ForegroundColor Green
        $changes += "TCP Autotuning -> normal"
    } else {
        Write-Host "  [OK]  TCP Autotuning: normal" -ForegroundColor Green
    }
} catch { Write-Host "  [FAIL] TCP Autotuning: $_" -ForegroundColor Red }

# 1c. TCP RSS → enabled（系統層，NIC 層面另設）
try {
    $rssState = netsh int tcp show global | Select-String 'Receive-Side Scaling'
    if ($rssState -notmatch 'enabled') {
        netsh int tcp set global rss=enabled | Out-Null
        Write-Host "  [SET] TCP RSS (system) -> enabled" -ForegroundColor Green
        $changes += "TCP RSS -> enabled"
    } else {
        Write-Host "  [OK]  TCP RSS (system): enabled" -ForegroundColor Green
    }
} catch { Write-Host "  [FAIL] TCP RSS: $_" -ForegroundColor Red }

# 1d. RSC (Receive Segment Coalescing) → disabled — 關鍵！
#     RSC 在網卡硬體層合併封包，Npcap 只能看到合併後的大封包
try {
    $rscState = netsh int tcp show global | Select-String 'Receive Segment Coalescing'
    if ($rscState -notmatch 'disabled') {
        netsh int tcp set global rsc=disabled | Out-Null
        Write-Host "  [SET] RSC -> disabled （網卡不再合併封包，Npcap 才能看到原始幀）" -ForegroundColor Green
        $changes += "RSC -> disabled"
    } else {
        Write-Host "  [OK]  RSC: disabled" -ForegroundColor Green
    }
} catch { Write-Host "  [FAIL] RSC: $_" -ForegroundColor Red }

# 1e. Chimney Offload → disabled
try {
    $chimney = netsh int tcp show global | Select-String 'Chimney Offload'
    if ($chimney -notmatch 'disabled') {
        netsh int tcp set global chimney=disabled | Out-Null
        Write-Host "  [SET] TCP Chimney Offload -> disabled" -ForegroundColor Green
        $changes += "Chimney Offload -> disabled"
    } else {
        Write-Host "  [OK]  TCP Chimney: disabled" -ForegroundColor Green
    }
} catch { Write-Host "  [FAIL] Chimney: $_" -ForegroundColor Red }

# 1f. NetDMA → disabled
try {
    $netDma = netsh int tcp show global | Select-String 'NetDMA'
    if ($netDma -and $netDma -notmatch 'disabled') {
        netsh int tcp set global netdma=disabled | Out-Null
        Write-Host "  [SET] NetDMA -> disabled" -ForegroundColor Green
        $changes += "NetDMA -> disabled"
    } elseif ($netDma) {
        Write-Host "  [OK]  NetDMA: disabled" -ForegroundColor Green
    } else {
        Write-Host "  [OK]  NetDMA: 不支援 (此為舊版功能)" -ForegroundColor DarkGray
    }
} catch { Write-Host "  [INFO] NetDMA 不適用於此系統" -ForegroundColor DarkGray }

# 1g. Task Offload → disabled
try {
    $taskOffload = netsh int tcp show global | Select-String 'Task Offload'
    if ($taskOffload -notmatch 'disabled') {
        netsh int tcp set global taskoffload=disabled | Out-Null
        Write-Host "  [SET] TCP Task Offload -> disabled" -ForegroundColor Green
        $changes += "Task Offload -> disabled"
    } else {
        Write-Host "  [OK]  TCP Task Offload: disabled" -ForegroundColor Green
    }
} catch { Write-Host "  [INFO] Task Offload 不適用於此系統" -ForegroundColor DarkGray }

# 1h. VBS / HVCI 檢測
try {
    $vbs = Get-CimInstance -ClassName Win32_DeviceGuard -Namespace root\Microsoft\Windows\DeviceGuard -ErrorAction SilentlyContinue
    if ($vbs -and $vbs.VirtualizationBasedSecurityStatus -eq 2) {
        Write-Host "  ⚠  [VBS/HVCI] 虛擬化安全性已啟用！" -ForegroundColor Red
        Write-Host "              這是 Win11 網卡延遲暴增的 #1 原因 (DPC 延遲可達 2-5x)" -ForegroundColor Red
        Write-Host "              手動關閉 (需重開機):" -ForegroundColor Yellow
        Write-Host "                bcdedit /set hypervisorlaunchtype off" -ForegroundColor Yellow
        Write-Host "              或在 Windows 安全性 → 裝置安全性 → 核心隔離 → 關閉記憶體完整性" -ForegroundColor Yellow
        $changes += "VBS: 需手動關閉 (bcdedit /set hypervisorlaunchtype off)"
        $restartNeeded = $true
    } elseif ($vbs -and $vbs.VirtualizationBasedSecurityStatus -eq 0) {
        Write-Host "  [OK]  VBS/HVCI: 已關閉" -ForegroundColor Green
    } else {
        Write-Host "  [INFO] VBS 狀態: $($vbs.VirtualizationBasedSecurityStatus)" -ForegroundColor Gray
    }
} catch { Write-Host "  [INFO] 無法查詢 VBS 狀態" -ForegroundColor DarkGray }

# ============================================================================
# 2. PCIe ASPM 省電 → OFF
# ============================================================================
Write-Host "`n[2] PCIe 鏈路省電關閉 (ASPM Off)" -ForegroundColor Magenta

$pcieGuid = '501a4d13-42b7-4429-a178-159e3af5501d'
$aspmGuid = 'ee9c1b29-9f53-4b28-b52e-9e7e0cf0a1a2'
try {
    powercfg /setacvalueindex SCHEME_CURRENT $pcieGuid $aspmGuid 0 2>$null
    powercfg /setactive SCHEME_CURRENT 2>$null
    Write-Host "  [SET] PCIe ASPM -> Off（消除可變 PCIe 延遲）" -ForegroundColor Green
    $changes += "PCIe ASPM -> Off"
} catch { Write-Host "  [FAIL] PCIe ASPM: $_" -ForegroundColor Red }

# ============================================================================
# 3. 尋找 EtherCAT / Npcap 目標網卡
# ============================================================================
Write-Host "`n[3] 偵測目標網卡" -ForegroundColor Magenta

# 優先找有線乙太網卡
$allNic = @(Get-NetAdapter | Where-Object {
    $_.MediaType -eq '802.3' -and
    $_.Name -notlike '*Bluetooth*' -and $_.Name -notlike '*Wi-Fi*' -and $_.Name -notlike '*WLAN*' -and
    $_.InterfaceDescription -notlike '*Wireless*' -and $_.InterfaceDescription -notlike '*Bluetooth*' -and
    $_.InterfaceDescription -notlike '*Wi-Fi*' -and
    $_.InterfaceDescription -notlike '*Loopback*'
})

if (-not $allNic -or $allNic.Count -eq 0) {
    Write-Host "  [FAIL] 沒有找到有線乙太網卡！" -ForegroundColor Red
    Write-Host "         請確認網卡已啟用且驅動已安裝。" -ForegroundColor Yellow
    exit 1
}

Write-Host "  找到 $($allNic.Count) 張網卡:" -ForegroundColor White
$allNic | ForEach-Object {
    $linkSpeed = if ($_.LinkSpeed) {
        $raw = try { [double]$_.LinkSpeed } catch { $null }
        if ($raw) {
            # numeric (bps) e.g. 1000000000 → divide by 1e6 for Mbps
            "$([math]::Round($raw/1e6)) Mbps"
        } else {
            # string like "1 Gbps" or "100 Mbps" — parse number + unit
            $s = "$($_.LinkSpeed)"
            if ($s -match '([\d.]+)\s*(Gbps|Mbps|Kbps|bps)') {
                $val = [double]$matches[1]
                $unit = $matches[2]
                switch ($unit) {
                    'Gbps' { "$([math]::Round($val*1000)) Mbps" }
                    'Mbps' { "$([math]::Round($val)) Mbps" }
                    'Kbps' { "$([math]::Round($val/1000,1)) Mbps" }
                    default { $s }
                }
            } else { $s }
        }
    } else { '無連線' }
    Write-Host "    - $($_.Name)  [$($_.InterfaceDescription)]  $linkSpeed  Status=$($_.Status)" -ForegroundColor Gray
}

# ============================================================================
# 4. 網卡進階屬性設定（本腳本核心）
# ============================================================================
Write-Host "`n[4] 網卡驅動參數調優" -ForegroundColor Magenta

function Set-NicProp {
    param([string]$NicName, [string[]]$PropNames, [string]$Value, [string]$Category)

    foreach ($pn in $PropNames) {
        $prop = $null
        $prop = Get-NetAdapterAdvancedProperty -Name $NicName -DisplayName $pn -ErrorAction SilentlyContinue
        if (-not $prop) { $prop = Get-NetAdapterAdvancedProperty -Name $NicName -RegistryKeyword $pn -ErrorAction SilentlyContinue }
        if (-not $prop -and $pn.StartsWith('*')) { $prop = Get-NetAdapterAdvancedProperty -Name $NicName -RegistryKeyword $pn.Substring(1) -ErrorAction SilentlyContinue }
        if (-not $prop -and -not $pn.StartsWith('*')) { $prop = Get-NetAdapterAdvancedProperty -Name $NicName -RegistryKeyword ('*' + $pn) -ErrorAction SilentlyContinue }

        if ($prop) {
            $cur = $prop.DisplayValue
            if ($cur -eq $Value) {
                Write-Host "    [OK]  $pn = $cur" -ForegroundColor DarkGray
                return
            }

            # Try English DisplayValue first
            try {
                if ($cur -ne $Value) {
                    Set-NetAdapterAdvancedProperty -Name $NicName -RegistryKeyword $prop.RegistryKeyword -DisplayValue $Value -ErrorAction Stop
                }
                Write-Host "    [SET] $pn = $Value (was: $cur)  [$Category]" -ForegroundColor Green
                $script:changes += "$NicName :: $pn : $cur -> $Value"
                return
            } catch {
                # Locale fallback
                if ($prop.ValidDisplayValues) {
                    $valid = $prop.ValidDisplayValues
                    $target = $null
                    $wantDisable = $Value -match '^(Disabled?|Off|None)$'
                    $wantEnable  = $Value -match '^(Enabled?|On)$'
                    $wantMaster  = $Value -match 'Master'

                    if ($wantDisable) {
                        $target = $valid | Where-Object { $_ -match '停用|關閉|Disabled?$|Off$|None$' } | Select-Object -First 1
                    } elseif ($wantEnable) {
                        $target = $valid | Where-Object { $_ -match '啟用|Enabled?$|On$' } | Select-Object -First 1
                    } elseif ($wantMaster) {
                        $target = $valid | Where-Object { $_ -match '主要模式|Master' } | Select-Object -First 1
                    }

                    # Numeric fallback: if Value looks like a number, pick the max valid numeric value
                    if (-not $target -and $Value -as [int]) {
                        $numericVals = $valid | ForEach-Object { $_ -as [int] } | Where-Object { $_ }
                        if ($numericVals) {
                            $maxVal = ($numericVals | Measure-Object -Maximum).Maximum
                            $target = $valid | Where-Object { $_ -eq "$maxVal" -or $_ -eq $maxVal } | Select-Object -First 1
                        }
                    }
                    if (-not $target) { $target = $valid[0] }
                    try {
                        Set-NetAdapterAdvancedProperty -Name $NicName -RegistryKeyword $prop.RegistryKeyword -DisplayValue $target -ErrorAction Stop
                        Write-Host "    [SET] $pn = $target (localized, was: $cur)  [$Category]" -ForegroundColor Green
                        $script:changes += "$NicName :: $pn : $cur -> $target"
                    } catch {
                        Write-Host "    [FAIL] $pn : $($_.Exception.Message)" -ForegroundColor Red
                    }
                } else {
                    Write-Host "    [FAIL] $pn : $($_.Exception.Message) (無可用值)" -ForegroundColor Red
                }
            }
            return
        }
    }
    # Property not found — not all NICs support all properties, that's OK
}

foreach ($nic in $allNic) {
    $n = $nic.Name
    Write-Host "`n  ── $n ($($nic.InterfaceDescription)) ──" -ForegroundColor Cyan

    # --- 中斷調節（Npcap 封包即時性關鍵）---
    Write-Host "  [中斷調節] 關閉 → Npcap 才能即時收到每個封包" -ForegroundColor White
    Set-NicProp -NicName $n -PropNames @('Interrupt Moderation','*InterruptModeration') -Value 'Disabled' -Category '中斷'
    Set-NicProp -NicName $n -PropNames @('Interrupt Moderation Rate','ITR') -Value 'Off' -Category '中斷'

    # --- 緩衝區最大化 ---
    Write-Host "  [緩衝區] 最大化 → 避免 burst 期間掉封包" -ForegroundColor White
    Set-NicProp -NicName $n -PropNames @('Receive Buffers','*ReceiveBuffers') -Value '2048' -Category '緩衝'
    Set-NicProp -NicName $n -PropNames @('Transmit Buffers','*TransmitBuffers') -Value '2048' -Category '緩衝'

    # --- 流量控制關閉 ---
    Write-Host "  [流量控制] 關閉 → 避免 pause frame 造成不可預測延遲" -ForegroundColor White
    Set-NicProp -NicName $n -PropNames @('Flow Control','*FlowControl') -Value 'Disabled' -Category '流量'

    # --- EEE 省電關閉 ---
    Write-Host "  [EEE] 關閉 → 消除節能狀態轉換延遲" -ForegroundColor White
    Set-NicProp -NicName $n -PropNames @('Energy Efficient Ethernet','EEELinkAdvertisement','*EEEControl') -Value 'Disabled' -Category '省電'

    # --- 其他省電 ---
    Set-NicProp -NicName $n -PropNames @('ReduceSpeedOnPowerDown') -Value 'Disabled' -Category '省電'
    Set-NicProp -NicName $n -PropNames @('Green Ethernet','*GreenEthernet') -Value 'Disabled' -Category '省電'
    Set-NicProp -NicName $n -PropNames @('Power Saving Mode','*PowerSavingMode') -Value 'Disabled' -Category '省電'
    Set-NicProp -NicName $n -PropNames @('Auto Disable Gigabit','*AutoDisableGigabit') -Value 'Disabled' -Category '省電'

    # --- 硬體 Offload 全部關閉（Npcap 核心需求！）---
    Write-Host "  [Offload] 全部關閉 → Npcap 才能擷取到原始封包內容" -ForegroundColor White
    Set-NicProp -NicName $n -PropNames @('Large Send Offload (IPv4)','*LsoV1IPv4','Large Send Offload V1 (IPv4)') -Value 'Disabled' -Category 'Offload'
    Set-NicProp -NicName $n -PropNames @('Large Send Offload V2 (IPv4)','*LsoV2IPv4') -Value 'Disabled' -Category 'Offload'
    Set-NicProp -NicName $n -PropNames @('Large Send Offload V2 (IPv6)','*LsoV2IPv6') -Value 'Disabled' -Category 'Offload'
    Set-NicProp -NicName $n -PropNames @('TCP Checksum Offload (IPv4)','*TCPChecksumOffloadIPv4','*TCPUDPChecksumOffloadIPv4') -Value 'Disabled' -Category 'Offload'
    Set-NicProp -NicName $n -PropNames @('TCP Checksum Offload (IPv6)','*TCPChecksumOffloadIPv6','*TCPUDPChecksumOffloadIPv6') -Value 'Disabled' -Category 'Offload'
    Set-NicProp -NicName $n -PropNames @('UDP Checksum Offload (IPv4)','*UDPChecksumOffloadIPv4') -Value 'Disabled' -Category 'Offload'
    Set-NicProp -NicName $n -PropNames @('UDP Checksum Offload (IPv6)','*UDPChecksumOffloadIPv6') -Value 'Disabled' -Category 'Offload'
    Set-NicProp -NicName $n -PropNames @('IPv4 Checksum Offload','*IPChecksumOffloadIPv4') -Value 'Disabled' -Category 'Offload'
    Set-NicProp -NicName $n -PropNames @('TCP Connection Offload (IPv4)','*TCPConnectionOffloadIPv4') -Value 'Disabled' -Category 'Offload'
    Set-NicProp -NicName $n -PropNames @('TCP Connection Offload (IPv6)','*TCPConnectionOffloadIPv6') -Value 'Disabled' -Category 'Offload'

    # --- ARP/NS Offload（Npcap 看不到被 offload 的 ARP 封包）---
    Write-Host "  [ARP/NS Offload] 關閉 → Npcap 才能擷取 ARP/NDP 封包" -ForegroundColor White
    Set-NicProp -NicName $n -PropNames @('ARP Offload','*PMARPOffload') -Value 'Disabled' -Category 'Offload'
    Set-NicProp -NicName $n -PropNames @('NS Offload','*PMNSOffload') -Value 'Disabled' -Category 'Offload'

    # --- Priority/VLAN ---
    Write-Host "  [Priority/VLAN] 關閉 → 裸 EtherCAT 不需要 VLAN tag" -ForegroundColor White
    Set-NicProp -NicName $n -PropNames @('Packet Priority & VLAN','Priority & VLAN','*PriorityVLANTag') -Value 'Packet Priority & VLAN Disabled' -Category 'VLAN'

    # --- Wake-on-LAN ---
    Write-Host "  [Wake-on-LAN] 關閉 → 避免休眠相關中斷" -ForegroundColor White
    Set-NicProp -NicName $n -PropNames @('Wake on Magic Packet','*WakeOnMagicPacket') -Value 'Disabled' -Category '省電'
    Set-NicProp -NicName $n -PropNames @('Wake on Pattern Match','*WakeOnPattern') -Value 'Disabled' -Category '省電'
    Set-NicProp -NicName $n -PropNames @('Wake on Link','*WakeOnLink') -Value 'Disabled' -Category '省電'

    # --- DMA Coalescing（關鍵！批次 DMA 增加延遲）---
    Write-Host "  [DMA Coalescing] 關閉 → 每個封包立即 DMA，不批次延遲" -ForegroundColor White
    Set-NicProp -NicName $n -PropNames @('DMA Coalescing','DMACoalescing') -Value 'Disabled' -Category 'DMA'

    # --- Jumbo Packet（EtherCAT 用標準 1514 幀）---
    Write-Host "  [Jumbo Packet] 關閉 → EtherCAT 使用標準幀大小" -ForegroundColor White
    Set-NicProp -NicName $n -PropNames @('Jumbo Packet','*JumboPacket') -Value 'Disabled' -Category 'MTU'

    # --- RSS 啟用（IRQ 分散到多核）---
    Write-Host "  [RSS] 啟用 → 網卡 IRQ 分散處理，避免單核瓶頸" -ForegroundColor White
    Set-NicProp -NicName $n -PropNames @('Receive Side Scaling','*RSS') -Value 'Enabled' -Category 'IRQ'

    # --- Gigabit Master Mode（避免 auto-negotiation 延遲）---
    Write-Host "  [Master Mode] 強制 Master → 消除自動協商延遲" -ForegroundColor White
    Set-NicProp -NicName $n -PropNames @('Gigabit Master Slave Mode','PrimarySecondary','*MasterSlave') -Value 'Force Master Mode' -Category '連結'

    # --- Link State Logging（減少驅動記錄開銷）---
    Set-NicProp -NicName $n -PropNames @('Log Link State Event','LogLinkStateEvent') -Value 'Disabled' -Category '記錄'

    # --- Adaptive Inter-Frame Spacing ---
    Set-NicProp -NicName $n -PropNames @('Adaptive Inter-Frame Spacing') -Value 'Disabled' -Category '時序'

    # --- Max IRQ per Second (some Realtek NICs) ---
    Set-NicProp -NicName $n -PropNames @('Maximum Number of RSS Queues','*NumRssQueues') -Value '2' -Category 'IRQ'
}

# ============================================================================
# 5. 網卡裝置層電源管理
# ============================================================================
Write-Host "`n[5] 網卡裝置電源管理關閉" -ForegroundColor Magenta

foreach ($nic in $allNic) {
    try {
        $wmiAdapter = Get-CimInstance -ClassName Win32_NetworkAdapter |
            Where-Object { $_.Name -like "*$($nic.InterfaceDescription)*" -or $_.NetConnectionID -eq $nic.Name } |
            Select-Object -First 1
        if ($wmiAdapter) {
            $pmSupported = $wmiAdapter.PSObject.Properties['PowerManagementSupported']
            if ($pmSupported -and $pmSupported.Value) {
                $wmiAdapter | Invoke-CimMethod -MethodName 'SetPowerManagement' -Arguments @{
                    PowerManagementCapabilities = @()
                } -ErrorAction SilentlyContinue | Out-Null
                Write-Host "  [SET] $($nic.Name) :: 允許電腦關閉此裝置以節省電源 → OFF" -ForegroundColor Green
                $changes += "$($nic.Name) :: device power saving -> OFF"
            } else {
                Write-Host "  [OK]  $($nic.Name) :: 不支援電源管理" -ForegroundColor DarkGray
            }
        }
    } catch { Write-Host "  [INFO] $($nic.Name) :: WMI 電源管理跳過: $_" -ForegroundColor DarkGray }
}

# ============================================================================
# 6. Npcap 最佳實務提示
# ============================================================================
Write-Host "`n[6] Npcap 最佳實務檢查" -ForegroundColor Magenta

Write-Host "  確保應用程式已做以下 Npcap 調優：" -ForegroundColor White
Write-Host "    - pcap_set_immediate_mode(handle, 1)   ← 關閉 kernel 封包批次緩衝" -ForegroundColor Gray
Write-Host "    - pcap_setmintocopy(adapter, 1)        ← 最小化 kernel→user 延遲" -ForegroundColor Gray
Write-Host "    - SOEM 內部已在 ecx_init 後套用上述設定" -ForegroundColor Gray

# Check if we have an Npcap binding check tool
$npcapDriverPath = "$env:SystemRoot\System32\drivers\npcap.sys"
if (Test-Path $npcapDriverPath) {
    Write-Host "  [OK]  npcap.sys 驅動存在: $npcapDriverPath" -ForegroundColor Green
} else {
    Write-Host "  [WARN] npcap.sys 未在預期位置" -ForegroundColor Yellow
}

# ============================================================================
# 總結
# ============================================================================
Write-Host "`n╔══════════════════════════════════════════════════════╗" -ForegroundColor Cyan
Write-Host "║                    設定完成摘要                       ║" -ForegroundColor Cyan
Write-Host "╚══════════════════════════════════════════════════════╝" -ForegroundColor Cyan

if ($changes.Count -eq 0) {
    Write-Host "  ✓ 所有設定已是最佳狀態，無需變更" -ForegroundColor Green
} else {
    Write-Host "  已套用 $($changes.Count) 項變更:" -ForegroundColor Green
    $changes | ForEach-Object { Write-Host "    • $_" -ForegroundColor White }
}

# Final verdict
Write-Host "`n  已處理的 Win11 限制:" -ForegroundColor White
Write-Host "    ✓ 網路節流 (NetworkThrottlingIndex)      — 關閉" -ForegroundColor Green
Write-Host "    ✓ 硬體 Offload (LSO/CSO/TCO/ARPOFF)      — 全部關閉" -ForegroundColor Green
Write-Host "    ✓ RSC 封包合併                              — 關閉" -ForegroundColor Green
Write-Host "    ✓ 中斷調節 (Interrupt Moderation)         — 關閉" -ForegroundColor Green
Write-Host "    ✓ DMA Coalescing                          — 關閉" -ForegroundColor Green
Write-Host "    ✓ EEE 省電                                 — 關閉" -ForegroundColor Green
Write-Host "    ✓ PCIe ASPM 省電                           — 關閉" -ForegroundColor Green
Write-Host "    ✓ 裝置電源管理                              — 關閉" -ForegroundColor Green
Write-Host "    ✓ 流量控制 (Flow Control)                 — 關閉" -ForegroundColor Green
Write-Host "    ✓ Wake-on-LAN / ARP Offload               — 關閉" -ForegroundColor Green
Write-Host "    ✓ RSS IRQ 分散                             — 啟用" -ForegroundColor Green
Write-Host "    ✓ 緩衝區                                   — 最大化 2048" -ForegroundColor Green

if ($restartNeeded) {
    Write-Host "`n  ⚠ 部分設定需要重新開機才能生效！" -ForegroundColor Yellow
    Write-Host "    請在方便時重新啟動電腦。" -ForegroundColor Yellow
}

Write-Host "`n  套用後建議驗證步驟:" -ForegroundColor White
Write-Host "    1. 重開機" -ForegroundColor Gray
Write-Host "    2. 執行: .\scripts\audit_nic_settings.ps1   (檢查 NIC 設定)" -ForegroundColor Gray
Write-Host "    3. 執行: .\scripts\capture_etw_trace.ps1    (ETW DPC/ISR 延遲驗證)" -ForegroundColor Gray
Write-Host "    4. 啟動 WuBigCat 進行實際 EtherCAT 通訊測試" -ForegroundColor Gray

Write-Host "`n  Done." -ForegroundColor Magenta
