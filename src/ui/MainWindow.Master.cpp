#include "MainWindow.h"
#include "../app/AppController.h"
#include "../include/EtherCATMaster.h"

// Centralized MainWindow-to-master accessors kept in a separate TU so
// heavier EtherCAT details do not leak into unrelated UI translation units.

bool MainWindow::ReadDeviceNameUtf8(std::string &outUtf8) {
    auto* master = AppController::GetInstance().GetMaster();
    if (!master) return false;
    char deviceName[256] = {0};
    int bufSz = (int)sizeof(deviceName);
    if (confirmedSlave <= 0) return false;
    if (master->ReadSDO(confirmedSlave, 0x1008, 0x00, deviceName, bufSz)) {
        outUtf8 = std::string(deviceName);
        return true;
    }
    return false;
}

int MainWindow::MasterGetSlaveCountPublic() const {
    if (auto* svc = AppController::GetInstance().GetEtherCATService()) {
        return svc->GetSlaveCount();
    }
    if (auto* master = AppController::GetInstance().GetMaster()) {
        return master->GetSlaveCount();
    }
    return 0;
}

bool MainWindow::MasterGetStatusword6041(int slaveIndex, uint32_t &outVal, bool &hasValue) {
    outVal = 0; hasValue = false;
    auto* master = AppController::GetInstance().GetMaster();
    if (!master) return false;
    if (slaveIndex <= 0) return false;

    std::vector<uint8_t> snap;
    if (!master->GetPDODataSnapshot(slaveIndex, snap) || snap.empty()) return false;

    // Try to find explicit 0x6041 mapping via pdoDisplay if available
    const PDOItem* statusItem = nullptr;
    for (const auto &it : pdoDisplay.pdoItems) {
        if ((int)it.index == 0x6041 && !it.isOutput) { statusItem = &it; break; }
    }

    int outputSize = 0;
    const ecx_contextt* ctx = master->GetContext();
    if (ctx && slaveIndex > 0 && slaveIndex <= ctx->slavecount) outputSize = ctx->slavelist[slaveIndex].Obytes;

    if (statusItem) {
        int bytes = (statusItem->bitLength > 0) ? ((statusItem->bitLength + 7) / 8) : 2;
        if (bytes > 4) bytes = 4;
        int idxInSnap = statusItem->isOutput ? statusItem->byteOffset : (outputSize + statusItem->byteOffset);
        if (idxInSnap >= 0 && idxInSnap + bytes <= (int)snap.size()) {
            uint32_t v = 0;
            for (int b = 0; b < bytes; ++b) v |= (uint32_t)snap[idxInSnap + b] << (8 * b);
            outVal = v; hasValue = true; return true;
        }
    }

    // fallback: read first two input bytes after outputs
    if (outputSize >= 0 && outputSize + 2 <= (int)snap.size()) {
        uint32_t sw = (uint32_t)snap[outputSize] | ((uint32_t)snap[outputSize + 1] << 8);
        outVal = sw; hasValue = true; return true;
    }

    return true; // snapshot present but no decoded value
}

bool MainWindow::AreAllSlavesReadyByStatusword() const {
    auto* master = AppController::GetInstance().GetMaster();
    if (!master) return false;

    int slaveCount = master->GetSlaveCount();
    if (slaveCount <= 0) return false;

    // CiA402 Operation Enabled: bits 0-6 = 0x37 (SO|QS|OE|VE|no-QS-active),
    // bit 9 = Remote.  Use a mask so manufacturer-specific bits (10, 12)
    // and target-reached (10) do not block the ready lamp.
    constexpr uint32_t kReadyMask  = 0x027Fu;  // bits 0-6 + bit 9
    constexpr uint32_t kReadyValue = 0x0237u;  // Operation Enabled + Remote

    for (int s = 1; s <= slaveCount; ++s) {
        uint32_t statusword = 0;
        bool hasVal = false;
        if (!const_cast<MainWindow*>(this)->MasterGetStatusword6041(s, statusword, hasVal) ||
            !hasVal || (statusword & kReadyMask) != kReadyValue) {
            return false;
        }
    }

    return true;
}

