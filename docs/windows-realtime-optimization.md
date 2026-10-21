---
description: Windows 即時優化 — C++ 自動化實作文件（拓撲自適應 IRQ 分配 + OS 自適應 Sleep 策略 + NIC 硬體鎖定 + Npcap Keep-Alive）
updated: 2026-09-19
---

# Windows 10 SOEM 實時優化指南（C++ 自動化版）

本文對照 `src/core/` 中的 C++ 實作，描述程式啟動時自動套用的全系統實時優化策略。
所有機制均為純 C++ Win32 API，無外部 PowerShell 腳本依賴。

---

## 1. 程式啟動時自動套用的優化

以下依 `main.cpp` → `MainWindow::Create()` → 連線流程的執行順序描述。

### 1.1 電源策略 (`CoreHelpers::ApplyRealtimePowerPolicy`)

- **檔案**: `src/core/CoreHelpers.cpp`
- **API**: `powrprof.h` — `PowerSetActiveScheme`, `PowerWriteACValueIndex`
- **呼叫點**: `src/app/main.cpp`（啟動早期，license check 之前）

| 設定 | API | 效果 |
|------|-----|------|
| 卓越性能電源計畫 | `PowerSetActiveScheme(GUID {e9a42b02...})` | 停用 CPU 頻率調節 |
| Fallback 高效能 | `PowerSetActiveScheme(GUID {8c5e7fda...})` | 卓越性能不可用時自動降級 |
| 禁用 Core Parking | `PowerWriteACValueIndex(core parking min=0)` | 所有核心保持喚醒 |
| 禁用 USB 選擇性暫停 | `PowerWriteACValueIndex(USB suspend=0)` | 避免 Quest 3/搖桿/TrackIR 被省電干擾 |
| 禁用硬碟關閉 timeout | `PowerWriteACValueIndex(disk timeout=0)` | 防止 StatusLogger 寫入時磁碟暫停 |

- **權限**: 需要 Admin；無 Admin 時記錄警告、非致命跳過
- **持久性**: 設定寫入目前電源計畫，重開機後保持

### 1.2 CPU 硬隔離 (`CpuIsolation::InitializeHardIsolation`)

- **檔案**: `src/core/CpuIsolation.cpp`
- **API**: `CpuTopology`（封裝 `GetLogicalProcessorInformationEx`）、`SetThreadAffinityMask`、`SetPriorityClass`、`SetProcessAffinityMask`（**僅驅離監控使用**）
- **呼叫點**: `src/app/main.cpp`（啟動早期；候選核由高往低自動挑選，跳過 core 0，可用 `BigCat.ini [EtherCAT] RtCoreIndex=N` 覆寫）／`src/ui/MainWindow.cpp`（啟動背景 monitor）
- **最後修訂**: 2026-09-19（對照 `CpuIsolation.cpp` 校正 Step 2 流程、遮罩表、設計意圖與軟體監控層）

#### 架構

```
Step 1: DetectCpuTopology(rtPhysicalCoreIndex)
  ← 由 main.cpp 由「最高實體核往下」挑選（跳過 core 0），INI 可覆寫；下例以實體核 4 說明
  └─ CpuTopology（GetLogicalProcessorInformationEx）
      └─ 找出實體核心 #4 對應的所有邏輯核心
         例: 8C/16T Intel → rtCoreMask_ = 0x1010 (LP 4 + LP 12)
         例: 8C/16T AMD   → rtCoreMask_ = 0x0030 (LP 4 + LP 5)

Step 2: ApplyProcessLevelIsolation()
  ├─ **不呼叫 SetProcessAffinityMask** — process 遮罩維持「全系統遮罩」(見下方設計意圖)
  ├─ ExcludeCurrentThreadFromRtCores()
  │   只把「呼叫端（main/UI）執行緒」釘到 nonRtCoreMask_
  ├─ SetPriorityClass(REALTIME_PRIORITY_CLASS)
  │   守護進程自身最高優先級（fallback: HIGH_PRIORITY_CLASS）
  └─ ApplyProcessPowerThrottling()   ← Win11 關閉 CPU 節流（須在提權之後）

Step 3: Cyclic thread 啟動時
  └─ PinCurrentThreadToRtCores()
      └─ SetThreadAffinityMask(rtThreadMask_)  ← 僅第一個邏輯核心
         例: 0x10 (CPU 4 only, CPU 12 閒置 → 乾淨 L1/L2 cache)
```

> `main.cpp` 在硬隔離成功後會立刻呼叫一次 `RepelOnce()`，關閉「初始化完成 → monitor 第一輪掃描」之間的空窗；
> 之後才由 `MainWindow::Create()` 啟動週期性 monitor（見本節末「軟體層級輔助監控」）。

#### 三組遮罩

| 遮罩 | 用途 | 位元範例（8C/16T Intel） |
|------|------|----------|
| `rtCoreMask_` | RT 實體核的**全部**邏輯核心。用於 monitor 的 `guardMask`、`VerifyRtCoreIsolation()` 比對基準、IRQ/USB 重導向的剝除來源。**不用於縮減 process 遮罩** | `0x1010`（LP 4 + LP 12） |
| `rtThreadMask_` | Cyclic thread 釘選（僅 primary LP，HT sibling 閒置） | `0x0010`（LP 4） |
| `nonRtCoreMask_` | 所有非 RT 執行緒的家 = **RT 相鄰實體核（physIdx − 1）的全部邏輯核心**。**不等於** `~rtCoreMask_` | `0x0003`（實體核 3 = LP 3 + LP 11） |

> **設計意圖**: Windows 要求 thread affinity 必須是 process affinity 的**子集**。
> 若把 RT 核從 process 遮罩剔除，cyclic thread 的 `SetThreadAffinityMask` 會**靜默失敗**而落到共用核心（曾導致 SOEM 斷線，2026-06-25 修正）。
> 因此 RT 核的保護改為「**不縮減 process 遮罩** ＋ per-thread 親和性 ＋ monitor 驅離外來 process」：
> cyclic thread 釘在 RT 核 primary LP，HT sibling 完全閒置以保持 L1/L2 乾淨；
> 其餘所有非 RT 執行緒（HTTP／UI／SSE／Monitor／per-slave）一律釘到 `nonRtCoreMask_`，兼顧快取鄰近性與 IRQ 同核。
>
> **⚠ 遮罩範例為 Intel 平台**（8C/16T，實體核 4 = LP 4/12）。AMD 平台的 LP 編號排列不同（同一實體核心的 LP 連續），運行時由 `GetLogicalProcessorInformationEx` 自動偵測正確遮罩，無需手動換算。

#### 軟體層級輔助監控

`StartIsolationMonitor()` 建立背景監控執行緒（自身先以 `ExcludeCurrentThreadFromRtCores()` 釘在近 RT 核 N-2），
依 `s_repelIntervalMs`（預設 10ms，程式碼夾在 5–100ms）分三層驅離。判定條件是
**process affinity 是否與 RT 核遮罩有交集**，命中即 `SetProcessAffinityMask` 剃除 RT 核。

```
MonitorThreadLoop()   週期 = s_repelIntervalMs（預設 10ms，夾在 5~100ms）
  ├─ 每 3 次 → RepelOtherProcessesFast(mask, forceThreadMigrate=true)   ← 中層：驅離 + thread 立即強制遷移
  ├─ 每 5 次 → RepelOtherProcesses(mask)                                ← 全掃：同上 + safeNonRtMask 前置檢查
  └─ 其餘    → RepelOtherProcessesFast(mask)                            ← 快層：只改 process 遮罩
```

