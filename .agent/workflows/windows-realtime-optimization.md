---
description: Windows 10 實時優化工作流程
---

# Windows 10 SOEM 實時性能優化指南

## 1. 系統層級優化

### 1.1 電源管理
```powershell
# 設定高性能電源計畫
powercfg /setactive 8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c

# 停用 USB 選擇性暫停
powercfg /setacvalueindex SCHEME_CURRENT 2a737441-1930-4402-8d77-b2bebba308a3 48e6b7a6-50f5-4782-a5d4-53bb8f07e226 0
powercfg /setactive SCHEME_CURRENT

# 防止硬碟關閉
powercfg /change disk-timeout-ac 0
```

### 1.2 CPU 核心隔離（Windows 10 IoT Enterprise）
```cmd
REM 透過 bcdedit 保留 CPU 核心給實時任務
bcdedit /set {current} numproc 4
bcdedit /set {current} onecpu off
```

### 1.3 停用不必要的 Windows 服務
```powershell
# 停用 Windows Search（減少磁碟 I/O）
sc config "WSearch" start= disabled
sc stop "WSearch"

# 停用 Windows Update（測試期間）
sc config "wuauserv" start= disabled
sc stop "wuauserv"

# 停用 SuperFetch/SysMain（減少記憶體管理干擾）
sc config "SysMain" start= disabled
sc stop "SysMain"
```

---

## 2. 應用程式層級優化

### 2.1 提升執行緒優先級
在 `EtherCATMaster::CyclicTask()` 中加入：

```cpp
// 設定為實時優先級
SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

// 設定處理程序優先級
SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS);
```

### 2.2 鎖定工作集（防止分頁）
```cpp
// 在 Initialize() 中加入
SIZE_T minWorkingSetSize = 100 * 1024 * 1024; // 100 MB
SIZE_T maxWorkingSetSize = 200 * 1024 * 1024; // 200 MB
SetProcessWorkingSetSize(GetCurrentProcess(), 
                         minWorkingSetSize, 
                         maxWorkingSetSize);

// 鎖定關鍵記憶體頁面
VirtualLock(IOmap, sizeof(IOmap));
```

### 2.3 CPU 親和性優化
```cpp
// 綁定到特定物理核心（避開核心 0，通常被系統使用）
DWORD_PTR affinityMask = 1ULL << 2; // 使用第 3 個核心
SetThreadAffinityMask(cyclicThread, affinityMask);
```

### 2.4 CPU 核心隔離（進階）⭐ NEW

**目標**: 為 EtherCAT 實時執行緒保留專用 CPU 核心，減少 Windows 系統執行緒干擾

#### 方法 A: 程式碼層級隔離（推薦，無需管理員權限）

```cpp
#include "core/CpuIsolation.h"

// 在程式初始化時（例如 main() 或 WinMain()）
void InitializeRealtimeEnvironment() {
    // 保留 CPU2 和 CPU3 給 EtherCAT 使用
    std::vector<int> reservedCores = {2, 3};
    
    CpuIsolation::InitializeIsolation(reservedCores, [](const std::string& msg) {
        printf("%s\n", msg.c_str());
    });
    
    // 顯示隔離狀態
    printf("%s", CpuIsolation::GetIsolationStatusReport().c_str());
}

// 在 EtherCAT 主執行緒啟動時
void StartEtherCATThread() {
    cyclicThread = CreateThread(NULL, 0, CyclicThreadFunc, this, 0, NULL);
    
    // 綁定到保留核心 (preferredCoreIndex=0 使用第一個保留核心)
    CpuIsolation::BindThreadToReservedCore(cyclicThread, 0);
    
    SetThreadPriority(cyclicThread, THREAD_PRIORITY_TIME_CRITICAL);
}

// UI、HTTP、日誌等非實時執行緒綁定到非保留核心
void StartBackgroundThread() {
    HANDLE bgThread = CreateThread(...);
    CpuIsolation::BindThreadToNonReservedCores(bgThread);
}
```

#### 方法 B: 系統層級隔離（需要管理員權限）

**選項 1**: 使用 bcdedit（永久性，需重開機）
```cmd
:: 限制 Windows 只使用 CPU 0-1，保留 CPU 2-3 給您的應用程式
bcdedit /set {current} numproc 2
:: 重開機後生效
shutdown /r /t 0
```

