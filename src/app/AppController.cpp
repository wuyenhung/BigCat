#include "AppController.h"
#include "../include/MainWindow.h"
#include "../include/EtherCATMaster.h"
#include "../include/core/CoreHelpers.h"
#include "../include/MachineConfigIO.h"
#include "../include/HttpServer.h"

namespace {

class EtherCATServiceAdapter final : public IEtherCATService {
public:
    explicit EtherCATServiceAdapter(EtherCATMaster* master) : master_(master) {}

    bool IsInitialized() const override {
        return master_ ? master_->IsInitialized() : false;
    }

    bool IsRunning() const override {
        return master_ ? master_->IsRunning() : false;
    }

    int GetSlaveCount() const override {
        return master_ ? master_->GetSlaveCount() : 0;
    }

    bool GetPdo60FD(std::vector<uint8_t>& out) override {
        if (!master_) return false;
        return master_->GetPdo60FD(out);
    }

    bool GetPdo60FDLowByte(uint8_t& outLowByte) override {
        if (!master_) return false;
        return master_->GetPdo60FDLowByte(outLowByte);
    }

    bool TryGetPDOChannelValuesLockFree(int slaveIndex, std::vector<PDOChannelValue>& outValues, uint64_t* outVersion) override {
        if (!master_) return false;
        return master_->TryGetPDOChannelValuesLockFree(slaveIndex, outValues, outVersion);
    }

    bool GetPDOChannelValues(int slaveIndex, std::vector<PDOChannelValue>& outValues) override {
        if (!master_) return false;
        return master_->GetPDOChannelValues(slaveIndex, outValues);
    }

    void SetMechanicalScalingFactors(int slaveIndex, double p1Scale, bool invertSign) override {
        if (!master_) return;
        master_->SetMechanicalScalingFactors(slaveIndex, p1Scale, invertSign);
    }

    bool TryGetMechanicalUnitMetricsLockFree(int slaveIndex, MechanicalUnitMetrics& outMetrics) override {
        if (!master_) return false;
        return master_->TryGetMechanicalUnitMetricsLockFree(slaveIndex, outMetrics);
    }

    bool GetMechanicalUnitMetrics(int slaveIndex, MechanicalUnitMetrics& outMetrics) override {
        if (!master_) return false;
        return master_->GetMechanicalUnitMetrics(slaveIndex, outMetrics);
    }

    void ClearGlobalError() override {
        if (master_) master_->ClearGlobalError();
    }

    void ResetPDOFilter(int slaveIndex) override {
        if (master_) master_->ResetPDOFilter(slaveIndex);
    }

    void ResetMaxObservedCommsTime() override {
        if (master_) master_->ResetMaxObservedCommsTime();
    }

    void ResetMaxObservedCycleMicros() override {
        if (master_) master_->ResetMaxObservedCycleMicros();
    }

private:
    EtherCATMaster* master_;
};

class MotionServiceAdapter final : public IMotionService {
public:
    explicit MotionServiceAdapter(MotionController* motion) : motion_(motion) {}

    MotionResult FaultResetAll() override {
        if (!motion_) return MotionResult{false, L"[Adapter] MotionController unavailable"};
        return motion_->FaultResetAll();
    }

    MotionResult PowerOnAllAsync(int slaveCount) override {
        if (!motion_) return MotionResult{false, L"[Adapter] MotionController unavailable"};
        return motion_->PowerOnAllAsync(slaveCount);
    }

private:
    MotionController* motion_;
};

class AutoStatusProviderAdapter final : public IAutoStatusProvider {
public:
    explicit AutoStatusProviderAdapter(MainWindow* window) : window_(window) {}

    AutoStatusSnapshot GetAutoStatusSnapshot() const override {
        if (!window_) return {};
        return CoreHelpers::ToAutoStatusSnapshot(window_->GetSystemStateSnapshot());
    }

private:
    MainWindow* window_;
};

} // namespace

// Singleton instance
AppController& AppController::GetInstance() {
    static AppController instance;
    return instance;
}

AppController::AppController() 
    : master_(nullptr), mainWindow_(nullptr) {
}

AppController::~AppController() {
}