- `IsCriticalSystemProcess()` 白名單（`System`、`System Idle Process`、`smss`、`csrss`、`wininit`、`services`、`lsass`、`winlogon`）一律跳過；`svchost.exe` 刻意不在白名單內
- `OpenProcess()` 失敗者靜默跳過（無 Admin 時幾乎全部跳過）
- `targetProcessNames` 目前**未被驅離路徑引用**；實際行為是「全程序無差別驅離」，非名稱比對
- `TriggerRepel()`（`CyclicTask` 每 PDO 週期呼叫）目前未被 monitor 等待，屬空轉
- 軟體驅離**無法**排除 kernel ISR/DPC（詳見 §2.1 限制表與 §4 判讀前提）

### 1.3 系統定時器精度 (`timeBeginPeriod`)

- **檔案**: `src/ethercat/EtherCATMaster.Init.cpp:995`
- **API**: `timeBeginPeriod(1)` — 系統定時器解析度提升至 1ms
- **恢復**: `timeEndPeriod(1)` — 在 cyclic thread 停止時呼叫
- **備註**: SOEM 自帶的 `osal.c` 也會呼叫 `timeBeginPeriod(1)`

### 1.4 高解析度 Waitable Timer

- **檔案**: `src/ethercat/EtherCATMaster.Cyclic.cpp`
- **API**: `CreateWaitableTimerExW(..., CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, ...)`
- **用途**: PDO cycle 等待時使用 μs 級精度 timer，搭配 QPC + TSC spin

### 1.5 USB 中斷頻率降低至 500 Hz (`SetUsbInterruptRate500Hz`)

- **檔案**: `src/core/CoreHelpers.cpp`
- **API**: `SetupDiGetClassDevsW` + `RegCreateKeyExW` (登錄檔)
- **呼叫點**: `src/app/main.cpp`（啟動早期，在 NIC IRQ 重導向之後、CPU 隔離之前）

#### 動機

USB xHCI 主控器的中斷速率由 IMOD（Interrupt Moderation）暫存器控制。
預設值通常為 0（無調節），導致 USB 中斷以微訊框（~125 μs）頻率產生，產生大量
ISR/DPC，可能在 PDO 週期中搶佔 RT 核心。

將 IMOD 設為 500 Hz（2 ms 間隔），可將 USB 中斷速率降低 75% 以上，
減少對 PDO 週期的干擾，同時不影響 USB 週邊的正常操作。

#### 實作

函式會執行以下步驟：

1. 列舉所有 USB 裝置，篩選出 xHCI（USB 3.0）主控器
2. 對每個 xHCI 主控器，在裝置硬體登錄檔路徑寫入：
   - `HKLM\SYSTEM\CurrentControlSet\Enum\PCI\<instance>\Device Parameters`
   - `InterruptModerationInterval` = `16`（REG_DWORD）
3. 同時寫入全域 USBXHCI 驅動參數路徑：
   - `HKLM\SYSTEM\CurrentControlSet\Services\USBXHCI\Parameters`
   - `InterruptModerationInterval` = `16`
4. 記錄每個控制器的寫入狀態與換算後的中斷頻率

#### 值計算

| 參數 | 值 | 說明 |
|------|-----|------|
| `InterruptModerationInterval` | 16 | 125 μs 增量 × 16 = 2000 μs |
| 中斷間隔 | ~2 ms | 2000 μs，滿足 500 Hz 目標 |
| 等效頻率 | 500 Hz | 每秒中斷次數上限 |
| IMOD 規格增量 | 250 ns（xHCI 1.x） | 相當於 IMOD = 8000（250 ns × 8000 = 2000 μs） |

> Windows usbxhci.sys 驅動以 125 μs 為單位暴露此登錄值（相容 xHCI 0.96 規格），
> 底層 xHCI 1.x 硬體實際使用 250 ns 增量；驅動會自動轉換。

#### 結果範例

```
[USB 500Hz] Setting USB xHCI interrupt moderation to 500 Hz...
[USB 500Hz]   Intel(R) USB 3.0 eXtensible Host Controller:
              wrote InterruptModerationInterval=16 (was 0, now reads 16) ≈ 500 Hz
[USB 500Hz] ✓ Global USBXHCI Parameters set to 16
[USB 500Hz] ✓ 2/2 xHCI controller(s) set to 500 Hz
[USB 500Hz] ⚠ 需重新啟動電腦，USB 中斷頻率設定才會生效！
```

- **權限**: 需要 Admin；無 Admin 時記錄警告、非致命跳過
- **持久性**: 寫入登錄檔，**需要重開機才能完全生效**

### 1.6 HID 裝置輪詢頻率降低至 125 Hz (`SetHidPollingRate`)

- **檔案**: `src/core/CoreHelpers.cpp`
- **API**: `SetupDiGetClassDevsW` + `RegCreateKeyExW`（登錄檔）
- **呼叫點**: `src/app/main.cpp`（啟動早期，在 USB 500 Hz 之後、CPU 隔離之前）

#### 動機

USB HID 裝置（搖桿、滑鼠、鍵盤、TrackIR、遊戲控制器）使用中斷傳輸
（interrupt transfer）定期回報資料。高階遊戲裝置的輪詢率（polling rate）
通常為 500–1000 Hz（1–2 ms 間隔），每個封包都會觸發 USB 中斷。

在即時 EtherCAT 系統中，這些 HID 中斷會與 PDO 週期競爭 CPU 資源，
即使 IRQ 親和性已導離 RT 核心，仍可能造成 DPC 延遲與排程干擾。

將 HID 輪詢率降為 125 Hz（8 ms 間隔）可減少 75–87 % 的 HID 中斷量，
同時對使用者操作的響應延遲影響極小（人類反應時間 ~200 ms）。

#### 實作

函式執行以下步驟：

1. **全域設定**：寫入 `HKLM\SYSTEM\CurrentControlSet\Services\HidUsb\Parameters`
   - `PollDeviceInterval` = `8`（REG_DWORD，單位 ms）
2. **個別裝置覆蓋**：列舉所有 HID 類別裝置，對每個裝置寫入
   - `HKLM\SYSTEM\CurrentControlSet\Enum\HID\<instance>\Device Parameters`
   - `PollIntervalOverride` = `8`（REG_DWORD）
   - HidUsb.sys 驅動在裝置層級值存在時優先使用此值

#### 效果

| 對照 | 預設（高階遊戲裝置） | 調整後 |
|------|--------------------|--------|
| 輪詢間隔 | 1–4 ms | **8 ms** |
| 輪詢頻率 | 250–1000 Hz | **125 Hz** |
| 每 2 ms PDO 週期中斷數 | 2–8 次 | **0–1 次** |
| 使用者感受 | 極低延遲 | 仍遠低於人體感知閾值 |

#### 結果範例

```
[HID Poll] Setting USB HID polling rate to 125 Hz (8 ms)...
[HID Poll] ✓ Wrote PollDeviceInterval=8 ms (was 4 ms, now reads 8 ms → 125 Hz)
[HID Poll]   Logitech G USB Wheel: wrote PollIntervalOverride=8 ms
[HID Poll]   TrackIR: wrote PollIntervalOverride=8 ms
[HID Poll]   HID-compliant mouse: already 8 ms, skip
[HID Poll] === Summary ===
[HID Poll] ✓ Global HidUsb\Parameters: PollDeviceInterval=8 ms (125 Hz)
[HID Poll] ✓ Per-device: 5/8 HID device(s) set to 8 ms
[HID Poll] ⚠ 需重新啟動電腦，HID polling rate 設定才會生效！
```

