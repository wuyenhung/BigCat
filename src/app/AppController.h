#ifndef APP_CONTROLLER_H
#define APP_CONTROLLER_H

#include <string>
#include <cstdint>
#include <memory>
#include <functional>
#include "../include/core/EventBus.h"
#include "../include/core/SystemState.h"
#include "../include/core/IEtherCATService.h"
#include "../include/core/IMotionService.h"
#include "../include/MachineConfigIO.h"
// Forward declarations
class EtherCATMaster;
class MainWindow;

/**
 * @brief 應用程式核心協調器 (AppController)
 * 集中管理主流程入口、狀態快照與 HTTP/Auto 相關協調邏輯。
 *
 * 目前契約：
 * 1. 外部呼叫端以 AppController 作為連線/斷線、運動命令與自動流程入口。
 * 2. UI 與 HTTP 透過 AppController 取得單一狀態快照，不直接拼裝多來源狀態。
 * 3. MainWindow 保留視窗與既有流程實作，AppController 負責協調與生命週期順序。
 */
class AppController {
public:
    using StatusSnapshot = CoreHelpers::SystemStateSnapshot;

    static StatusSnapshot BuildStatusSnapshot(const AutoStatusSnapshot& autoStatus,
                                             bool masterInitialized,
                                             bool workerRunning) {
        return CoreHelpers::BuildSystemStateSnapshot(autoStatus, masterInitialized, workerRunning);
    }

    static bool CanStartAutoProcedure(const StatusSnapshot& snapshot) {
        return CoreHelpers::EvaluateStartAutoProcedure(snapshot).allowed;
    }

    static bool CanRequestAutoConnect(const StatusSnapshot& snapshot) {
        return CoreHelpers::EvaluateAutoConnect(snapshot).allowed;
    }

    static bool CanRequestAutoDownToOrigin(const StatusSnapshot& snapshot) {
        return CoreHelpers::EvaluateAutoDownToOrigin(snapshot).allowed;
    }

    static bool CanRequestAutoFaultReset(const StatusSnapshot& snapshot) {
        return CoreHelpers::EvaluateAutoFaultReset(snapshot).allowed;
    }

    static CoreHelpers::StatusEvent ToStatusEvent(const StatusSnapshot& snapshot) {
        return CoreHelpers::ToStatusEvent(snapshot);
    }

    static AppController& GetInstance();

    // 系統初始化，綁定現有的元件
    void Initialize(EtherCATMaster* master, MainWindow* mainWindow);

    // 連線生命週期
    void Connect(const std::string& ifname);
    void Disconnect();

    // 運動命令入口
    void FaultReset(bool assertFlag);
    void PowerOn(bool assertFlag);
    void PowerOff(bool assertFlag);
    void Home(bool assertFlag);

    void StartJogPositive();
    void StartJogNegative();
    void StopJog();

    // Shared request entry points used by PC UI, AutoWindow, web, and external inputs.
    bool RequestStartAutoProcedure(bool showWindow);
    void RequestStopAutoProcedure();
    bool RequestAutoConnect();
    bool RequestAutoDownToOrigin(uintptr_t requesterWindow = 0,
                                 bool notifyAutoWindowHomeReset = false);
    bool RequestAutoFaultReset(uintptr_t requesterWindow = 0);

    // 單一狀態快照
    StatusSnapshot GetStatusSnapshot() const;

    // HTTP / Auto 協調入口
    bool StartAutoProcedureFromHttp(bool showWindow);
    bool RequestAutoConnectFromHttp();
    void StopAutoProcedureFromHttp();
    void RequestAutoDownToOriginFromHttp();
    void AutoFaultResetAllFromHttp();

    CoreHelpers::EventSubscription SubscribeLogEvent(std::function<void(const CoreHelpers::LogEvent&)> handler);
    CoreHelpers::EventSubscription SubscribeStatusEvent(std::function<void(const CoreHelpers::StatusEvent&)> handler);
    void PublishLogEvent(const std::string& source, const std::string& message);
    void PublishStatusEvent();

    void BindInterfaces(IEtherCATService* ethercatService,
                        IMotionService* motionService,
                        IAutoStatusProvider* statusProvider);

    IEtherCATService* GetEtherCATService() const { return ethercatService_; }
    IMotionService* GetMotionService() const { return motionService_; }
    IAutoStatusProvider* GetAutoStatusProvider() const { return autoStatusProvider_; }

    // Accessor for integration points that still need the concrete master.
    EtherCATMaster* GetMaster() const { return master_; }

    // MMF1 input mode query (forwarded from EtherCATMaster, set once at startup)
    Mmf1InputMode GetMmf1InputMode() const;
    bool LoadMachineConfigXml(std::string& xmlContent,
                              std::string* errorMessage = nullptr) const;
    bool SaveMachineConfigXml(const std::string& xmlContent,
                              std::string* errorMessage = nullptr) const;
    bool TryLoadSavedMmf1InputMode(Mmf1InputMode& mode) const;
    bool SaveMmf1InputModeConfig(Mmf1InputMode mode) const;

private:
    AppController();
    ~AppController();

    // 禁用複製拷貝
    AppController(const AppController&) = delete;
    AppController& operator=(const AppController&) = delete;

private:
    EtherCATMaster* master_ = nullptr;
    MainWindow* mainWindow_ = nullptr;
    mutable CoreHelpers::EventBus eventBus_;
    IEtherCATService* ethercatService_ = nullptr;
    IMotionService* motionService_ = nullptr;
    IAutoStatusProvider* autoStatusProvider_ = nullptr;
    std::unique_ptr<IEtherCATService> defaultEthercatService_;
    std::unique_ptr<IMotionService> defaultMotionService_;
    std::unique_ptr<IAutoStatusProvider> defaultAutoStatusProvider_;
    bool useExternalBindings_ = false;
};

#endif // APP_CONTROLLER_H
