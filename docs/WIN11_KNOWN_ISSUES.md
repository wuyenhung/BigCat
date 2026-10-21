# Win11 特定問題與解決方案（2026-06-28）

本文記錄在 Win11 24H2 上發現的系統缺陷及其因應措施。
Win11 未來版本若修復以下問題，可用 Win10 相同方式簡化程式碼。

---

## 1. `Sleep(0)` 導致 30-40ms 隨機 Deschedule

**症狀**：CyclicTask 最大週期 40-75ms，平均 ~2ms。

**根因**：推測為 Win11 排程器在非完全隔離核心上對 yield 類 API 的行為改變，非微軟正式承認的缺陷。
- Win10：`Sleep(0)` 僅讓給同優先級 ready 執行緒，隔離核心上幾乎立即返回。
- Win11：在核心未完全隔離（外部 process affinity 包含 RT 核心）的前提下，
  `Sleep(0)` 返回後可能因 core parking 或排程器延遲導致 30-40ms 喚醒延遲。
  實測 `SwitchToThread()` 在此場景下延遲較低，但兩者 API 合約皆無即時保證；
  真正的根本解是系統層級 CPU 隔離（見「系統層級限制」一節）。

**修復**：TSC spin 中用 `SwitchToThread()` 取代 `Sleep(0)`。
```cpp
// 錯誤（Win11）：
Sleep(0);  // 可能 deschedule 30-40ms

// 正確：
SwitchToThread();  // 讓出剩餘時間片（實測隔離核心上延遲低於 Sleep(0)）
```
**適用範圍**：`CyclicTask` TSC spin 等待迴圈。

**若 Win11 修復**：可恢復 `Sleep(0)` 以降低 CPU 使用率。

---

## 2. PDOProcessingTask Busy-Wait Cache Line Ping-Pong

**症狀**：CyclicTask 最大週期 36-42ms，PDOProcessingTask 處理率正常（500Hz）。

**根因**：PDOProcessingTask busy-wait 連續讀取 `pdosSentCount`（atomic），
CyclicTask 每週期寫入同一變數。兩個核心反覆失效對方 cache line，造成記憶體匯流排競爭。
Win11 上 cache coherency 延遲比 Win10 更明顯。

**修復**：PDOProcessingTask 改用事件驅動同步。
```cpp
// 錯誤：
while (pdosSentCount.load(relaxed) == lastSeen) { YieldProcessor(); }

// 正確：
WaitForSingleObject(pdoCycleEvent, INFINITE);  // kernel 休眠，零 cache 流量
```
CyclicTask 在 PDO 交換後呼叫 `SetEvent(pdoCycleEvent)`（~1us overhead）。

**適用範圍**：PDOProcessingTask 週期同步。

**若 Win11 修復**：可恢復 busy-wait（更低延遲、無 kernel call）。

---

## 3. `SetWaitableTimer` + `WaitForMultipleObjects` 導致 EtherCAT OP 初始化失敗

**症狀**：EtherCAT 連線無法達到 OP（Operational）狀態。

**根因**：Win11 24H2 的 high-resolution waitable timer（`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`）
與 `WaitForMultipleObjects` 搭配使用時，在特定執行緒親和性與優先級組合下，
`WaitForMultipleObjects` 可能無法在計時器到期後及時返回，導致 CyclicTask
錯過 SOEM 狀態機的 PDO 交換時間窗。Win10 無此問題。

具體觸發條件：
  - 執行緒綁定到隔離 RT 核心（`SetThreadAffinityMask`）
  - 執行緒優先級為 `THREAD_PRIORITY_TIME_CRITICAL`
  - 使用 `SetWaitableTimer(..., CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, ...)`
  上述組合在 Win11 上會造成 `WaitForMultipleObjects` 不定時延遲 10-30ms，
  導致 `ecx_receive_processdata` 超時、WKC=0、OP 狀態機失效。

**修復**：Win11 不使用 waitable timer，改用原始的 `SwitchToThread()` 迴圈。
```cpp
// Win11 錯誤（共用 Win10 的 timer 策略）：
SetWaitableTimer(waitTimer, ...);
WaitForMultipleObjects(2, handles, FALSE, INFINITE);  // 可能延遲 10-30ms

// Win11 正確（保留原始 SwitchToThread 策略）：
if (remaining > 1000μs) { SwitchToThread(); continue; }
```
代價是 RT 核心 CPU 使用率約 95%，但因 RT 核心已從 process mask 排除，
不影響其他核心的遊戲/應用效能。

**適用範圍**：`CyclicTask` 週期等待迴圈。由 `!g_isWin11` guard 隔離。

**若 Win11 修復**：可移除 `!g_isWin11` guard，讓 Win11 共用 Win10 的 timer 策略
（預期 CPU 從 95% 降至 15%）。

**手動強制切換（不需改程式碼）**：`BigCat.ini` 新增 `[Compat] ForceWin10Mode`
（預設 `0` = 停用）。設為 `1` 時，`DetectOSVersion()` 會把 `g_isWin11` 覆寫為
`false`，等效於所有 `!g_isWin11` guard 皆成立 → 即使在 Win11 也採用 Win10 的
waitable timer + 200µs TSC margin 策略。用於：

- 釐清 Win10/Win11 行為差異（可與預設值做 A/B 比較）。
- 若未來 Win11 修復了 waitable timer 行為，可直接用此開關切換而無需重新編譯。

