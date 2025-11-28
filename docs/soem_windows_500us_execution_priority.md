# SOEM Windows 500us 可執行優先級清單

## 目的

- 在不削弱現有 PDO cycle 穩定性的前提下，逐步把 Windows + SOEM 架構推向更低 jitter 與更接近 500 us 的可預測行為。
- 這份清單只列值得投入的缺口，不重做目前專案已經具備的基礎優化。

## 目前已具備的基礎

以下能力已經存在，後續工作應建立在它們之上，而不是重寫同一層功能。

- 專用 EtherCAT cyclic thread，並設定高優先級與核心綁定。
- 以 QPC、高解析 waitable timer、必要時的 Sleep fallback 與短尾 TSC spin 維持週期。
- DC 基本配置、SYNC0 phase 設定與基礎漂移補償。
- VirtualLock 與 process working set 調整，降低 paging 干擾。
- 背景 StatusLogger 寫檔執行緒，避免同步磁碟 I/O 進入關鍵路徑。
- UI 讀取 PDO/機械量測資料時，已提供 lock-free cache 路徑。
- 已有核心隔離監控、RSS affinity helper、連線前網路預熱。
- PDO cyclic running 期間，SDO/mailbox 請求會在 caller thread 等待 master 發布的 idle window 作為起跑 gate，再以原本的呼叫 timeout 自行執行 mailbox；cyclic thread 只發布 active/deadline gate，不在 wait phase 執行 mailbox I/O。
- UI 已能顯示 comms time、observed cycle、max cycle 等基礎量測值。

參考實作：

- [src/ethercat/EtherCATMaster.Cyclic.cpp](../src/ethercat/EtherCATMaster.Cyclic.cpp)
- [src/ethercat/EtherCATMaster.Init.cpp](../src/ethercat/EtherCATMaster.Init.cpp)
- [src/core/CoreHelpers.cpp](../src/core/CoreHelpers.cpp)
- [src/core/CpuIsolation.cpp](../src/core/CpuIsolation.cpp)
- [src/io/StatusLogger.cpp](../src/io/StatusLogger.cpp)
- [src/ui/MainWindow.Timer.cpp](../src/ui/MainWindow.Timer.cpp)
- [src/app/main.cpp](../src/app/main.cpp)

## 執行原則

1. 先量測，再調參，不接受無量測依據的 timing 變更。
2. 每次只改一類 timing policy，避免多個來源同時影響 jitter。
3. 所有變更都必須保留現有 affinity、priority、timer 與 UI/非 RT 分流原則，除非該任務本身就是針對這些策略。
4. 每一項都至少要經過 Build: Incremental Crash、Smoke Test，以及實機 cycle 指標比對。
5. 不把額外記錄、阻塞 I/O、長鎖或額外 heap churn 帶入 cyclic path。

## 優先級總表

| Priority | 項目 | 目前狀態 | 預期產出 |
| --- | --- | --- | --- |
| P0 | 建立量測基線與回歸門檻 | 部分具備 | 可重跑的基線報告與驗收門檻 |
| P1 | DC PI controller | 初版已實作 | 可切換的 PI 漂移補償 |
| P2 | ETW / WPR jitter 與 interrupt latency 分析 | 未實作 | 一套可重現的外部量測流程 |
| P3 | NIC driver 設定稽核與半自動化 | 部分實作 | 稽核腳本與受控套用流程 |
| P4 | Cache alignment 與真正 prefetch 實驗 | 未實作 | 小範圍、可量測的 hot-path 微優化 |
| P5 | NUMA-aware memory | 未實作 | 只在特定硬體成立時才啟動 |

## P0: 建立量測基線與回歸門檻

**狀態**

- 部分具備，因為專案已經有 comms time、observed cycle、max cycle 等 runtime 指標，但尚未整理成固定的比較流程。

**為什麼先做**

- 如果沒有基線，後續 DC、NIC、cache 類修改很難判斷是改善還是單次運氣好。

**具體工作**

- 固定一組測試條件：NIC、cycle time、Game Coexistence 模式、DC 補償開關、代表性動作流程。
- 記錄 idle 與代表性動作兩種情境下的 comms time、observed cycle、max cycle、WKC 異常次數。
- 定義回歸門檻，例如 max cycle 不可比基線退步超過固定比例，且不可新增 watchdog 或 WKC 穩定性問題。
- 將量測步驟寫成文件或腳本，讓後續優化能重跑同一流程。

**完成條件**

- 有一份固定格式的基線報告。
- 有一組明確的驗收門檻。
- 後續每項 timing 優化都能對照同一套數據。

**驗證**

- Build: Incremental Crash
- Smoke Test
- 實機 idle run
- 實機代表性 motion run

## P1: DC PI controller

**狀態**

- 初版已實作。`EtherCATMaster` 已改為 bounded PI controller，加入 conditional integration anti-windup，並保留 legacy smoothing fallback 方便 A/B 比較。
- 後續主要剩下實機長時間 drift baseline、實際 gain 調參，以及與 ETW / interrupt-latency 量測交叉驗證。

**為什麼是最高優先**

- 這是距離文件目標最近、而且直接作用於長時間時脈漂移控制的缺口。

**具體工作**

