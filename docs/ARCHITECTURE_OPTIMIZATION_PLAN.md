# 架構優化項目（依現有專案重建）

本文件不沿用舊版優化條目，直接依目前 `WuBigCat` 專案現況重新盤點下一輪優化項目。

盤點時間：2026-03-07

注意：本文件的量化熱點與行數統計是當時的架構盤點快照，用來保留優化方向與優先序。若要描述目前程式行為或最新數字，應重新掃描程式碼並更新對應文件；不要把本文件的歷史統計當成自動同步的現況資料。

## 進展追蹤 | Progress Tracking

### 已完成工作項 (2026-03-08)

**P0-2 單一狀態模型進展：**
- ✅ 提取 `MainWindow::HasAnyTransientBusyFlag()` 輔助方法
  - 集中定義所有瞬時忙碌條件（disconnecting, homing, auto tasks, jog, fault reset, immediate stop, power op）
  - 在 `GetSystemStateSnapshot()` 和 `IsBusyOnlyBecauseConnect()` 中使用，消除重複邏輯
  - 建立狀態組合的唯一來源，降低規則漂移風險
  - 增量編譯 + 煙霧測試全部通過 (5/5 ✓)

- ✅ 提取 `MainWindow::AreAllSlavesReadyByStatusword()` 輔助方法
  - 集中定義 ready statusword 常數 (0x1637u = CiA402 Operation Enabled)
  - 將 `GetSystemStateSnapshot()` 和 `AutoWindow` 中重複的 statusword 迴圈邏輯合併
  - 建立 ready 狀態檢查的唯一來源，簡化 PPU 自動狀態邏輯
  - 增量編譯 + 煙霧測試全部通過 (5/5 ✓)

- ✅ 簡化 `AppController::StartAutoProcedureFromHttp()` 拒絕記錄
  - 移除 kMasterNotInitialized / kSystemBusy 特殊情況處理
  - 統一使用 DescribeCommandPolicyRejection 模式
  - 減少拒絕訊息重複，改善一致性
  - 增量編譯 + 煙霧測試全部通過 (5/5 ✓)

- ✅ 提取 `MainWindow::MarkConnectionWorkComplete()` 輔助方法
  - 集中定義連線工作完成時的狀態清理：`workerRunning.store(false); connectAttemptInProgress.store(false);`
  - 替換 11 個出現點（主要在 MainWindow.Connect.cpp 各個中止/失敗路徑中）
  - 建立連線狀態收尾的唯一來源，簡化錯誤流程管理
  - 增量編譯 + 煙霧測試全部通過 (5/5 ✓)

- ✅ 提取 `MainWindow::StartJogPreamble()` 輔助方法
  - 集中定義 Jog 操作的前置條件檢查與狀態標誌設置
  - 替換 StartJogPositiveUI() 和 StartJogNegativeUI() 中 5 行重複代碼（2 × 5 = 10 行重複）
  - 單一 jogBusy / jogPressRequested / PostAutoStatusUpdate 序列
  - 增量編譯 + 煙霧測試全部通過 (5/5 ✓)


### 進行中的工作項 (Next Priority)

**P0-2 進一步深化（機會識別中）**
- 評估 HTTP 命令端點中是否有其他可 consolidate 的模式（例如同一原子變數的重複操作、狀態轉換序列等）
- 檢視 AutoWindow 與 MainWindow 在某些狀態轉換邏輯中的重複實作

**P0-3 預備**
- MainWindow.Command.cpp（3028 行）與 AutoWindow.cpp（1769 行）擴充拆解候選
- MainWindow 內部狀態轉移函式（OnConnect/OnDisconnect/CompleteDisconnect）已分離，ConnectWorkflow 聚合機制已改善

**P0-1 深化**
- 評估是否進一步擴充 IEtherCATService / IMotionService 邊界
- 檢視 AppController 合約測試是否可涵蓋更多 connect/disconnect 路徑

## 現況摘要