- **權限**: 需要 Admin；無 Admin 時記錄警告、非致命跳過
- **持久性**: 寫入登錄檔，**需要重開機才能完全生效**
- **備註**: 少數 HID 裝置（如某些 VR 控制器）可能在 125 Hz 下出現精度下降；若有此情形可為該裝置單獨移除 `PollIntervalOverride` 值

---

## 2. 連線時自動套用的優化

### 2.1 硬體 MSI/MSI-X 中斷親和性 — 自動適應拓撲 (`ApplySystemWideIrqAffinityToCpu0`)

- **檔案**: `src/core/CoreHelpers.cpp`（`ApplySystemWideIrqAffinityToCpu0`）
- **API**: `SetupDiGetClassDevsW` + `RegCreateKeyExW` (登錄檔)
- **呼叫點**: `src/ui/MainWindow.Connect.cpp`、`src/ui/MainWindow.Adapter.cpp`
- **最後修訂**: 2026-09-19（**文件對照程式碼校正**：補齊實際寫入行為與限制、修正啟動期 USB 目標核心描述、修正 RSS/無線目標描述）
- **歷史**: 2026-06-26 改為自動適應 CPU 拓撲的實體核心範圍分配，不再硬編碼 CPU 編號

#### 策略: 拓撲自適應 IRQ 分散

函式名稱雖含 `ToCpu0`，但實作已改為**根據系統實體核心數量自動分配 IRQ 目標範圍**。所有 IRQ 只分配到實體核心（primary LP），絕不碰 SMT/HT 兄弟執行緒。

| 實體核心數 | GPU IRQ（1核） | I/O + 儲存 IRQ（合併） | DCS 遊戲（乾淨核心） | EtherCAT NIC（獨立） |
|-----------|---------------|----------------------|--------------------|-------------------|
| ≥8 核心 | 實體核 **0** | 實體核 **1**（USB+NIC+Audio+NVMe+SATA，實際命中依關鍵字比對） | 實體核 **2~N-3**（多個乾淨核心） | 實體核 **N-2** |
| 7 核心 | 實體核 0 | 實體核 1 | 實體核 2~3（2 核） | 實體核 5 |
| 6 核心 | 實體核 0 | 實體核 1 | 實體核 2（1 核） | 實體核 4 |
| 5 核心 | 實體核 0 | 實體核 1 | —（無乾淨核心） | 實體核 3 |
| 4 核心 | 實體核 0 | 實體核 1 | —（無乾淨核心） | 實體核 2 |
| 2–3 核心 | 實體核 0 | 實體核 0 | — | 實體核 1 |

- **RT 核心自動剝離**: `stripRt()` — 所有遮罩會自動排除 RT 實體核心的所有邏輯 CPU
- **僅使用 primary LP**: 透過 `CpuTopology::GetPrimaryLpMask()`，IRQ 永遠不會分配到 SMT sibling
- **裝置類別辨識**: GPU (Display/NVIDIA)、NVMe (SCSIAdapter)、SATA AHCI (HDC class)、USB (xHCI/EHCI host controller)、音效 (Media class)、遊戲網卡 (Net，自動排除 EtherCAT NIC instance ID)

#### 登錄檔寫入內容（每個裝置寫入兩個 key）

```
Affinity Policy:
  DevicePolicy           = 4  (REG_DWORD, IRQ_DEVICE_POLICY_SPECIFIED_PROCESSORS)
  AssignmentSetOverride  = KAFFINITY mask  (REG_BINARY, 多核心遮罩)
  → 指定 MSI-X 中斷只在目標實體核心範圍內處理
```

> ⚠ 不再寫入 `MSISupported=1` — Windows 和裝置驅動會自動協商 MSI 模式。強制寫入可能導致部分 NIC 驅動載入失敗（已在 Intel I210 上觀察到）。
> ⚠ 遮罩使用 `ULONG64`（`REG_BINARY`）而非 `REG_DWORD`，支援超過 32 核心的系統。

#### MSI/MSI-X vs 傳統 Line-based IRQ

```
變更前 (line-based IRQ)              變更後 (MSI/MSI-X)
─────────────────────────           ─────────────────────
IRQ 16 ← GPU + NVMe + SATA + USB  MSI Vec 0 → GPU 專用（core 0）
IRQ 17 ← NIC + Audio 共享          MSI Vec 1 → 所有 I/O+儲存合併（core 1）
                                    MSI Vec 2 → EtherCAT 專用（core N-2）
每個中斷都要排隊處理 ❌              每個裝置獨立中斷 ✅
                                    可獨立指定 CPU affinity ✅
```

#### 網卡自動識別

- `FindEthercatNicInstanceId` 根據主畫面選定的網卡名稱，透過 SetupDi 比對 FriendlyName/DeviceDesc 自動解析 device instance ID
- 不再硬編碼 Intel I210/I225 — 任何品牌網卡都能正確識別
- 遊戲網卡排除邏輯也改為精確 instance ID 比對

#### 全域 IRQ 寫入開關

- `g_irqWriteEnabled`（`CoreHelpers.cpp` 頂部）：設為 `false` 可停用所有 MSI/MSI-X registry 寫入與 RSS PowerShell 呼叫
- 讀取與顯示功能（`GetSystemIrqAffinitySummary` 等）不受影響

- **權限**: 需要 Admin；無 Admin 時記錄警告跳過
- **持久性**: 寫入登錄檔，**需要重開機才能完全生效**
- **設計意圖**: NVIDIA GPU MSI-X 向量極少（1~8 個），僅需 1 個實體核心處理 GPU IRQ；所有非 EtherCAT 的 I/O 與儲存裝置（USB、網卡、音效、NVMe、SATA）合併到同 1 個核心，釋放中間核心作為 DCS 遊戲的「乾淨」執行環境；EtherCAT NIC 獨立核心；RT 核心完全不接觸任何 IRQ

#### 實作細節與限制（2026-09-19 對照 `CoreHelpers.cpp`）

實際寫入行為與理想配置圖有幾處差異，判讀 log 與 §4 配置圖時必須一併考慮：

| 項目 | 實作現況 |
|------|----------|
| **綁定粒度** | 每個目標實體核心只寫入**一個位元**（`CpuTopology::GetPrimaryLpMask()`，primary LP），**不含 SMT sibling**。嚴格說是「該核的 primary LP」而非「整顆實體核」。 |
| **裝置比對方式** | 以 **device class GUID + 描述關鍵字** 篩選，命中才寫入：GPU 需含 `nvidia`；NVMe 需含 `nvme` / `nvm express`；SATA 需含 `ahci` / `sata`；USB 需含 `host controller` / `xhci` / `ehci`；音效需含 `high definition audio` / `audio`；遊戲網卡取 Net class 全部（排除 EtherCAT NIC instance ID）。**比對不到則整個類別 skipped**（log 顯示 `no matching devices found`），因此「非 EtherCAT 全部合併到實體核 1」並非保證結果。 |
| **RT 核心剝離的退化路徑** | `stripRt()` 把 RT 遮罩從五個 mask 全部剝除；但若剝除後為 0，會先退回 `buildRangeMask(0,0) & ~rtFullMask`，仍為 0 時**還原成剝除前的原值**（可能含 RT 核心）。`checkMaskRt()` 對此只印 ❌ 警告，**不會阻止寫入**。少核心 + RT core = 0 的拓撲才會踩到。 |
| **生效驗證範圍** | 只驗證 **registry 值**，**不驗證執行期 IRQ 路由**。`CheckEthercatIrqAffinityReady()` 比對 NIC 的 `AssignmentSetOverride` 與期望值；不符時彈窗警告，使用者可選擇強制繼續連線（`MainWindow.Connect.cpp`）。 |
| **失敗不致命** | 任何單一裝置寫入失敗都不會中止流程；函式固定回傳 `true`。無 Admin 時全部類別都會 `totalSkipped`。 |
| **系統核心 IRQ** | Timer、DPC watchdog 等由核心自行排程的中斷無法透過 device affinity 控制。因此「RT 核心零 IRQ」是**相對保證** — 程式只能做到「不指定」＋硬隔離＋驅離 foreign process＋`PROCESSOR_IDLE_DISABLE`。 |