bool MainWindow::GetPDODataSnapshotWrapper(int slaveIndex, std::vector<uint8_t>& snapshot) {
    auto* master = AppController::GetInstance().GetMaster();
    if (!master) return false;
    return master->GetPDODataSnapshot(slaveIndex, snapshot);
}

int MainWindow::SnapshotIOMapWrapper(int offset, uint8_t* outBuf, int len) {
    auto* master = AppController::GetInstance().GetMaster();
    if (!master) return 0;
    return master->SnapshotIOMap(offset, outBuf, len);
}

bool MainWindow::GetSlaveIOBaseBytesWrapper(int slaveIndex, int &outBase, int &inBase) {
    auto* master = AppController::GetInstance().GetMaster();
    if (!master) return false;
    return master->GetSlaveIOBaseBytes(slaveIndex, outBase, inBase);
}

// Master wrappers used by Logic.cpp (hide EtherCATMaster from that TU)
void MainWindow::MasterSetCycleTime(uint32_t microseconds) {
    if (auto* master = AppController::GetInstance().GetMaster()) master->SetCycleTime(microseconds);
}

void MainWindow::MasterSetAutoDisconnectOnNetworkFailure(bool enable, int threshold) {
    if (auto* master = AppController::GetInstance().GetMaster()) master->SetAutoDisconnectOnNetworkFailure(enable, threshold);
}

    bool MainWindow::MasterIsAutoDisconnectOnNetworkFailureEnabled() const {
        if (auto* master = AppController::GetInstance().GetMaster()) {
            return master->IsAutoDisconnectOnNetworkFailureEnabled();
        }
        return false;
    }

    int MainWindow::MasterGetNetworkFailureThreshold() const {
        if (auto* master = AppController::GetInstance().GetMaster()) {
            return master->GetNetworkFailureThreshold();
        }
        return 5;
    }

bool MainWindow::MasterInitialize(const std::string &ifnameUtf8) {
    if (auto* master = AppController::GetInstance().GetMaster()) {
        return master->Initialize(ifnameUtf8.c_str());
    }
    return false;
}

std::string MainWindow::MasterGetLastError() const {
    if (auto* master = AppController::GetInstance().GetMaster()) {
        return master->GetLastError();
    }
    return std::string();
}

bool MainWindow::MasterConfigureSlaves() {
    if (auto* master = AppController::GetInstance().GetMaster()) {
        return master->ConfigureSlaves();
    }
    return false;
}

int MainWindow::MasterGetSlaveCount() const {
    if (auto* master = AppController::GetInstance().GetMaster()) {
        return master->GetSlaveCount();
    }
    return 0;
}

void MainWindow::MasterSendProcessDataOnce() {
    if (auto* master = AppController::GetInstance().GetMaster()) master->SendProcessDataOnce();
}

bool MainWindow::MasterStartCyclicOperation() {
    if (auto* master = AppController::GetInstance().GetMaster()) {
        return master->StartCyclicOperation();
    }
    return false;
}

bool MainWindow::MasterHasActiveSlaveThreads() const {
    if (auto* master = AppController::GetInstance().GetMaster()) {
        return master->HasActiveSlaveThreads();
    }
    return false;
}

bool MainWindow::MasterHasGlobalError() const {
    if (auto* master = AppController::GetInstance().GetMaster()) {
        return master->HasGlobalError();
    }
    return false;
}

void MainWindow::MasterShutdownWithCallback(const std::function<void(const std::string&)> &cb) {
    if (auto* master = AppController::GetInstance().GetMaster()) master->Shutdown(cb);
}

void MainWindow::MasterShutdownSync() {
    if (auto* master = AppController::GetInstance().GetMaster()) master->Shutdown();
}

void MainWindow::MasterResetMaxObservedCommsTime() {
    if (auto* master = AppController::GetInstance().GetMaster()) master->ResetMaxObservedCommsTime();
}

void MainWindow::MasterStopCyclicOperation() {
    if (auto* master = AppController::GetInstance().GetMaster()) master->StopCyclicOperation();
}

void MainWindow::MasterForceSlaveOperational(int slaveIndex, std::string &outLog) {
    if (auto* master = AppController::GetInstance().GetMaster()) master->ForceSlaveOperational((uint16_t)slaveIndex, outLog);
}