- 目錄與 CMake target 已完成第一輪模組化：`core / io / security / ethercat / motion / app / server / ui` 已切開。
- 下一輪瓶頸已經不是「再拆資料夾」，而是「執行期耦合、狀態來源分散、執行緒生命週期不一致」。
- 目前量化熱點如下：
  - `src/ui/MainWindow.Command.cpp`：3028 行
  - `src/ui/MainWindow.cpp`：2511 行
  - `src/ethercat/EtherCATMaster.Cyclic.cpp`：1866 行
  - `src/ui/AutoWindow.cpp`：1769 行
  - `src/motion/MotionController.cpp`：1441 行
  - `src/ui/MainWindow.Connect.cpp`：1300 行
  - `src/ethercat/PDOFilter.cpp`：1142 行
  - `AppController::GetInstance().GetMaster()` 直接呼叫：102 處
  - `MainWindow::Master*` wrapper：47 個
  - `HttpServerInternal` 全域 `g_*` 狀態：47 個
  - `.detach()` 背景執行緒：21 處
  - `CMakeLists.txt` 已接入的 `add_test(...)`：8 個
- `AppController`、`IEtherCATService`、`IMotionService`、`IAutoStatusProvider`、`EventBus` 都已存在，表示抽象邊界已經開了頭，但呼叫端尚未完全收斂。
- `MainWindow::GetAutoGlobalStatus()` 仍標示為暫定實作，HTTP/API、Auto 流程與 UI enable/disable 判斷也還有重複邏輯。

## 優先順序

### P0-1 完成 `AppController` 邊界收斂

原因：
- 專案已經有 service interface，但 UI 仍大量直接抓 concrete `EtherCATMaster`。
- `MainWindow.Master.cpp` 目前主要是在做轉呼叫，表示抽象層尚未真正接管。

工作項目：
- 擴充 `IEtherCATService`，把目前仍由 `MainWindow::Master*` 包裝的必要能力正式收進 service 邊界。
- 讓 HTTP、UI、Auto 流程優先透過 `AppController` 或 use-case service 呼叫，不再直接依賴 `EtherCATMaster*`。
- 把 adapter 類別從 `AppController.cpp` 匿名區塊移出，整理成可重用的 binding 層。

完成標準：
- 不再新增新的 `GetInstance().GetMaster()` 使用點。
- `MainWindow.Master.cpp` 明顯縮水，剩下少量無法避免的 RT 專用入口。
- `AppController` 合約測試擴充到 connect/disconnect/auto action 路徑。

### P0-2 建立單一狀態模型與命令準則

原因：
- `busy / ready / error / home_done / online / master_initialized / worker_running` 目前由多處組合。
- API 驗證、Auto 規則、UI 按鈕狀態有重複判斷，容易出現規則漂移。

工作項目：
- 定義單一 `SystemStateSnapshot` 或同等級狀態模型。
- 定義 `CommandPolicy`，統一處理 connect、auto connect、fault reset、down-to-origin 等前置條件。
- 讓 AutoWindow、HTTP API、MainWindow 控制項 enable/disable 全部讀同一份狀態與 policy。

完成標準：
- `GetAutoGlobalStatus()` 不再是暫定聚合函式。
- HTTP 拒絕理由與 UI 按鈕禁用條件由同一份 policy 推導。
- 狀態轉移表有對應測試。

### P0-3 拆解 `MainWindow` 巨型協調責任

原因：
- `MainWindow` 相關檔案仍是全專案最大的熱點。
- 現在的問題不是「檔案太大」本身，而是視窗、流程、狀態、背景工作與業務規則仍綁在一起。

工作項目：
- 拆出 `ConnectWorkflow`、`MotionCommandController`、`StatusPresenter`、`TimerCoordinator` 或等價角色。
- 把 UI 控制項與面板狀態整理成 feature-specific state struct，不再全部堆在 `MainWindow_Internal.h`。
- 逐步把不需要公開的細節移出 `include/MainWindow.h`，降低 header 擴散。

完成標準：
- `MainWindow` 僅保留視窗生命週期、HWND 路由與協調入口。
- 新增功能時不再優先塞進 `MainWindow.Command.cpp`。
- `MainWindow` 最大單檔降到可持續維護的範圍。

### P1-4 讓 HTTP server 從全域狀態集合變成可管理元件

原因：
- `HttpServerInternal` 同時承載 socket、session、SSE、UI cache、mailbox、cycle info，責任過多。
- 目前 47 個 `g_*` 全域狀態讓 shutdown、測試隔離與多執行緒追查都變困難。