### 2.1b USB 中斷啟動期重導向 (`RedirectUsbIrqAffinityFromRtCore`)

- **檔案**: `src/core/CoreHelpers.cpp`
- **API**: `SetupDiGetClassDevsW` + registry MSI/MSI-X Affinity Policy
- **呼叫點**: `src/app/main.cpp`（啟動早期，在 cyclic thread 建立之前）
- **最後修訂**: 2026-06-26

在啟動階段（而非連線時）就將 USB 主控器（xHCI/EHCI/OHCI/UHCI）的中斷親和性寫入 registry，避免 USB ISR/DPC 在 PDO 週期搶佔 RT 核心。

#### 運作流程

```
Phase 1: 收集 USB host controller instance IDs
  └─ SetupDi 列舉 USB class 裝置，篩選 host controller 類型
  └─ 檢查 MSISupported registry key 辨識 MSI-X vs line-based (INTx)

Phase 2: 寫入 registry IRQ affinity
  └─ SetDeviceIrqAffinityToCpu() → DevicePolicy=4 + AssignmentSetOverride
  └─ 目標 CPU 由 CpuTopology::FindBestNonRtLp(rtCoreMask) 自動選擇

Phase 3: 事後驗證
  └─ 讀回 registry 確認寫入成功（GetDeviceInstanceEffectiveIrqCpu）
  └─ 不自動重啟 USB 控制器（避免鍵盤/滑鼠斷線）
```

- **目標 CPU**: 由 `CpuTopology::FindBestNonRtLp(rtCoreMask)` 決定，該函式回傳**第一個非 RT 實體核心的 primary LP**（通常為實體核 0 / LP 0）— **不是**連線期採用的實體核 1。
- **⚠ 與連線期目標不一致（重要）**: 啟動期先寫入實體核 0；之後使用者完成一次連線時，`ApplySystemWideIrqAffinityToCpu0` 會**覆寫為實體核 1**。因此若只跑過啟動階段就重開機，USB IRQ 會落在實體核 0（與 GPU 同核）；要符合 §4 配置圖，需「完整連線至少一次 → 再重開機」。
- **不重啟裝置**: 與 `ApplySystemWideIrqAffinityToCpu0` 不同，USB 控制器不會被程式化重啟，因為會導致所有 USB 裝置短暫斷線（`g_usbIrqMsiXRestartOk` 固定為 `false`）
- **全域狀態**: 寫入結果存入 `g_usbIrqRedirectAttempted` 等靜態變數，供主畫面 `GetUsbIrqRedirectStatus()` 查詢顯示
- **權限**: 需要 Admin；無 Admin 時記錄警告跳過
- **持久性**: 寫入 registry，需重開機生效

### 2.1c 非 EtherCAT 網卡中斷啟動期重導向 (`RedirectNicIrqFromRtCore`)

- **檔案**: `src/core/CoreHelpers.cpp`
- **API**: 同 USB redirect 模式 — registry MSI/MSI-X Affinity Policy
- **呼叫點**: `src/app/main.cpp`（啟動早期，緊接在 USB redirect 之後）

列舉所有 Net-class 裝置，排除 EtherCAT NIC，將其 IRQ affinity 寫入 registry 遠離 RT 核心。

- **目標 CPU**: 實體核 1（≥4 核系統；2–3 核系統為實體核 0），取該核 primary LP；僅在拓撲查詢失敗時 fallback 到 `CpuTopology::FindBestNonRtLp()` — 與連線期 `ApplySystemWideIrqAffinityToCpu0` 的遊戲網卡目標一致
- **設計意圖**: 確保其他網卡（WiFi、藍牙 PAN、第二有線網卡）的中斷不會干擾 RT 核心

### 2.1d EtherCAT IRQ 親和性就緒檢查 (`CheckEthercatIrqAffinityReady`)

- **檔案**: `src/core/CoreHelpers.cpp`
- **呼叫點**: `src/ui/MainWindow.Connect.cpp`（使用者點擊 Connect 時，在所有其他 IRQ 優化之前執行）
- **最後修訂**: 2026-06-26

在允許 EtherCAT 連線之前，檢查 EtherCAT NIC 的 registry IRQ affinity 是否已正確指向目標 CPU。若尚未生效（例如前次寫入 registry 後未重開機），彈出對話框警告使用者，使用者可選擇強制繼續或取消。

- **比對邏輯**: 讀取 registry 中的 `AssignmentSetOverride` KAFFINITY mask，比對 `ApplySystemWideIrqAffinityToCpu0` 所計算的預期目標 CPU
- **返回值**: `outReady=true` 表示 IRQ 已正確配置；`outReady=false` 表示需要重開機

### 2.2 EtherCAT 網卡 RSS 優化 (`OptimizeNicIrqAffinity`)

- **檔案**: `src/core/CoreHelpers.cpp`
- **API**: PowerShell `Set-NetAdapterRss`（唯一仍用 PS 的項目，因無 C API）
- **設定**: `BaseProcessorNumber=rssBaseCpu, MaxProcessorNumber=rssBaseCpu, NumberOfReceiveQueues=1`
- **rssBaseCpu**: 由 `CpuTopology::FindBestNonRtLp(rtCoreMask)` 取得，即**第一個非 RT 實體核心的 primary LP**（通常為實體核 0）— 與 registry IRQ affinity 給 EtherCAT NIC 的目標（實體核 N-2）**不是同一個核心**，兩者為獨立機制（RSS 佇列 vs MSI-X 中斷路由）
- **TwinCAT 介面卡自動跳過**: 名稱含 `twincat` 者直接略過（由 TwinCAT 核心層管理網路，不支援 RSS）
- **設計意圖**: 確保 RSS 佇列不落在 RT 核心，NIC ndis.sys DPC 不與 RT 核心競爭

### 2.3 無線網卡 IRQ 重平衡 (`RebalanceWirelessIrqAffinityAwayFromRtCore`)

- **檔案**: `src/core/CoreHelpers.cpp`
- **API**: PowerShell `Set-NetAdapterRss`（只調 RSS，**不寫 registry IRQ affinity**）
- **設定**: 僅處理 `Status -eq 'Up'` 且判定為 WiFi / 藍牙 PAN 的介面卡，`BaseProcessorNumber=1`、`MaxProcessorNumber=max(1, rtCore-1)`（RT core 不存在時改用 `logicalCount-1`）、`NumberOfReceiveQueues=1` — 實際範圍是 **CPU 1 ~ (RT core − 1)**，並非常數 1-3
- **藍牙裝置**: 只稽核 `HKLM\SYSTEM\CurrentControlSet\Enum\...\Affinity Policy` 是否存在並輸出 INFO，**不自動修改**

### 2.4 EtherCAT 網卡協定隔離 (`IsolateEtherCATAdapter`)