生效時機：`CyclicTask` 啟動（連線）時讀取一次，因此修改後需重新連線或重新啟動程式；
生效時 `logs/status.log` 會出現
`Compat: [Compat] ForceWin10Mode=1 - 已在 Win11 上強制採用 Win10 週期等待策略…`。
`tests/test_Ethercat_jog.cpp` 會讀取同一份 `BigCat.ini` 的同一鍵值，保持工具與主程式一致。

---

## 4. `std::atomic<double>` 可能使用內部 Mutex

**症狀**：`lastPdoHz.store(hz, relaxed)` 導致 CyclicTask 偶發性延遲。

**根因**：`std::atomic<double>` 在 MSVC x64 上 IS lock-free（`is_lock_free()` 傳回 true），
但 store 操作使用 `lock cmpxchg` 等 CAS 指令，在週期路徑上與其他執行緒
造成快取行競爭（cache line contention）而產生偶發延遲。問題不在 OS 版本，而是
應避免在即時執行緒路徑中使用 CAS 操作。

**修復**：避免在 CyclicTask 中使用 `std::atomic<double>`。
```cpp
// 錯誤：
std::atomic<double> lastPdoHz{0.0};
lastPdoHz.store(hz, relaxed);  // 可能 blocking

// 正確：改用整數表示（如 milli-Hz）
std::atomic<uint32_t> lastPdoMilliHz{0};
// 或：僅在非即時執行緒中讀寫
```
**適用範圍**：所有 CyclicTask 週期路徑上的 atomic 操作。

**若 Win11/MSVC 修復**：可恢復使用（確認 `is_lock_free()` 為 true）。

---

## 5. CyclicTask 內檔案 I/O 觸發磁碟 DPC

**症狀**：`pdo_diag.log` 寫入觸發 163ms 尖峰。

**根因**：`fopen`/`fputs`/`fclose` 觸發檔案系統驅動 DPC，
在未完全隔離核心上搶佔 CyclicTask。Win11 磁碟 DPC 延遲比 Win10 更長。

**修復**：CyclicTask 和 PDOProcessingTask 週期路徑上完全禁止檔案 I/O。
診斷資料改用 atomic 變數，由 UI/HTTP 執行緒非即時讀取。

**適用範圍**：所有即時執行緒（CyclicTask、PDOProcessingTask）。

---

## 6. PDOProcessingTask 自帶定時等待（HR Timer）在非 RT 核心 Oversleep

**症狀**：PDOProcessingTask 處理只需 ~100us，但整體週期 7.3ms（136Hz）。

**根因**：PDOProcessingTask 使用 `CreateWaitableTimerExW` + `WaitForMultipleObjects`
等待剩餘週期時間。在非 RT 核心上，Win11 排程器延遲導致 timer 醒來延遲 5ms+。

**修復**：移除 PDOProcessingTask 自帶定時等待，完全依賴 CyclicTask 的 `SetEvent` 同步。
PDOProcessingTask 處理完後立即回到 `WaitForSingleObject`。

**若 Win11 修復**：可恢復 HR timer 定時以降低 CyclicTask `SetEvent` 開銷。

---

## 7. `WakeConditionVariable` / `WaitOnAddress` 連結問題

**症狀**：`WaitOnAddress` 連結失敗（LNK2019）。

**根因**：MSVC 需要 `Synchronization.lib`，但在這個專案中不可用。

**修復**：使用 `SetEvent`/`WaitForSingleObject` 取代。

---

## 系統層級限制：CPU 隔離不完整

**症狀**：63 個外部 process affinity=0xFFFF（包含 RT 核心 CPU 14）。

**影響**：即使所有軟體層級最佳化到位，仍可能有偶發性 OS 層級搶佔（3-5ms）。
純軟體無法完全消除。

**建議**：使用 BCDEdit 或 `start /affinity` 在系統層級隔離 RT 核心。

---

## 當前最簡架構（Win11 妥協方案）

```
CyclicTask (CPU 14, TIME_CRITICAL):
  ├─ TSC chunked spin (500us) + SwitchToThread (DPC drain)
  ├─ PDO exchange (CS + send + recv)
  ├─ Atomic stores (no CAS loops, no atomic<double>)
  ├─ SetEvent(pdoCycleEvent) → 喚醒 PDOProcessingTask
  └─ NO Sleep(0), NO file I/O

PDOProcessingTask (其他核心):
  ├─ WaitForSingleObject(pdoCycleEvent) → 休眠等待
  ├─ 處理 (IO metadata, filter, IK, MMF, recording) ~100us
  ├─ ResetEvent(pdoCycleEvent)
  └─ NO busy-wait, NO file I/O, NO timer wait
```

---

## Win10 參考行為

以下機制在 Win10 上正常運作，可作為未來 Win11 修復後的回退目標：
| 機制 | Win10 | Win11 (目前) |
|------|-------|-------------|
| `Sleep(0)` 精確度 | ~0us (立即返回) | 30-40ms (不可用) |
| busy-wait cache ping-pong | 輕微影響 | 嚴重影響 (36ms+) |
| HR timer 精確度 | ~1ms | 5ms+ oversleep |
| `atomic<double>` lock-free | 是（lock cmpxchg，仍建議避開即時路徑） | 同 Win10（非 Win11 特有問題） |