工作項目：
- 引入 `HttpServerRuntime` 或等價 owner object，承接目前 `g_*` 資源。
- 把 session、SSE、UI state、status cache、mailbox cache 分成子元件。
- HTTP route handler 只產生命令或查詢，不直接依賴 Win32 window 搜尋與 fallback 邏輯。

完成標準：
- server 啟停由單一物件生命週期控制。
- `HttpServer.Internal.h` 不再是全域狀態宣告清單。
- 保留現有 pure helper tests，另外補 runtime lifecycle tests。

### P1-5 移除大量 detached thread，統一背景工作生命週期

原因：
- 目前 `.detach()` 有 21 處，表示很多背景工作沒有明確 owner。
- 這會直接影響 disconnect、shutdown、crash dump、測試穩定性與資源回收。

工作項目：
- 建立 `TaskGroup`、`AsyncOperationRegistry` 或等價背景工作管理器。
- 將 UI 觸發的短任務、連線任務、自動流程任務、非 RT worker 分類管理。
- 每條背景工作都要有 stop/join 或明確 timeout/完成通知。

完成標準：
- 一般流程不再使用裸 `.detach()`。
- 關閉主程式、斷線、停止 auto 流程時可明確等待背景工作收斂。
- 補 shutdown/race smoke tests。

### P1-6 切開 EtherCAT RT 熱路徑與診斷/格式化工作

原因：
- `EtherCATMaster.Cyclic.cpp` 同時做 thread priority、affinity、IOmap、機構值換算、錯誤字串、診斷快取與 fallback。
- RT 路徑越肥，越難驗證 jitter、鎖競爭與例外情況。

工作項目：
- 切出 `RealtimeCycleEngine`、`CycleDiagnosticsPublisher`、`PdoOutputStager`、`MechanicalMetricsPublisher` 或等價模組。
- 熱路徑只保留必需的週期控制、資料交換、快取寫入。
- 字串格式化、檔案紀錄、非必要 fallback 計算搬到 RT 外圍。

完成標準：
- RT loop 不再直接負責大量文字組裝與非必要分支。
- cycle metrics 以 lock-free / snapshot 方式對外發布。
- cyclic 核心邏輯可以被獨立閱讀與驗證。

### P2-7 補齊測試矩陣，讓重構有保護網

原因：
- 目前測試已開始建立，但仍集中在 helper、數學與部分 API。
- 下一輪重構若沒有 state/policy/lifecycle tests，成本會很高。

工作項目：
- 補 `AppController` 行為測試與 command policy 測試。
- 補 HTTP server lifecycle、session cleanup、SSE 訂閱管理測試。
- 補背景工作 shutdown/race 測試。
- 把硬體依賴測試獨立標籤化，避免和純邏輯 smoke tests 混在一起。

完成標準：
- `ctest -L smoke` 可涵蓋核心狀態規則。
- `ctest -L server` 可涵蓋 API helper 以外的 runtime 行為。
- 重大重構先補測試再切模組，避免回歸只靠人工驗證。

## 建議執行順序

### 第一波

1. `P0-1` AppController 邊界收斂
2. `P0-2` 單一狀態模型與命令準則
3. `P0-3` MainWindow 協調責任拆解

目標：
- 先把依賴方向與狀態來源收斂，否則後面的 server 與 thread 重構會一直反覆返工。

### 第二波

1. `P1-4` HTTP server 物件化
2. `P1-5` 背景工作生命週期統一
3. `P1-6` EtherCAT RT 熱路徑瘦身

目標：
- 把最容易造成 race、shutdown 不穩與線上診斷困難的區塊收斂。

### 第三波

1. `P2-7` 測試矩陣補齊與 CI 標籤化

目標：
- 讓後續優化不再依賴人工反覆點 UI 或實機碰運氣驗證。

## 暫不列為本輪主項目的內容

以下項目目前不列為主優先，避免分散焦點：

- 再次大規模調整資料夾結構
- 單純為了拆檔而拆檔
- 單純追求 CMake 表面整潔，但不處理 runtime ownership
- 先做 UI 外觀重畫，卻不處理狀態模型與生命週期

## 下一步建議

若要開始實作，建議先從以下兩件事著手：

1. 先做 `SystemStateSnapshot + CommandPolicy`
2. 接著把 `MainWindow.Master.cpp` 的 wrapper 逐步轉成透過 service/use-case 呼叫

這兩步完成後，其餘重構的成本會明顯下降。