- **檔案**: `src/core/CoreHelpers.cpp`
- **停用綁定**: TCP/IPv6, MS Client, File Sharing, QoS, LLTD
- **保留**: TCP/IPv4、Npcap driver
- **額外**: 停用 NetBIOS、防火牆 Public

### 2.5 網卡驅動層級調優 (`OptimizeNicForRealtime`)

- **檔案**: `src/core/CoreHelpers.cpp`
- **API**: PowerShell `Set-NetAdapterAdvancedProperty`（唯一仍用 PS 的項目，因無 C API）
- **呼叫點**: `src/ui/MainWindow.Connect.cpp`（緊接在 `IsolateEtherCATAdapter` 之後）
- **最後修訂**: 2026-06-14（IPv6 offload、ARP/NS offload、中斷調節分離、屬性名修正、網卡名後綴剝離）

| 設定 | 目標值 | 影響 |
|------|--------|------|
| **Interrupt Moderation** | **Disabled** | 🔴 最關鍵：關閉後中斷延遲從 50-200us → <5us |
| **Interrupt Moderation Rate** | **Off** | 🔴 Intel I210 獨立 ITR，必須與上項分開設定 |
| Receive Buffers | 2048 (max) | 減少 descriptor 不足導致的 Rx 暫停 |
| Transmit Buffers | 2048 (max) | 減少 Tx 佇列阻塞 |
| Flow Control | Disabled | 避免 pause frame 延遲 Tx |
| Energy Efficient Ethernet | Disabled | 避免低功耗模式的喚醒延遲 |
| LSO (IPv4 / IPv6) | Disabled | 避免軟體模擬巨型封包的處理延遲 |
| TCP/UDP Checksum Offload (IPv4 / IPv6) | Disabled | 避免 offload engine 的 DMA 競爭 |
| ARP Offload / NS Offload | Disabled | 避免不必要的鄰居探索 offload 延遲 |
| Packet Priority & VLAN | Disabled | 減少不必要的標籤處理（Intel I210 屬性名為 `Packet Priority & VLAN`） |
| Gigabit Master Mode | Force Master | 避免自動協商的時脈同步延遲 |

- **屬性名稱**因 NIC 廠牌而異（Intel I210/I225、Realtek 等），腳本會嘗試多種名稱
- TwinCAT 網卡自動跳過（kernel-level real-time）
- **2026-06-14 Bug 修正**：
  - 剝離 `FindAdapterDisplayName` 附加的 ` [XXXXXXXX]` 後綴，避免 PowerShell 查無網卡
  - Intel I210 的 `Interrupt Moderation` 與 `Interrupt Moderation Rate` 是兩個獨立屬性，必須分開呼叫（前者 `Disabled`，後者 `Off`）
  - `Packet Priority & VLAN` 屬性名修正（原誤用 `Priority & VLAN`）
  - 補上 IPv6 offload（LSO v2、TCP/UDP Checksum Offload IPv6）及 ARP/NS Offload

### 2.6 Npcap 封包即時交付 (`NpcapApplyImmediateMode`)

- **檔案**: `src/core/CoreHelpers.cpp`
- **API**: `pcap_set_immediate_mode`, `pcap_setmintocopy`（runtime 動態解析自 `wpcap.dll`）
- **呼叫點**: `src/ethercat/EtherCATMaster.Init.cpp`（`ecx_init` 成功後、`ecx_config_init` 之前）
- **最後修訂**: 2026-06-14

Npcap 預設使用批次交付模式：kernel driver 等待 timer（~1ms）或 buffer 填滿才將封包複製到 userspace。此函式對已開啟的 pcap handle 套用：

| API | 參數 | 效果 |
|-----|------|------|
| `pcap_set_immediate_mode` | 1 | 封包到達即交付，不等 timer |
| `pcap_setmintocopy` | 1 | kernel buffer 有 1 byte 就複製到 userspace |

- **只優化主程式選用的 EtherCAT 網卡** — 透過 `context.port.sockhandle` 精確定位
- **無 Npcap SDK 依賴** — runtime 動態解析，編譯時不需連結 Npcap
- **安全降級** — Npcap 版本過舊時記錄警告、不回傳失敗

---

## 3. Cyclic Thread 內的最佳化

### 3.1 執行緒優先級與親和性

```cpp
// EtherCATMaster.Cyclic.cpp — CyclicThreadFunc()

// 1. 硬隔離（優先）
if (CpuIsolation::IsHardIsolationActive()) {
    CpuIsolation::PinCurrentThreadToRtCores();
    // → SetThreadAffinityMask(rtThreadMask_)    僅實體邏輯核心
    // → SetThreadPriority(THREAD_PRIORITY_TIME_CRITICAL)
}

// 2. 傳統 fallback（硬隔離未啟用或失敗時）
if (!CpuIsolation::IsHardIsolationActive() || mask == 0) {
    SetThreadAffinityMask(..., 0x10);  // legacy Core 4
}
```

### 3.2 鎖定工作集（防止分頁）

- `VirtualLock(IOmap, sizeof(IOmap))` — 鎖定 IOMap 實體頁面
- `SetProcessWorkingSetSize` — 亦在初始化階段設定

### 3.3 即時週期排程

```
QPC 粗調等待 → High-Resolution Waitable Timer → TSC 短尾 spin
  ├─ QueryPerformanceCounter 計算下次喚醒時間
  ├─ CreateWaitableTimerExW(HIGH_RESOLUTION)
  └─ __rdtsc() spin 最後 <200μs 的精密等待
```

### 3.4 Win11 Sleep(1) 粒度問題與 QPC 閉環時序修正

**最後修訂**: 2026-06-28（Win11 OP timeout 診斷、OS 自適應 Sleep 策略、BigCat.exe 整合）

#### 問題

Win10 上正常運行的 EtherCAT 程式在 Win11 相同硬體上出現 `FATAL: EtherCAT OP timeout`，從站回報 `AL=0x001A Synchronization error`。診斷發現 PDO 週期偏離目標值 75%：

| 指標 | Win10 | Win11（修復前） |
|------|-------|----------------|
| 500 rounds 實際耗時 | ~1.00 s | ~1.76 s |
| 平均週期 | ~2000 us | ~3520 us |
| DC SYNC0 | 穩定鎖定 | 時序漂移 → AL=0x001A |

#### 根因：`Sleep(1)` 跨平台粒度差異

即使呼叫 `timeBeginPeriod(1)` 將系統定時器設為 1ms，`Sleep(1)` 的實際行為在 Win10 與 Win11 上顯著不同：

| OS | `Sleep(1)` 實際耗時 | 對 2000us 週期的影響 |
|----|-------------------|-------------------|
| Win10 | ~1.0 ms（可預測） | 可作為 bulk-wait（overshoot ~0.2ms） |
| Win11 | 1–3 ms（不可預測） | 週期漂移 +76%，DC SYNC0 崩潰 |

Win11 排程器在 power management、core parking、scheduler heuristics 等方面行為不同，導致 `Sleep(1)` 的 granularity 遠不如 Win10 穩定。

#### 修復策略：OS 自適應 Sleep(1) + TSC spin 混合

核心原則是**不讓 `Sleep(1)` 的粒度接近或超過 PDO 週期**，並依 OS 自動選擇最優策略：

```
OS 偵測 (RtlGetVersion, build >= 22000 = Win11)
  ├─ Win11: 純 TSC spin（Sleep(1) 不可預測，禁用短週期 Sleep）
  └─ Win10: Sleep(1) bulk + TSC spin tail（Sleep ≈1ms 可靠，降 CPU）
```

