# AppController Interface Contract

更新日期: 2026-05-18

## 目的

AppController 是主流程的收斂入口，負責協調 MainWindow、EtherCATMaster、MotionController 與 HTTP/Auto 流程。現行實作仍由 MainWindow 保留 Win32 UI 與既有流程細節；AppController 負責對外入口、狀態快照、命令 policy 與生命週期順序。

## 對外可用介面

- Initialize(master, mainWindow)
  - 啟動期綁定既有元件。
  - 必須在 UI 建立完成、MainWindow 可用後呼叫。

- Connect(ifname)
  - 啟動連線流程。
  - ifname 為空時，使用 MainWindow 目前選定的介面。

- Disconnect()
  - 啟動斷線流程。
  - 呼叫端不應直接操作 EtherCATMaster 的 shutdown 流程。

- FaultReset / PowerOn / PowerOff / Home / StartJogPositive / StartJogNegative / StopJog
  - 統一的運動命令入口。
  - UI 或 HTTP 不應繞過 AppController 自行拼接流程。

- GetStatusSnapshot()
  - 提供單一狀態快照。
  - 呼叫端應優先使用此快照，而不是混合讀取 MainWindow 與 EtherCATMaster 狀態。

- RequestStartAutoProcedure / RequestStopAutoProcedure / RequestAutoConnect / RequestAutoDownToOrigin / RequestAutoFaultReset
  - PC UI、AutoWindow、Web 與外部輸入共用的 Auto 流程請求入口。
  - 這些入口負責進入共同 policy / pending gate，再委派 MainWindow 的既有流程實作。

- StartAutoProcedureFromHttp / RequestAutoConnectFromHttp / StopAutoProcedureFromHttp / RequestAutoDownToOriginFromHttp / AutoFaultResetAllFromHttp
  - HTTP 相容入口；目前轉呼叫上列共用 Auto 請求入口。

- CanStartAutoProcedure / CanRequestAutoConnect / CanRequestAutoDownToOrigin / CanRequestAutoFaultReset
  - 以 SystemStateSnapshot 評估共用 command policy。

- SubscribeLogEvent / SubscribeStatusEvent / PublishLogEvent / PublishStatusEvent
  - 事件匯流排入口，供狀態與 log 發布使用。

- BindInterfaces / GetEtherCATService / GetMotionService / GetAutoStatusProvider
  - 測試或替代實作可注入 service 介面；預設 adapter 仍包裝既有 MainWindow / EtherCATMaster 流程。

## 生命週期順序

1. 建立 EtherCATMaster 與 MainWindow。
2. 呼叫 AppController::Initialize(master, mainWindow)。
3. 新增或維護 UI / HTTP / Auto / 外部輸入時，優先透過 AppController 的 request helpers 進入主流程。
4. 需要狀態時，優先讀取 GetStatusSnapshot()。
5. 一般斷線請求呼叫 AppController::Disconnect()；視窗關閉/銷毀等強制同步 teardown 路徑可保留 MainWindow 內部處理。

## 邊界規則

- MainWindow 保留視窗、控制項與既有流程實作。
- AppController 負責入口收斂、跨模組協調與狀態彙整。
- 仍需直接存取 EtherCATMaster 的既有整合點，統一經由 AppController::GetMaster() 取得；新功能應優先擴充 service/request 介面，而不是新增散落的 concrete master 讀取。
- 不再提供 Win32Window / AutoProcedure 類型相容層。