**選項 2**: 使用 PowerShell 調整系統程序（臨時性）
```powershell
# 將高優先級系統程序推到 CPU 0-1
$systemProcesses = @("svchost", "lsass", "services")
foreach ($procName in $systemProcesses) {
    $processes = Get-Process -Name $procName -ErrorAction SilentlyContinue
    foreach ($proc in $processes) {
        $proc.ProcessorAffinity = 0x3  # CPU 0-1 (二進位: 0011)
    }
}
```

**選項 3**: 在程式中呼叫 AttemptIsolateOtherProcesses（需管理員權限）
```cpp
// 警告：需要管理員權限執行
if (IsUserAdmin()) {
    int adjusted = CpuIsolation::AttemptIsolateOtherProcesses();
    printf("已調整 %d 個程序到非保留核心\n", adjusted);
}
```

#### 驗證核心隔離效果

使用 Process Explorer 或 Process Lasso 查看:
1. 您的 EtherCAT 執行緒是否只在保留核心上執行
2. 系統執行緒分佈情況
3. CPU 使用率是否集中在特定核心

#### 推薦配置（8 核心系統）

| CPU編號 | 用途 | 親和性設定 |
|---------|------|-----------|
| CPU 0 | Windows 系統、中斷 | 系統預設 |
| CPU 1 | UI、HTTP、日誌執行緒 | 非保留遮罩 (0x03) |
| CPU 2 | **EtherCAT 主執行緒** ⭐ | 保留遮罩 (0x04) |
| CPU 3 | **EtherCAT PDO 處理** ⭐ | 保留遮罩 (0x08) |
| CPU 4-7 | 其他應用程式 | 非保留遮罩 (0xF0) |

---

## 3. SSD 干擾緩解

### 3.1 停用寫入快取緩衝區清除
```cmd
REM 查詢磁碟編號
wmic diskdrive get index,caption,interfacetype

REM 停用寫入快取緩衝區清除（編號 0 為例）
reg add "HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e967-e325-11ce-bfc1-08002be10318}\0000" /v "CacheIsPowerProtected" /t REG_DWORD /d 1 /f
```

### 3.2 Over-Provisioning（預留空間）
- 保持 SSD 至少 20% 空間空閒
- 使用廠商工具設定 Over-Provisioning

### 3.3 記錄緩衝區策略
```cpp
// 使用記憶體映射檔案（MMF）替代直接磁碟寫入
// 在非實時執行緒中非同步寫入
if (IsMMFOutputEnabled()) {
    // 非阻塞寫入到 MMF
    mmfWriter_.Write(data, size);
} else {
    // 傳統磁碟 I/O - 應避免在循環執行緒中使用
}
```

---

## 4. 監控與診斷

### 4.1 使用 Windows Performance Analyzer
```powershell
# 記錄系統追蹤
xperf -on PROC_THREAD+LOADER+CSWITCH -stackwalk Profile
# 執行測試...
xperf -d trace.etl
```

### 4.2 實時抖動監控
```cpp
// 已在代碼中實作（第 268-269 行）
std::atomic<uint32_t> maxObservedCommsTime{0};

// 定期檢查
if (GetMaxObservedCommsTime() > desiredCycleTime / 2) {
    // 記錄警告
    SetError("High jitter detected!");
}
```

---

## 5. 環境變數配置

建議加入的環境變數：

```cmd
REM CPU 親和性遮罩（限制使用核心 2-5）
setx WUSOEM_AFFINITY_MASK 0x3C

REM 最大核心數
setx WUSOEM_MAX_CORES 2

REM SYNC0 相位百分比
setx WUSOEM_SYNC0_PHASE_PCT 15
```

---

## 6. 硬體建議

### 6.1 網卡選擇
- **Intel I210/I211** 系列（良好的實時性能）
- **Intel I350** 系列
- 啟用 RSS（Receive Side Scaling）
- 停用節能模式

### 6.2 BIOS 設定
```
- 停用 C-States（CPU 節能狀態）
- 停用 Turbo Boost（避免頻率變化）
- 啟用 VT-d/IOMMU（減少 DMA 延遲）
- 停用 Hyper-Threading（部分情況下）
```

---

## 7. 驗證檢查清單

- [ ] `maxObservedCommsTime` < `desiredCycleTime / 2`
- [ ] Working Counter 穩定
- [ ] 無頁面錯誤（Page Faults）
- [ ] CPU 使用率 < 80%
- [ ] 無 DPC 延遲超過 100μs

---

## 參考資料
- Microsoft Windows IoT Enterprise Real-Time Capabilities
- SOEM Documentation
- Intel Ethernet Controller Optimization Guide