bool MainWindow::MasterAllSlavesOperationalNoAlarm() {
    if (auto* master = AppController::GetInstance().GetMaster()) {
        return master->AllSlavesOperationalNoAlarm();
    }
    return false;
}

bool MainWindow::MasterSendMailbox(uint16_t slave, uint8_t* data, int size) {
    if (auto* master = AppController::GetInstance().GetMaster()) {
        return master->SendMailbox(slave, data, size);
    }
    return false;
}

bool MainWindow::MasterReadSDO(uint16_t slave, uint16_t index, uint8_t subindex, void* data, int &size, int timeoutUs) {
    auto* master = AppController::GetInstance().GetMaster();
    if (!master) return false;
    if (timeoutUs < 0) return master->ReadSDO(slave, index, subindex, data, size, EC_TIMEOUTRXM);
    return master->ReadSDO(slave, index, subindex, data, size, timeoutUs);
}

bool MainWindow::MasterWriteSDO(uint16_t slave, uint16_t index, uint8_t subindex, void* data, int size) {
    if (auto* master = AppController::GetInstance().GetMaster()) {
        return master->WriteSDO(slave, index, subindex, data, size);
    }
    return false;
}

bool MainWindow::MasterDumpESI(uint16_t slave, std::string &dump) {
    auto* master = AppController::GetInstance().GetMaster();
    if (!master) return false;
    return master->DumpESI(slave, dump);
}

// MMF1_IN wrappers
bool MainWindow::MasterStartMMF1In() {
    auto* master = AppController::GetInstance().GetMaster();
    if (!master) return false;
    return master->StartMMF1In();
}

void MainWindow::MasterStopMMF1In() {
    auto* master = AppController::GetInstance().GetMaster();
    if (!master) return;
    master->StopMMF1In();
}

bool MainWindow::MasterIsMMF1InRunning() const {
    auto* master = AppController::GetInstance().GetMaster();
    if (!master) return false;
    return master->IsMMF1InRunning();
}

// MMF1 accessors
std::array<float,6> MainWindow::MasterGetMMF1Latest() {
    std::array<float,6> z{};
    auto* master = AppController::GetInstance().GetMaster();
    if (!master) return z;
    return master->GetMMF1Latest();
}

void MainWindow::MasterGetMMF1LatestWithVelocities(std::array<float,6>& outValues,
                                                   std::array<double,6>& outVelocities) {
    outValues.fill(0.0f);
    outVelocities.fill(0.0);
    auto* master = AppController::GetInstance().GetMaster();
    if (!master) return;
    master->GetMMF1LatestWithVelocities(outValues, outVelocities);
}

bool MainWindow::MasterMMF1IsMapped() {
    auto* master = AppController::GetInstance().GetMaster();
    if (!master) return false;
    return master->MMF1IsMapped();
}

bool MainWindow::MasterMMF1GetRawSnapshot(std::vector<uint8_t> &out) {
    auto* master = AppController::GetInstance().GetMaster();
    if (!master) return false;
    return master->MMF1GetRawSnapshot(out);
}

uint64_t MainWindow::MasterMMF1GetAndResetUpdateCount() {
    auto* master = AppController::GetInstance().GetMaster();
    if (!master) return 0;
    return master->MMF1GetAndResetUpdateCount();
}

uint64_t MainWindow::MasterMMF1GetAndResetReadCount() {
    auto* master = AppController::GetInstance().GetMaster();
    if (!master) return 0;
    return master->MMF1GetAndResetReadCount();
}

uint64_t MainWindow::MasterMMF1GetAndResetNonZeroReadCount() {
    auto* master = AppController::GetInstance().GetMaster();
    if (!master) return 0;
    return master->MMF1GetAndResetNonZeroReadCount();
}