void AppController::Initialize(EtherCATMaster* master, MainWindow* mainWindow) {
    master_ = master;
    mainWindow_ = mainWindow;

    // Load Stewart machine parameters and MMF1 input mode from Par/machine.xml
    if (master_) {
        WuSOEM::StewartParams machineParams;
        Mmf1InputMode mmf1Mode = Mmf1InputMode::ServoAngle;
        Mmf1PoseCoordinateSystem coordinateSystem = Mmf1PoseCoordinateSystem::World;
        WuSOEM::Pose6DLimits pose6dLimits;
        const std::string machineConfigPath = MachineConfigIO::GetDefaultMachineConfigPath();

        // Pre-check: if machine.xml is missing or corrupt, rebuild from built-in defaults
        // and prompt the user to restart. The rebuilt file won't take effect until restart.
        {
            std::string xmlContent;
            bool restartRequired = false;
            std::string rebuildError;
            if (!MachineConfigIO::LoadMachineConfigXmlWithDefaults(machineConfigPath, xmlContent, restartRequired, &rebuildError)) {
                if (restartRequired) {
                    HWND parent = mainWindow_ ? mainWindow_->GetHwnd() : NULL;
                    MessageBoxW(parent,
                                L"machine.xml 已遺失或損毀，已使用內建預設值重建。\r\n"
                                L"請重新啟動程式以使重建的設定檔生效。",
                                L"machine.xml 已重建",
                                MB_OK | MB_ICONWARNING);
                }
            }
        }

        if (MachineConfigIO::LoadMachineParams(machineConfigPath, machineParams, mmf1Mode, &coordinateSystem, &pose6dLimits)) {
            master_->SetStewartParams(machineParams);
            master_->SetMmf1InputMode(mmf1Mode);
            master_->SetMmf1PoseCoordinateSystem(coordinateSystem);
        } else {
            // Use defaults - still set them so seqlock snapshot is initialized
            master_->SetStewartParams(machineParams);
            master_->SetMmf1InputMode(mmf1Mode);
            master_->SetMmf1PoseCoordinateSystem(coordinateSystem);
        }
        // 總是設定 Pose6D 硬限制（來自 machine.xml 或預設值）
        master_->SetPose6DLimits(pose6dLimits);
        // 也推送給 HTTP server，讓 web API 端使用相同限制值
        SetStewartPose6DLimits(pose6dLimits);
    }

    // MainWindow::Create() may construct MotionController before AppController
    // is initialized, so its master pointer can be nullptr due to startup order.
    // Rebind the current master here to avoid runtime "master missing" errors.
    if (mainWindow_ && mainWindow_->motionController) {
        mainWindow_->motionController->SetMaster(master_);
    }

    defaultEthercatService_ = std::make_unique<EtherCATServiceAdapter>(master_);
    defaultMotionService_ = std::make_unique<MotionServiceAdapter>(mainWindow_ ? mainWindow_->motionController.get() : nullptr);
    defaultAutoStatusProvider_ = std::make_unique<AutoStatusProviderAdapter>(mainWindow_);

    if (!useExternalBindings_) {
        ethercatService_ = defaultEthercatService_.get();
        motionService_ = defaultMotionService_.get();
        autoStatusProvider_ = defaultAutoStatusProvider_.get();
    }
}

void AppController::Connect(const std::string& ifname) {
    if (mainWindow_) {
        if (ifname.empty()) {
            mainWindow_->OnConnect();
        } else {
            std::string name = ifname;
            mainWindow_->OnConnect(&name);
        }
    }
}

void AppController::Disconnect() {
    if (mainWindow_) {
        mainWindow_->OnDisconnect();
    }
}

void AppController::FaultReset(bool assertFlag) {
    if (mainWindow_) {
        mainWindow_->OnFaultReset(assertFlag);
    }
}

void AppController::PowerOn(bool assertFlag) {
    if (mainWindow_) {
        mainWindow_->OnPowerOn(assertFlag);
    }
}

void AppController::PowerOff(bool assertFlag) {
    if (mainWindow_) {
        mainWindow_->OnPowerOff(assertFlag);
    }
}

void AppController::Home(bool assertFlag) {
    if (mainWindow_) {
        mainWindow_->OnHome(assertFlag);
    }
}

void AppController::StartJogPositive() {
    if (mainWindow_) {
        mainWindow_->StartJogPositiveUI();
    }
}

void AppController::StartJogNegative() {
    if (mainWindow_) {
        mainWindow_->StartJogNegativeUI();
    }
}

void AppController::StopJog() {
    if (mainWindow_) {
        mainWindow_->StopJogUI();
    }
}

bool AppController::RequestStartAutoProcedure(bool showWindow) {
    if (!mainWindow_) {
        return false;
    }

    mainWindow_->SetAutoStopLockedToMainWindow(true);
    mainWindow_->StartAutoProcedure(showWindow);
    return true;
}

void AppController::RequestStopAutoProcedure() {
    if (!mainWindow_) {
        return;
    }

    if (!mainWindow_->CanExecuteAutoProcedureStopRequest()) {
        mainWindow_->PrependStatus(L"[AUTO] 僅在 oBusy 未動作時，才能執行自動程序停止\r\n");
        return;
    }

    mainWindow_->SetAutoStopLockedToMainWindow(false);
    mainWindow_->StopAutoProcedure();
}

bool AppController::RequestAutoConnect() {
    if (!mainWindow_) {
        return false;
    }

    const StatusSnapshot snapshot = GetStatusSnapshot();
    if (!CanRequestAutoConnect(snapshot)) {
        return false;
    }

    if (mainWindow_->autoConnectRequestPending.exchange(true)) {
        return false;
    }

    if (!PostMessageW(mainWindow_->hwnd, WM_USER_AUTO_STATUS_UPDATE, 1, 0)) {
        mainWindow_->autoConnectRequestPending.store(false);
        return false;
    }
    return true;
}