**`cycle_exchange()` (test_Ethercat_jog.cpp)**：
```cpp
if (!g_isWin11 && remainUs > 1500) {
    Sleep(1);  // Win10: bulk wait ~1ms, CPU released
} else if (remainUs > cycleUs) {
    Sleep(1);  // Win11/long-cycle: only when safe
}
// TSC spin precision tail (all paths)
cycle_timer::spin_us(remainder);
```

**`CyclicTask()` fallback (EtherCATMaster.Cyclic.cpp)**：
```cpp
// Primary: HR Waitable Timer (unchanged, works on both OS)
// Fallback (timer unavailable):
if (!g_isWin11 && waitDuration > 1500us) {
    Sleep(1);  // Win10 hybrid
} else if (waitDuration > period) {
    Sleep(ms); // Win11/long-cycle: only when safe
}
```

**`PDOProcessingTask()` fallback (EtherCATMaster.Cyclic.cpp)**：
```cpp
if (!g_isWin11 && remaining_us > 1500) {
    Sleep(1);  // Win10 hybrid
} else if (remaining_us > 1500 && remaining_us > cycleMicros) {
    Sleep(1);  // Win11: only for long cycles
}
```

#### CPU 使用率（2ms 週期估算）

| OS | 策略 | 工作 | Sleep | Spin | CPU |
|----|------|------|-------|------|-----|
| Win11 | 純 TSC spin | 500us | 0 | 1500us | 75% |
| Win10 | Sleep(1) 混合 | 500us | 1000us | 500us | 25% |

- Win11 高 CPU 是 OS 限制下的必要代價（DC SYNC0 穩定性優先）
- Win10 Sleep(1) 可靠 → 大幅降 CPU 同時保持週期精度

#### 連帶修正：`pump_processdata` 改用 QPC 閉環

`pump_processdata` 原使用 `ecx_receive + osal_usleep` 模式，其中 `osal_usleep` 內部呼叫 `SleepEx`，同樣受 Win11 Sleep 粒度影響。導致 CiA402 狀態機（`wait_axis_status`）卡在 Ready to Switch On。

修復為內部委派給 `cycle_exchange`，共用 function-local static `phaseRef` 維持相位連續性。

#### Npcap `pcap_set_immediate_mode` 回傳 -4 並非問題

診斷初期懷疑 Npcap 即時模式未生效導致封包延遲。經確認：
- SOEM `ecx_init` 在 `pcap_open` 時已使用 `PCAP_OPENFLAG_MAX_RESPONSIVENESS` 旗標
- 此旗標等價於 immediate mode
- `pcap_set_immediate_mode` 回傳 **-4 (`PCAP_ERROR_ACTIVATED`)** 是因為 handle 已啟動，不影響即時性

#### DC SYNC0 預穩定化

在 SAFE_OP 狀態進行 4 秒 PDO pumping 讓從站 DC 時鐘收斂後再請求 OP，大幅提高首次 OP 轉換成功率。

#### 驗證結果（2026-06-28 Win10 + Win11 實測）

| 指標 | 修復前 (Win11) | 修復後 (Win11) | 修復後 (Win10) |
|------|---------------|---------------|---------------|
| DC-pre-stab 500-round | ~1,757,764 us | ~999,988 us | ~999,988 us |
| 週期誤差 | +75.8% | ~0% | ~0% |
| EtherCAT OP | FAIL | PASS | PASS |
| CiA402 enable | N/A | PASS | PASS |
| CPU (2ms 週期) | — | 75% | 25% |

#### 相關檔案

| 項目 | 檔案 | 函式 |
|------|------|------|
| OS 偵測 (RtlGetVersion) | `tests/test_Ethercat_jog.cpp` | `cycle_timer::init()` |
| OS 偵測 (RtlGetVersion) | `src/ethercat/EtherCATMaster.Cyclic.cpp` | `DetectOSVersion()` |
| QPC 閉環週期（自適應 Sleep） | `tests/test_Ethercat_jog.cpp` | `cycle_exchange()` |
| CyclicTask Sleep fallback 加固 | `src/ethercat/EtherCATMaster.Cyclic.cpp` | `CyclicTask()` |
| PDOProcessingTask Sleep fallback 加固 | `src/ethercat/EtherCATMaster.Cyclic.cpp` | `PDOProcessingTask()` |
| pump_processdata 改用 cycle_exchange | `tests/test_Ethercat_jog.cpp` | `pump_processdata()` |
| 可調週期測試（-cycle CLI + 互動） | `tests/test_Ethercat_jog.cpp` | `g_cycle_us`, `read_cycle_input()` |

---

## 3.5 非 RT 執行緒近核心限制策略

**修訂**: 2026-07-01

#### 設計

所有非 `TIME_CRITICAL` 的 BigCat.exe 執行緒（HTTP、UI、Monitor、SSE、Per-slave、Jog 等）全部限制到 **RT 核心的相鄰實體核心（N-2）** 上執行，包含該實體核心的全部邏輯 CPU（primary LP + SMT sibling）。

#### 動機

```
┌─────────┬─────────┬──────────┬──────────┬─────────┐
│ Core 0  │ Core 1  │ Core 2~4 │ Core 5   │ Core 6  │  ★ Core 7
│ GPU IRQ │ I/O+儲存 │  🎮 DCS  │ (spare)  │ 非 RT   │  RT Cyclic
│         │   IRQ   │  乾淨     │          │ threads │  (硬隔離)
└─────────┴─────────┴──────────┴──────────┴─────────┘
                                            ↑ 相鄰
                                       快取鄰近性最高
```

1. **快取鄰近性** — 非 RT thread 與 RT Cyclic thread 在相鄰核心，L3 cache 共享命中率最高，避免跨 NUMA/socket 延遲
2. **不佔用 DCS 核心** — 中間核心（2~N-3）完全沒有 BigCat 執行緒，DCS World 可獨佔
3. **IRQ 親和性一致** — EtherCAT IRQ 也在 core N-2，IRQ 處理與非 RT 工作在相同核心完成，無跨核心 bounce

#### 實作

```cpp
// CpuIsolation.cpp — DetectCpuTopology()
int nearRtPhys = rtPhysicalCoreIndex - 1;  // physical core just before RT
// 取得該實體核心的所有邏輯 CPU（primary LP + SMT sibling）
DWORD_PTR nearRtMask = 0;
for (int lp : nearInfo->lps)
    nearRtMask |= ((DWORD_PTR)1 << lp);
nonRtCoreMask_ = nearRtMask & systemMask;

// EtherCATMaster.cpp — BuildCoreCandidates()
DWORD_PTR nonRtMask = GetNonRtCoreMask();
if (nonRtMask != 0) available &= nonRtMask;  // per-slave thread 也被限制
```

#### 受影響的執行緒