bool MainWindow::MasterIsRunning() const {
    if (auto* master = AppController::GetInstance().GetMaster()) return master->IsRunning();
    return false;
}
uint32_t MainWindow::MasterGetCommsTime() const {
    if (auto* master = AppController::GetInstance().GetMaster()) return master->GetCommsTime();
    return 0;
}
uint32_t MainWindow::MasterGetMaxObservedCommsTime() const {
    if (auto* master = AppController::GetInstance().GetMaster()) return master->GetMaxObservedCommsTime();
    return 0;
}
uint32_t MainWindow::MasterGetCycleTime() const {
    if (auto* master = AppController::GetInstance().GetMaster()) return master->GetCycleTime();
    return 0;
}
uint32_t MainWindow::MasterGetActualCycleTime() const {
    if (auto* master = AppController::GetInstance().GetMaster()) return master->GetActualCycleTime();
    return 0;
}
uint32_t MainWindow::MasterGetObservedCycleMicros() const {
    if (auto* master = AppController::GetInstance().GetMaster()) return master->GetObservedCycleTimeUs();
    return 0;
}
uint32_t MainWindow::MasterGetMaxObservedCycleMicros() const {
    if (auto* master = AppController::GetInstance().GetMaster()) return master->GetMaxObservedCycleMicros();
    return 0;
}
void MainWindow::MasterResetMaxObservedCycleMicros() {
    if (auto* master = AppController::GetInstance().GetMaster()) master->ResetMaxObservedCycleMicros();
}
uint32_t MainWindow::MasterGetLastSpikePhase() const {
    if (auto* master = AppController::GetInstance().GetMaster()) return master->GetLastSpikePhase();
    return 0;
}
uint32_t MainWindow::MasterGetLastSpikeCycleUs() const {
    if (auto* master = AppController::GetInstance().GetMaster()) return master->GetLastSpikeCycleUs();
    return 0;
}
uint32_t MainWindow::MasterGetLastSpikeCommsUs() const {
    if (auto* master = AppController::GetInstance().GetMaster()) return master->GetLastSpikeCommsUs();
    return 0;
}
uint32_t MainWindow::MasterGetSpikeCount() const {
    if (auto* master = AppController::GetInstance().GetMaster()) return master->GetSpikeCount();
    return 0;
}
void MainWindow::MasterResetSpikeCounters() {
    if (auto* master = AppController::GetInstance().GetMaster()) master->ResetSpikeCounters();
}

uint32_t MainWindow::MasterGetWorkingCounter() const {
    if (auto* master = AppController::GetInstance().GetMaster()) return master->GetWorkingCounter();
    return 0;
}
uint64_t MainWindow::MasterGetPDOSentCount() const {
    if (auto* master = AppController::GetInstance().GetMaster()) return master->GetPDOSentCount();
    return 0;
}
double MainWindow::MasterGetPDOFrequencyHz() const {
    if (auto* master = AppController::GetInstance().GetMaster()) return master->GetLastPDOFrequencyHz();
    return 0.0;
}

double MainWindow::MasterGetMMF1UpdateFrequencyHz() const {
    if (auto* master = AppController::GetInstance().GetMaster()) return master->GetLastMMF1UpdateFrequencyHz();
    return 0.0;
}

int MainWindow::MasterGetSlaveOutputBytes(int slaveIndex) const {
    auto* master = AppController::GetInstance().GetMaster();
    if (!master) return -1;
    const ecx_contextt* ctx = master->GetContext();
    if (!ctx) return -1;
    if (slaveIndex <= 0 || slaveIndex > ctx->slavecount) return -1;
    return (int)ctx->slavelist[slaveIndex].Obytes;
}

bool MainWindow::MasterDetectWatchdogCondition() const {
    auto* master = AppController::GetInstance().GetMaster();
    if (!master) return false;

    // EC_NOFRAME (-1): NIC received no EtherCAT frame at all
    int32_t rawWkc = master->GetWorkingCounterRaw();
    if (rawWkc < 0) return true;

    uint32_t wkc = master->GetWorkingCounter();
    if (wkc == 0) return true;

    // Partial WKC: some slaves not responding but others still active.
    // This catches the scenario where WKC drops from expected (e.g. 18)
    // to a lower non-zero value (e.g. 10) — partial slave failure.
    uint32_t expected = master->GetExpectedWkc();
    if (expected > 0 && wkc > 0 && wkc < expected) return true;

    auto slaves = master->GetSlaveInfo();
    for (const auto &s : slaves) { if (s.state == 0x14) return true; }
    return false;
}

bool MainWindow::MasterGetMechanicalUnitMetrics(int slaveIndex, MechanicalUnitMetrics& out) {
    auto* master = AppController::GetInstance().GetMaster();
    if (!master) return false;
    return master->GetMechanicalUnitMetrics(slaveIndex, out);
}