bool AppController::RequestAutoDownToOrigin(uintptr_t requesterWindow) {
    if (!mainWindow_) {
        return false;
    }

    const StatusSnapshot snapshot = GetStatusSnapshot();
    if (!CanRequestAutoDownToOrigin(snapshot)) {
        return false;
    }

    if (mainWindow_->autoDownToOriginRequestPending.exchange(true)) {
        return false;
    }

    if (!PostMessageW(mainWindow_->hwnd,
                      WM_USER_AUTO_DOWN_TO_ORIGIN_REQUEST,
                      static_cast<WPARAM>(requesterWindow),
                      0)) {
        mainWindow_->autoDownToOriginRequestPending.store(false);
        return false;
    }
    return true;
}

bool AppController::RequestAutoFaultReset(uintptr_t requesterWindow) {
    if (!mainWindow_) {
        return false;
    }

    if (mainWindow_->autoFaultResetRequestPending.exchange(true)) {
        return false;
    }

    if (!PostMessageW(mainWindow_->hwnd,
                      WM_USER_AUTO_FAULT_RESET_REQUEST,
                      static_cast<WPARAM>(requesterWindow),
                      1)) {
        mainWindow_->autoFaultResetRequestPending.store(false);
        return false;
    }
    return true;
}

AppController::StatusSnapshot AppController::GetStatusSnapshot() const {
    StatusSnapshot snapshot;
    if (!autoStatusProvider_) {
        return snapshot;
    }

    const auto autoStatus = autoStatusProvider_->GetAutoStatusSnapshot();
    return BuildStatusSnapshot(autoStatus,
                               ethercatService_ ? ethercatService_->IsInitialized() : false,
                               mainWindow_ ? mainWindow_->IsWorkerRunning() : false);
}

Mmf1InputMode AppController::GetMmf1InputMode() const {
    if (master_) return master_->GetMmf1InputMode();
    return Mmf1InputMode::ServoAngle;
}

bool AppController::LoadMachineConfigXml(std::string& xmlContent,
                                         std::string* errorMessage) const {
    return MachineConfigIO::LoadMachineConfigXml(
        MachineConfigIO::GetDefaultMachineConfigPath(),
        xmlContent,
        errorMessage);
}

bool AppController::SaveMachineConfigXml(const std::string& xmlContent,
                                         std::string* errorMessage) const {
    return MachineConfigIO::SaveMachineConfigXml(
        MachineConfigIO::GetDefaultMachineConfigPath(),
        xmlContent,
        errorMessage);
}

bool AppController::TryLoadSavedMmf1InputMode(Mmf1InputMode& mode) const {
    WuSOEM::StewartParams machineParams;
    return MachineConfigIO::LoadMachineParams(
        MachineConfigIO::GetDefaultMachineConfigPath(),
        machineParams,
        mode);
}

bool AppController::SaveMmf1InputModeConfig(Mmf1InputMode mode) const {
    return MachineConfigIO::SaveMmf1InputMode(
        MachineConfigIO::GetDefaultMachineConfigPath(),
        mode);
}

bool AppController::StartAutoProcedureFromHttp(bool showWindow) {
    return RequestStartAutoProcedure(showWindow);
}

bool AppController::RequestAutoConnectFromHttp() {
    return RequestAutoConnect();
}

void AppController::StopAutoProcedureFromHttp() {
    RequestStopAutoProcedure();
}

void AppController::RequestAutoDownToOriginFromHttp() {
    RequestAutoDownToOrigin(0);
}

void AppController::AutoFaultResetAllFromHttp() {
    uintptr_t requesterWindow = 0;
    if (mainWindow_ && mainWindow_->hAutoWindow && IsWindow(mainWindow_->hAutoWindow)) {
        requesterWindow = reinterpret_cast<uintptr_t>(mainWindow_->hAutoWindow);
    }
    RequestAutoFaultReset(requesterWindow);
}

void AppController::BindInterfaces(IEtherCATService* ethercatService,
                                   IMotionService* motionService,
                                   IAutoStatusProvider* statusProvider) {
    useExternalBindings_ = true;
    ethercatService_ = ethercatService ? ethercatService : defaultEthercatService_.get();
    motionService_ = motionService ? motionService : defaultMotionService_.get();
    autoStatusProvider_ = statusProvider ? statusProvider : defaultAutoStatusProvider_.get();
}

CoreHelpers::EventSubscription AppController::SubscribeLogEvent(std::function<void(const CoreHelpers::LogEvent&)> handler) {
    return eventBus_.Subscribe<CoreHelpers::LogEvent>(std::move(handler));
}

CoreHelpers::EventSubscription AppController::SubscribeStatusEvent(std::function<void(const CoreHelpers::StatusEvent&)> handler) {
    return eventBus_.Subscribe<CoreHelpers::StatusEvent>(std::move(handler));
}

void AppController::PublishLogEvent(const std::string& source, const std::string& message) {
    CoreHelpers::LogEvent evt;
    evt.source = source;
    evt.message = message;
    eventBus_.Publish<CoreHelpers::LogEvent>(evt);
}

void AppController::PublishStatusEvent() {
    eventBus_.Publish<CoreHelpers::StatusEvent>(ToStatusEvent(GetStatusSnapshot()));
}