| 執行緒 | 親和性設定方式 | 結果 |
|--------|--------------|------|
| Cyclic Thread | `PinCurrentThreadToRtCores()` | 固定在 RT 核心 N-1 |
| PDO Processing (低延遲) | `PinCurrentThreadToRtCores()` | 固定在 RT 核心 N-1 |
| PDO Processing (一般) | `ExcludeCurrentThreadFromRtCores()` | 近 RT 核心 N-2 |
| Per-Slave Cyclic | `AllocateCoreMaskRoundRobin()` + `BuildCoreCandidates()` 交集 | 近 RT 核心 N-2 |
| HTTP Server + Connections | `ExcludeCurrentThreadFromRtCores()` | 近 RT 核心 N-2 |
| SSE Broadcaster | 繼承 process mask（已排除 RT） | 近 RT 核心 N-2 |
| Stewart Native Planner | 繼承 process mask | 近 RT 核心 N-2 |
| Isolation Monitor | `ExcludeCurrentThreadFromRtCores()` | 近 RT 核心 N-2 |
| Error Code Check | 繼承 process mask | 近 RT 核心 N-2 |
| Main UI Thread | `ExcludeCurrentThreadFromRtCores()` | 近 RT 核心 N-2 |
| Jog Soft-Start / License / Log | 繼承 process mask | 近 RT 核心 N-2 |

---

## 4. 完整 CPU 核心配置圖（拓撲自適應，2026-07-01 修訂）

> 本節為 `ApplySystemWideIrqAffinityToCpu0` 的**目標配置**。實際生效結果受「裝置關鍵字是否命中」與「是否已重開機」影響，判讀前請先看本節最後的〈判讀這張圖的前提〉。

IRQ 分配不再使用硬編碼 CPU 編號，而是根據 `CpuTopology` 偵測的實體核心數量自動選擇範圍。
以下以 8C/16T 系統為例說明分配邏輯（僅使用 physical core index，不指定 LP 編號）：

```
實體核 0: ★ GPU IRQ（僅 1 核 — NVIDIA MSI-X 向量極少，1 核綽綽有餘）
實體核 1: I/O + 儲存 IRQ（USB + 遊戲網卡 + 音效 + NVMe + SATA 全合併）
實體核 2: 🎮 DCS World（乾淨核心，無 IRQ 綁定）
實體核 3: 🎮 DCS World（乾淨核心，無 IRQ 綁定）
實體核 4: 🎮 DCS World（乾淨核心，無 IRQ 綁定）
實體核 5: 閒置備用
實體核 6: ★ EtherCAT NIC IRQ（獨立，完全遠離 IRQ 干擾）
實體核 7: ★ SOEM Cyclic (硬隔離) — 不容許任何 IRQ，不容許任何 foreign thread
```

> **⚠ Intel vs AMD 邏輯處理器編號差異**：Windows 工作管理員顯示的 CPU 編號（LP index）**因 CPU 廠商而異**，本文件的「實體核 N」指的是 `CpuTopology` 偵測到的 physical core index（0-based），完全獨立於 LP 編號：
>
> | 廠商 | 8C/16T 的 LP 排列（工作管理員編號） | 本文件對應 |
> |------|-----------------------------------|-----------|
> | **Intel** (Hyper-Threading) | Core 0=LP 0,8 / Core 1=LP 1,9 / … / Core 7=LP 7,15 | 實體核 0–7 |
> | **AMD** (SMT) | Core 0=LP 0,1 / Core 1=LP 2,3 / … / Core 7=LP 14,15 | 實體核 0–7 |
>
> 程式碼透過 `GetLogicalProcessorInformationEx` + `CpuTopology` 自動處理此差異，無需手動換算。`GetPrimaryLpMask(N)` 傳回實體核 N 的 primary LP 遮罩，正確適用於兩種平台。

#### 判讀這張圖的前提（2026-09-19 對照程式碼校正）

1. **這是穩態配置，不是即時狀態**：registry IRQ affinity 需重開機才生效；且 USB 的目標核心在啟動期（實體核 0）與連線期（實體核 1）不同。圖示成立的前提是「**已完整連線至少一次 → 已重開機**」。
2. **只指定 primary LP**：圖中「實體核 N」實際只寫入該核 primary LP 一個位元，SMT sibling 不在遮罩內。
3. **裝置未命中關鍵字就不會被配置**：詳見 §2.1「實作細節與限制」的裝置比對表；log 出現 `no matching devices found` 即代表該類別未生效。
4. **實體核 N-2 同時是非 RT 執行緒的家**：EtherCAT NIC IRQ 與 BigCat 所有非 RT 執行緒（HTTP / UI / Monitor / SSE / per-slave）共用實體核 N-2（見 §3.5）。圖示只標示了 IRQ 部分。
5. **「RT 核心零 IRQ」是相對保證**：系統核心自行排程的中斷（timer / DPC watchdog）無法透過 device affinity 排除；程式只能不指定 + 硬隔離 + 驅離 foreign process。

---

## 5. 程式碼對照表

| 優化項目 | 檔案 | 函式 |
|----------|------|------|
| 電源計畫 + Core Parking + USB + 磁碟 | `CoreHelpers.cpp` | `ApplyRealtimePowerPolicy()` |
| CPU 拓撲偵測 + 硬隔離 | `CpuIsolation.cpp` | `InitializeHardIsolation()` |
| Cyclic thread 綁定實體核心 | `CpuIsolation.cpp` | `PinCurrentThreadToRtCores()` |
| 軟體隔離監控 (repel) | `CpuIsolation.cpp` | `StartIsolationMonitor()` / `MonitorThreadLoop()` |
| IRQ 分散策略（拓撲自適應） | `CoreHelpers.cpp` | `ApplySystemWideIrqAffinityToCpu0()` |
| USB IRQ 啟動期重導向 | `CoreHelpers.cpp` | `RedirectUsbIrqAffinityFromRtCore()` |
| USB 中斷頻率 500 Hz | `CoreHelpers.cpp` | `SetUsbInterruptRate500Hz()` |
| HID 輪詢頻率 125 Hz | `CoreHelpers.cpp` | `SetHidPollingRate()` |
| 非 EtherCAT 網卡 IRQ 啟動期重導向 | `CoreHelpers.cpp` | `RedirectNicIrqFromRtCore()` |
| EtherCAT IRQ 親和性就緒檢查 | `CoreHelpers.cpp` | `CheckEthercatIrqAffinityReady()` |
| 網卡自動識別（不限品牌） | `CoreHelpers.cpp` | `FindEthercatNicInstanceId()` |
| NIC RSS → 安全核心 | `CoreHelpers.cpp` | `OptimizeNicIrqAffinity()` |
| 無線 IRQ 遠離 RT | `CoreHelpers.cpp` | `RebalanceWirelessIrqAffinityAwayFromRtCore()` |
| 網卡協定隔離（含 TCP/IPv4 卸載） | `CoreHelpers.cpp` | `IsolateEtherCATAdapter()` |
| NIC 驅動層級調優 | `CoreHelpers.cpp` | `OptimizeNicForRealtime()` |
| NIC 硬體鎖定（登錄檔） | `CoreHelpers.cpp` | `LockNicHardwareRealtime()` |
| Npcap Keep-Alive 開啟 | `CoreHelpers.cpp` | `NpcapKeepAliveOpen()` |
| Npcap Keep-Alive 關閉 | `CoreHelpers.cpp` | `NpcapKeepAliveClose()` |
| EtherCAT NIC 靜態 IP（TCP/IP 可用時） | `CoreHelpers.cpp` | `EnsureStaticIpOnEthercatNic()` |
| Npcap immediate mode | `CoreHelpers.cpp` | `NpcapApplyImmediateMode()` |
| NVIDIA GPU 動態 P-State 停用 | `CoreHelpers.cpp` | `NvidiaSetDisableDynamicPState()` |
| TwinCAT 服務控制 | `CoreHelpers.cpp` | `ControlTwinCatServices()` |
| 系統定時器 1ms | `EtherCATMaster.Init.cpp` | `timeBeginPeriod(1)` |
| High-res waitable timer | `EtherCATMaster.Cyclic.cpp` | `CreateHighResolutionTimerHandle()` |
| 工作集鎖定 | `EtherCATMaster.Init.cpp` | `VirtualLock(IOmap)` |
| Process REALTIME priority | `CpuIsolation.cpp` | `ApplyProcessLevelIsolation()` |
| Thread TIME_CRITICAL | `EtherCATMaster.Cyclic.cpp` | `CyclicThreadFunc()` |
| OS 偵測 + 自適應 Sleep 策略 | `EtherCATMaster.Cyclic.cpp` | `DetectOSVersion()`, `g_isWin11`（可由 `BigCat.ini [Compat] ForceWin10Mode=1` 強制為 Win10 策略） |
| CyclicTask Sleep fallback 加固 | `EtherCATMaster.Cyclic.cpp` | `CyclicTask()` |
| PDOProcessingTask Sleep fallback 加固 | `EtherCATMaster.Cyclic.cpp` | `PDOProcessingTask()` |
| QPC 閉環週期（OS 自適應 Sleep） | `tests/test_Ethercat_jog.cpp` | `cycle_exchange()`, `cycle_timer::init()` |
| pump_processdata 改用 QPC 閉環 | `tests/test_Ethercat_jog.cpp` | `pump_processdata()` |
| 可調週期測試（-cycle CLI + 互動） | `tests/test_Ethercat_jog.cpp` | `g_cycle_us`, `read_cycle_input()` |
| CPU 拓撲查詢 | `CpuTopology.cpp` | `FindBestNonRtLp()`, `GetPrimaryLpMask()` |
| 全域 IRQ 寫入開關 | `CoreHelpers.cpp` | `g_irqWriteEnabled` |
| 非 RT 執行緒近核心限制 | `CpuIsolation.cpp` | `nonRtCoreMask_`（近 RT 核心） |
| Per-slave 執行緒近核心限制 | `EtherCATMaster.cpp` | `BuildCoreCandidates()` → `GetNonRtCoreMask()` |
| 非 RT 執行緒親和性套用 | `CpuIsolation.cpp` | `ExcludeCurrentThreadFromRtCores()` |