- 將目前的平滑補償改為明確的 P 項與 I 項。
- 對積分項做 anti-windup，避免長時間累積過度修正。
- 對輸出修正量維持既有上限概念，例如限制在週期的一定比例內。
- 保留 feature flag 或參數化切換，方便與現行演算法 A/B 比較。
- 不引入任何新的 blocking、logging 或 heap allocation 到 cyclic path。

**建議修改區域**

- [include/EtherCATMaster.h](../include/EtherCATMaster.h)
- [src/ethercat/EtherCATMaster.Cyclic.cpp](../src/ethercat/EtherCATMaster.Cyclic.cpp)

**完成條件**

- 在長時間運行下，週期飄移或 max cycle 表現優於目前版本。
- WKC、slave state、fault 行為沒有退化。
- 可以快速切回舊策略做對照。

**驗證**

- Build: Incremental Crash
- Smoke Test
- 同一台機器、同一組條件下做 before/after drift run 比較

## P2: ETW / WPR jitter 與 interrupt latency 分析

**狀態**

- 未實作。

**為什麼排第二**

- 目前應用程式內部量測已足夠看結果，但還不夠判斷 jitter 是來自 scheduler、DPC、ISR、NIC driver，還是應用程式本身。

**具體工作**

- 建立一份 WPR/ETW 捕捉流程，用來覆蓋 connect、steady-state、motion 負載三段場景。
- 定義要看的欄位：ISR、DPC、context switch、CPU migration、timer latency。
- 把 ETW 的外部結果與目前 UI/QPC 量測值對照，建立故障定位流程。
- 優先做外部量測流程與說明，不要一開始就把 tracing 放進 cyclic loop。

**預期產出**

- 一份量測說明文件。
- 視需要增加一個輔助腳本，幫忙啟停 WPR 與整理輸出檔。

**完成條件**

- 能在同一台測試機上重現一次完整 capture。
- 能用這份流程區分系統層干擾和應用程式路徑干擾。

## P3: NIC driver 設定稽核與半自動化

**狀態**

- 部分實作。現有程式已能處理 EtherCAT adapter RSS affinity、adapter isolation，以及 connect-time 的 wireless IRQ rebalance：對 active WiFi / Bluetooth PAN adapter 會 best-effort 將 receive queues 移離 RT core，並盤點 Bluetooth PnP device 是否暴露標準 affinity policy 路徑；但尚未覆蓋文件提到的 Interrupt Moderation、EEE、Flow Control 等 driver 設定稽核。

**為什麼現在做**

- 文件中 NIC 調校是重要一環，但這類設定很依賴實際網卡與驅動版本，適合用受控腳本與稽核方式導入，而不是直接硬寫進主流程。

**具體工作**

- 先做 read-only 稽核腳本，列出目前 adapter 的 RSS、Interrupt Moderation、EEE、Flow Control 等狀態。
- 再視需要加入 opt-in 套用腳本，要求明確指定 adapter 與管理員權限。
- 對 TwinCAT 或不支援的 adapter，明確輸出不適用而不是假設失敗。
- 將套用前後量測流程與回復步驟寫清楚。

**現有基礎**

- [src/core/CoreHelpers.cpp](../src/core/CoreHelpers.cpp)
- [src/ui/MainWindow.Connect.cpp](../src/ui/MainWindow.Connect.cpp)

**完成條件**

- 能一鍵列出目前 NIC 優化狀態。
- 能選擇性套用支援的設定，且不會默默改錯 adapter。
- 套用前後有基線比較結果。

## P4: Cache alignment 與真正 prefetch 實驗

**狀態**

- 未實作。目前只有整理 hot-path metadata 的前置動作，尚未看到明確的 cache-line alignment 或 prefetch intrinsic 使用。

**為什麼不是前面就做**

- 這類優化收益通常小，而且很容易變成猜測式調整。應該等前面的量測與系統層定位完成後，再做小範圍實驗。

**具體工作**

- 先鎖定真正 hot 的資料結構，例如 timing atomics、UI cache、PDO metadata 暫存。
- 僅在有 before/after 證據時才引入 alignas 或 prefetch intrinsic。
- 避免大範圍重構與資料結構搬移。

**完成條件**

- 有明確 before/after 數據。
- 不引入可維護性明顯下降的複雜度。

## P5: NUMA-aware memory

**狀態**

- 未實作。

**為什麼排最後**

- 只有在目標硬體真的跨 NUMA node 或多 socket 時，這件事才有實際意義。對大多數單機 EtherCAT 控制機，它不是當前瓶頸。

**啟動條件**

- 實機硬體確認有 NUMA 拓樸影響。
- ETW 或其他量測顯示 remote-node latency 可能是瓶頸。

## 現階段不建議先做的事

- 不要先重新調 process/thread priority 策略。
- 不要把更多 log、診斷或同步 I/O 放進 cyclic path。
- 不要在沒有 ETW 或基線的情況下盲目追 cache/prefetch/NUMA。
- 不要因為追求 500 us 就撤掉目前已驗證的安全 fallback 與狀態檢查。

## 建議的下一個 Sprint 順序

1. 完成 P0 基線與驗收門檻。
2. 實作可切換的 P1 DC PI controller。
3. 建立 P2 ETW/WPR capture 流程。
4. 補齊 P3 NIC 稽核與受控套用腳本。