---

## 6. 啟動流程總覽

```
WinMain (main.cpp)
  ├─ CPU 相容性檢查
  ├─ TPM 檢查
  ├─ SetPhysicalEthercatAllowed
  ├─ ApplyRealtimePowerPolicy()                    ← 電源策略
  ├─ RedirectUsbIrqAffinityFromRtCore()            ← USB IRQ 啟動期重導向（registry write，需重開機）
  ├─ RedirectNicIrqFromRtCore()                    ← 非 EtherCAT 網卡 IRQ 啟動期重導向
  ├─ SetUsbInterruptRate500Hz()                    ← USB 中斷頻率 500 Hz
  ├─ SetHidPollingRate()                           ← HID polling rate 125 Hz
  ├─ CpuIsolation::InitializeHardIsolation()         ← CPU 硬隔離（自動偵測拓撲選取 RT 核心）
  ├─ CpuIsolation::RepelOnce()                     ← 立即驅離 foreign process
  ├─ License check
  └─ MainWindow::Create()
       ├─ SetProcessHighPriorityAndCore4Affinity() ← REALTIME_PRIORITY
       └─ CpuIsolation::StartIsolationMonitor()   ← 軟體監控（背景週期性 repel）

使用者點擊 Connect → OnConnect()
  ├─ CheckEthercatIrqAffinityReady()               ← IRQ 親和性就緒檢查（EtherCAT NIC → 目標 CPU 是否已生效）
  │    └─ 未就緒 → 彈出警告對話框，使用者可強制繼續或取消
  └─ OptimizeAdapterForEthercat()
       ├─ OptimizeNicIrqAffinity()                  ← NIC RSS → 安全核心
       ├─ RebalanceWirelessIrqAffinityAwayFromRtCore() ← WiFi 遠離 RT
       ├─ ApplySystemWideIrqAffinityToCpu0()        ← 全裝置 IRQ 分散（僅在 IRQ 未就緒時執行）
       ├─ IsolateEtherCATAdapter()                  ← 網卡協定隔離（含 TCP/IPv4 卸載）
       ├─ LockNicHardwareRealtime()                 ← NIC 硬體鎖定（登錄檔，防省電）
       ├─ NpcapKeepAliveOpen()                      ← Npcap 持久 handle（防 NIC 閒置）
       ├─ OptimizeNicForRealtime()                  ← NIC 驅動層級調優
       ├─ NvidiaSetDisableDynamicPState(true)       ← NVIDIA GPU 動態 P-State 停用
       └─ ControlTwinCatServices(true)              ← 停用 TwinCAT 服務

  InitializeMasterWithFallback()
       ├─ EnsureStaticIpOnEthercatNic()             ← 靜態 IP（無 TCP/IP 時跳過）
       └─ MasterInitialize()                       ← SOEM ecx_init + pcap_open

... EtherCAT 運行中 ...

使用者點擊 Disconnect → OnDisconnect() → CompleteDisconnect()
  ├─ NpcapKeepAliveClose()                         ← 關閉 keep-alive handle
  ├─ NvidiaSetDisableDynamicPState(false)           ← 恢復 GPU P-State
  └─ ControlTwinCatServices(false)                  ← 恢復 TwinCAT 服務

Cyclic Thread 啟動 (EtherCATMaster.Init.cpp)
  ├─ timeBeginPeriod(1)
  ├─ ecx_init(...)
  ├─ NpcapApplyImmediateMode(sockhandle)            ← Npcap 即時模式
  └─ CreateThread(CyclicThreadFunc)
       └─ CpuIsolation::PinCurrentThreadToRtCores() ← 綁定實體核心
       └─ THREAD_PRIORITY_TIME_CRITICAL
       └─ 進入 PDO cycle 迴圈
```

---

## 7. 驗證方式

### ETW/WPR Trace
```powershell
.\scripts\capture_etw_trace.ps1 -Scenario motion -DurationSec 120
```

### NIC 設定稽核
```powershell
.\scripts\audit_nic_settings.ps1
```

**2026-06-14 驗證結果**（Intel I210，`.\scripts\audit_nic_settings.ps1 -AdapterName "乙太網路 4"`）：
- PASS=16, FAIL=0, WARN=2（RSS BaseProcessor、Allow Turn Off，不影響 EtherCAT）
- 所有關鍵設定（Interrupt Moderation、Flow Control、Offload、VLAN）均已正確套用

### P0 基線量測
```powershell
.\scripts\measure_baseline.ps1
```

---

## 8. BIOS 建議設定

```
- 停用 C-States（CPU 節能狀態）
- 停用 SpeedStep / Turbo Boost（固定頻率）
- 啟用 VT-d / IOMMU（減少 DMA 延遲）
- Hyper-Threading: 保持啟用（程式會自動讓 HT sibling 閒置）
- 固定 CPU 頻率（若 BIOS 支援）
```

---

## 9. 注意事項

1. **REALTIME_PRIORITY_CLASS** 會讓 process 比系統服務更優先；cyclic thread 若無窮迴圈需硬體重開。硬隔離確保僅一個核心受影響。
2. **登錄檔 IRQ affinity** 修改後可能需要重開機才能完全生效（視驅動而定）。
3. **電源計畫設定** 寫入目前 active scheme，重開機後保持。
4. **CPU 拓撲偵測** 自動適應不同 CPU，無需手動設定遮罩。
5. **所有優化皆為非致命**：無 Admin 權限時記錄警告並跳過，不影響程式正常運作。
