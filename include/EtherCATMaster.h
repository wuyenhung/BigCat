#ifndef ETHERCAT_MASTER_H
#define ETHERCAT_MASTER_H

// 必須按照特定順序引入標頭檔，避免衝突
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// HAVE_REMOTE and WPCAP are defined via CMake compile_definitions; avoid redefining them here
#define BPF_PROGRAM_DEFINED  // 防止 Packet32.h 重複定義 BPF 結構
#define BPF_INSN_DEFINED
#include <winsock2.h>
#include <windows.h>

#include <string>
#include <vector>
#include <functional>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <chrono>
#include <array>

// 在 Windows 標頭檔之後引入 SOEM
#include "soem/soem.h"
#include "MMFReader.h"
#include "MMFWriter.h"
#include "PDOModel.h"
#include "PDOFilter.h"
#include "StewartIK.h"
#include "MachineConfigIO.h"
#include "core/Cia402Mode.h"
#include "core/CpuIsolation.h"

// EtherCAT 從站資訊結構
struct SlaveInfo {
    int position;
    std::string name;
    uint16_t aliasAddress;
    uint32_t vendorId;
    uint32_t productCode;
    uint32_t revision; // eep_rev from EEPROM
    std::string ip;    // optional network identifier (may be empty)
    uint16_t state;
    int outputBits;
    int inputBits;
};

// PDO mapping entry decoded from SDO (index:subindex:bitlen)
struct PdoMappingEntry {
    uint16_t index = 0;
    uint8_t  subindex = 0;
    uint8_t  bitLength = 0;
};

// Actual PDO mapping status read from a slave via SDO
struct PdoMappingStatus {
    bool valid = false;
    uint16_t rxPdoAssigned = 0;    // e.g. 0x1600 or 0x1601 (from 0x1C12:01)
    uint16_t txPdoAssigned = 0;    // e.g. 0x1A00 or 0x1A01 (from 0x1C13:01)
    std::vector<PdoMappingEntry> rxEntries;
    std::vector<PdoMappingEntry> txEntries;
};

// EtherCAT 主站類別
class EtherCATMaster {
public:
    EtherCATMaster();
    ~EtherCATMaster();

    static const char* GetSimulationInterfaceName() { return "__SIM_ECAT6__"; }
    static const char* GetSimulationAdapterDisplayName() { return "模擬測試 / 模擬6軸 EtherCAT 虛擬電機驅動"; }
    static const char* GetLegacySimulationAdapterDisplayName() { return "模擬測試 / Virtual 6-Axis EtherCAT Drive"; }
    static bool IsSimulationInterfaceName(const std::string& ifname) { return ifname == GetSimulationInterfaceName(); }
    static bool IsSimulationAdapterDisplayName(const std::string& value) {
        return value == GetSimulationAdapterDisplayName() ||
               value == GetLegacySimulationAdapterDisplayName();
    }
    static bool IsSimulationAdapterSelection(const std::string& value) {
        return IsSimulationInterfaceName(value) || IsSimulationAdapterDisplayName(value);
    }
    static void SetPhysicalEthercatAllowed(bool allowed, const std::string& reason = std::string());
    static bool IsPhysicalEthercatAllowed();
    static std::string GetPhysicalEthercatRestrictionReason();
    bool IsSimulationMode() const { return simulationMode_.load(std::memory_order_relaxed); }

    // 初始化和關閉
    bool Initialize(const char* ifname);
    void Shutdown();
    void Shutdown(std::function<void(const std::string&)> logCallback);
    
    // 狀態管理
    bool ConfigureSlaves();
    bool StartCyclicOperation();

    // Performs a lightweight initialization on the specified network interface
    // without altering the persistent state of this object.  This is intended to
    // "warm up" the SOEM library and network driver so that when the real
    // connection is established later (via Initialize/ConfigureSlaves) the first
    // PDO cycles are less likely to suffer from latency/jitter caused by lazy
    // driver loading or cold caches.  The function simply calls ecx_init and
    // ecx_close on a temporary context; it does *not* set isInitialized on the
    // caller.
    static bool PreloadInterface(const char* ifname);
    void StopCyclicOperation();
    
    // PDO 操作
    bool ReadProcessData();
    bool WriteProcessData();
    
    // SDO 讀寫
    // Read SDO. timeoutUs is in microseconds; default uses EC_TIMEOUTRXM.
    bool ReadSDO(uint16_t slave, uint16_t index, uint8_t subindex, 
                 void* data, int &size, int timeoutUs = EC_TIMEOUTRXM);
    bool WriteSDO(uint16_t slave, uint16_t index, uint8_t subindex, 
                  void* data, int size);
    // Write SDO and read back to verify the written bytes. Returns true if verified.
    // Write SDO and read back to verify the written bytes. Returns true if verified.
    // timeoutUs: per-attempt timeout in microseconds; default is EC_TIMEOUTRXM
    bool WriteAndVerifySDO(uint16_t slave, uint16_t index, uint8_t subindex,
                           const void* data, int size, int maxRetries = 3,
                           int timeoutUs = EC_TIMEOUTRXM);
    
    // Mailbox 操作
    bool SendMailbox(uint16_t slave, uint8_t* data, int size);
    bool ReceiveMailbox(uint16_t slave, uint8_t* data, int* size);
    
    // 狀態查詢
    int GetSlaveCount() const { return slaveCount; }
    bool IsRunning() const { return isRunning; }
    bool IsInitialized() const { return isInitialized; }
    // 網路斷線強制關閉時使用：跳過正常 shutdown 流程直接標記未初始化
    void ForceMarkUninitialized() { isInitialized = false; }
    bool HasActiveSlaveThreads() const;
    bool HasGlobalError() const { return persistentGlobalError_.load(); }
    // Enable/disable recording of PDO 0x607A (logical target position) for diagnostics
    void EnablePDO607ARecording(bool enable) { pdo607aRecording_.store(enable, std::memory_order_relaxed); }
    void SetGlobalError() { persistentGlobalError_.store(true, std::memory_order_relaxed); }
    void ClearGlobalError() { persistentGlobalError_.store(false, std::memory_order_relaxed); }

    // Cyclic operation mode (CSP=8 / CSV=9).  Read from INI [Motion] OperationMode;
    // default is CSP.  This is the mode written to 0x6060 during PowerOn.
    int8_t GetCyclicOperationMode() const { return cyclicOperationMode_.load(std::memory_order_relaxed); }
    void SetCyclicOperationMode(int8_t mode) { cyclicOperationMode_.store(mode, std::memory_order_relaxed); }
    // Returns true if the configured cyclic mode is CSP (position-driven).
    bool IsCspMode() const { return GetCyclicOperationMode() == CoreHelpers::kCia402OperationModeCyclicSyncPosition; }
    std::vector<SlaveInfo> GetSlaveInfo();
    // Get matched ESI filename for a slave (1-indexed), empty if unknown
    std::string GetSlaveEsiFile(int slaveIndex) const;
    // Get ESI device name for a slave (1-indexed), empty if unknown
    std::string GetSlaveEsiDeviceName(int slaveIndex) const;
    std::string GetLastError() const {
        EnterCriticalSection(&errorCS_);
        std::string copy = lastError;
        LeaveCriticalSection(&errorCS_);
        return copy;
    }
    
    // 週期任務
    void SetCyclicCallback(std::function<void()> callback) { 
        cyclicCallback = callback; 
    }
    
    // 設定週期時間
    void SetCycleTime(uint32_t microseconds) { 
        desiredCycleTime = microseconds;
    }
    // 取得週期時間（微秒）
    uint32_t GetDesiredCycleTime() const {
        return desiredCycleTime;
    }
    // 啟用/停用 DC 漂移補償（依據 SOEM DCtime 做細微相位校正），預設為關閉
    void EnableDcDriftCompensation(bool enable) { enableDcDriftComp.store(enable, std::memory_order_relaxed); }
    // 切換 DC 漂移補償控制器；預設使用 PI controller，可回退到舊的 smoothing 行為做 A/B 比較。
    void SetDcPiControllerEnabled(bool enable) { useDcPiController.store(enable, std::memory_order_relaxed); }
    // 控制循環執行緒內的詳盡日誌（會影響抖動），預設為關閉
    void SetCyclicVerboseLogging(bool enable) { verboseCyclicLog = enable; }
    // 設定 DC SYNC0 相位百分比（10~20% 建議區間），預設 15%
    void SetDCSyncPhasePercent(int pct) {
        if (pct < 10) pct = 10;
        if (pct > 20) pct = 20;
        dcSyncPhasePercent = pct;
    }
    
    // ========================================================================
    // Low-Latency MMF Path: 將 MMF→StewartIK→PDO filter 整條運算鏈
    // 移入 CyclicTask（RT 核心），消除跨執行緒 staging 延遲。
    // PDO receive 後立即運算 IK+filter 並寫入 IOmap，同一週期 send 出去。
    // 預設啟用（PDOProcessingTask pin 到 RT 核心，同核心 cache 共享）。
    void SetLowLatencyMmfPath(bool enable) { lowLatencyMmfPath_.store(enable, std::memory_order_relaxed); }
    bool IsLowLatencyMmfPath() const { return lowLatencyMmfPath_.load(std::memory_order_relaxed); }

    
    // 診斷
    uint32_t GetCycleTime() const { return desiredCycleTime; }  // 返回期望的週期時間
    uint32_t GetActualCycleTime() const { return cycleTime; }   // 返回實際測量的週期時間
    uint32_t GetWorkingCounter() const { return (workingCounter < 0) ? 0u : (uint32_t)workingCounter; }
    // Raw accessor: returns the SOEM working counter as-is.
    // - EC_NOFRAME (-1) = NIC received no EtherCAT frame at all (NIC/driver issue)
    // - 0              = frame received but no slave processed it (slave/cable issue)
    // - >0             = normal operation
    int32_t GetWorkingCounterRaw() const { return workingCounter; }
    // Expected WKC for group 0: sum of outputsWKC + inputsWKC configured by SOEM.
    // Compare against GetWorkingCounterRaw() to detect partial slave failures.
    uint32_t GetExpectedWkc() {
        EnterCriticalSection(&cs);
        uint32_t exp = static_cast<uint32_t>(context.grouplist[0].outputsWKC +
                                              context.grouplist[0].inputsWKC);
        LeaveCriticalSection(&cs);
        return exp;
    }
    bool IsPDO607ARecordingEnabled() const { return pdo607aRecording_.load(std::memory_order_relaxed); }
    uint32_t GetCommsTime() const { return commsTime_.load(std::memory_order_relaxed); }        // 返回通訊工作時間 (send/receive only)
    uint64_t GetPDOSentCount() const { return pdosSentCount.load(std::memory_order_relaxed); }
    // Max observed comms time (updated from cyclic thread in a lock-free manner)
    uint32_t GetMaxObservedCommsTime() const { return maxObservedCommsTime.load(std::memory_order_relaxed); }
    uint32_t GetMaxRecvTimeUs() const { return maxRecvTimeUs_.load(std::memory_order_relaxed); }
    void ResetMaxObservedCommsTime() { maxObservedCommsTime.store(0, std::memory_order_relaxed); }

    // ── Cycle spike diagnostics ──
    // Records which phase (comms vs idle-wait) was dominant during the last
    // cycle-time spike (>5 ms).  Returns: 0=no spike, 1=comms phase, 2=idle phase.
    uint32_t GetLastSpikePhase() const { return lastSpikePhase_.load(std::memory_order_relaxed); }
    uint32_t GetLastSpikeCycleUs() const { return lastSpikeCycleUs_.load(std::memory_order_relaxed); }
    uint32_t GetLastSpikeCommsUs() const { return lastSpikeCommsUs_.load(std::memory_order_relaxed); }
    uint32_t GetSpikeCount() const { return spikeCount_.load(std::memory_order_relaxed); }
    void ResetSpikeCounters() {
        lastSpikePhase_.store(0, std::memory_order_relaxed);
        lastSpikeCycleUs_.store(0, std::memory_order_relaxed);
        lastSpikeCommsUs_.store(0, std::memory_order_relaxed);
        spikeCount_.store(0, std::memory_order_relaxed);
    }
    // Observed PDO/master cycle time in microseconds (most recently measured)
    uint32_t GetObservedCycleTimeUs() const { return observedCycleMicros_.load(std::memory_order_relaxed); }
    // Max observed cycle-to-cycle period in microseconds (high-water mark)
    uint32_t GetMaxObservedCycleMicros() const { return maxObservedCycleMicros_.load(std::memory_order_relaxed); }
    void ResetMaxObservedCycleMicros() { maxObservedCycleMicros_.store(0, std::memory_order_relaxed); }
    
    // PDO數據訪問
    uint8_t* GetIOMap() { return IOmap; }
    const ecx_contextt* GetContext() const { return &context; }
    // Thread-safe helper to obtain byte-aligned IO base offsets for a slave
    // Returns true if offsets were filled; outBase/inBase are in bytes
    bool GetSlaveIOBaseBytes(int slaveIndex, int &outBase, int &inBase);
        // Thread-safe snapshot of IOmap (copies requested range) - returns number of bytes copied
        int SnapshotIOMap(int offset, uint8_t* outBuf, int len);
        // Thread-safe write to output bytes within IOmap. Returns true if queued/written successfully.
        bool WriteOutputBytes(int absOffset, const uint8_t* data, int len);

    // PDO 即時資料處理 (核心執行緒)
    void StartPDOProcessingThread();
    void StopPDOProcessingThread();

    // Affinity allocator helper (returns a single logical-processor mask)
    static DWORD_PTR AllocateCoreMaskRoundRobin();
    // Return the CPU core mask assigned to the main CyclicTask thread (0 if unbound)
    DWORD_PTR GetCyclicThreadCoreMask() const { return cyclicThreadCoreMask_.load(std::memory_order_relaxed); }
    bool GetPDODataSnapshot(int slaveIndex, std::vector<uint8_t>& snapshot);
    bool WritePDOData(int slaveIndex, const std::string& itemName, int32_t value);
    // Stage output bytes into a persistent buffer that the PDO processing thread
    // will copy into IOmap every cycle. This avoids direct IOmap writes from UI threads.
    bool StageOutputBytes(int absOffset, const uint8_t* data, int len,
                          const char* callerFile = nullptr, int callerLine = 0, const char* callerFunc = nullptr);
    // Stage output bytes only if there is not already a staged entry at absOffset.
    // Returns true if the bytes were staged, false if an entry already existed.
    bool StageOutputIfNotExists(int absOffset, const uint8_t* data, int len,
                                const char* callerFile = nullptr, int callerLine = 0, const char* callerFunc = nullptr);
    // Register/display helpers for PDO table values (UI/display decoupling)
    void RegisterPDODescriptors(int slaveIndex, const std::vector<PDOChannelDescriptor>& descriptors);
    // Immediately compute pdoDisplayValues for one slave from current IOmap,
    // avoiding the one-cycle race between RegisterPDODescriptors erase and the
    // next async PDOProcessingTask pass.
    void RefreshPDODisplayValues(int slaveIndex);
    bool GetPDOChannelValues(int slaveIndex, std::vector<PDOChannelValue>& outValues);

    // ========================================================================
    // UI Performance: Lock-free accessors for PDO/Mechanical data
    // ========================================================================
    // These methods read from a double-buffered cache that is updated by the
    // RT thread without locks. UI can call these safely without blocking.
    // Returns true if data was available. outVersion can be compared between
    // calls to detect if data changed (avoids unnecessary UI redraws).
    bool TryGetPDOChannelValuesLockFree(int slaveIndex, std::vector<PDOChannelValue>& outValues, uint64_t* outVersion = nullptr);
    bool TryGetMechanicalUnitMetricsLockFree(int slaveIndex, MechanicalUnitMetrics& outMetrics, uint64_t* outVersion = nullptr);
    // Get the current UI cache version (for change detection in OnTimer)
    uint64_t GetUICacheVersion() const { return uiCacheVersion.load(std::memory_order_acquire); }

    // Mechanical unit helpers: master-side calculations delivered to the UI
    void SetMechanicalScalingFactors(int slaveIndex, double p1Scale, bool invertSign);
    bool GetMechanicalUnitMetrics(int slaveIndex, MechanicalUnitMetrics& outMetrics);

    // Check whether a name-based pending PDO output exists (enqueued via WritePDOData)
    bool HasPendingPDOOutput(const std::string& itemName);
    // Return last computed PDO frequency (Hz) using high-resolution perf counter
    double GetLastPDOFrequencyHz() const { return lastPdoHz.load(std::memory_order_relaxed); }
    // PDO processing task actual cycle counter (incremented per completed PDOProcessingTask cycle)
    uint64_t GetPdoProcCycleCount() const { return pdoProcCycleCount_.load(std::memory_order_relaxed); }
    uint32_t GetPdoProcElapsedUs() const { return pdoProcElapsedUs_.load(std::memory_order_relaxed); }
    // Read actual PDO mapping from slave via SDO (0x1C12/0x1C13 + PDO entries).
    // Returns true if mapping was successfully read; status is populated on success.
    bool ReadActualPdoMapping(int slaveIdx, PdoMappingStatus& status);

    // Dump ESI / EEPROM for a specific slave. Returns dump as UTF-8 string.
    bool DumpESI(uint16_t slave, std::string &outDump);
    // Try runtime initialization for a slave (SDO/runtime steps) to reach OP without writing EEPROM.
    // Returns true if slave reached OP.
    bool ForceSlaveOperational(uint16_t slave, std::string &outLog);
    // Check if all slaves are OP (0x8) with ALstatuscode==0.  Takes cs lock internally;
    // safe to call from watchdog recovery thread after cyclic has been stopped.
    bool AllSlavesOperationalNoAlarm();
    // Configure slave runtime PDO but don't transition to OP (stays in SAFE_OP)
    bool ConfigureSlaveRuntime(uint16_t slave, std::string &outLog);
    // Safely transition all slaves from OP to INIT before disconnecting
    void StopAllSlaves();
    // Send a single processdata roundtrip (SOEM standard practice before requesting OP)
    void SendProcessDataOnce();

    // PDO 0x60FD monitoring (was SDO reader; now PDO-cached every cycle)
    // StartSdoReader: kept for API compatibility; the actual 0x60FD value is
    //   now read from PDO every cycle and available via GetPdo60FD/GetPdo60FDLowByte.
    //   periodMs: ignored (PDO updates every cycle)
    //   logCb: optional callback to receive human-readable change/diagnostic messages
    bool StartSdoReader(unsigned int periodMs, const std::function<void(const std::string&)> &logCb);
    // Stop the 0x60FD monitor (no-op for PDO; kept for API compatibility)
    void StopSdoReader();
    bool IsSdoReaderRunning() const { return sdoReaderRunning.load(); }
    double GetSdoReaderLastObservedCycleMs() const { return sdoLastObservedCycleMs.load(); }
    // Return a copy of the last-read PDO 0x60FD value (bytes). Thread-safe.
    // Returns true if a value is available (non-empty), false otherwise.
    bool GetPdo60FD(std::vector<uint8_t>& out);
    // Return the low byte of the last-read PDO 0x60FD value without copying the full buffer.
    bool GetPdo60FDLowByte(uint8_t& outLowByte);
    // Per-slave PDO cache accessors (updated every cycle by CyclicTask).
    // slave: 1-based index. Returns true if cache is populated.
    bool GetPdo603F(uint16_t slave, uint16_t& outErrorCode) const;
    bool GetPdo6061(uint16_t slave, uint8_t& outModeDisplay) const;

    // ========================================================================
    // CoE Emergency (EMCY) Callback — 走 Mailbox 不受 PDO cycle time 影響
    // ========================================================================
    // 當從站發生錯誤時，主動推送 CoE Emergency 訊息到 Master。
    // Emergency 訊息走 Mailbox (SM0/SM1)，不依賴 PDO，可解決
    // 「PDO 逾時後用 PDO 讀不到 0x603F」的診斷死循環。
    //
    // 訊息結構 (CANopen DS301, 8 bytes after CoE header):
    //   [0:1] Emergency Error Code (U16 LE) → 等同 0x603F
    //   [2]   Error Register (U8)           → 等同 0x1001
    //   [3:7] Manufacturer-specific (5 bytes)

    struct EmcyInfo {
        uint16_t slave = 0;          // 1-based slave index
        uint16_t errorCode = 0;      // Emergency Error Code (= 0x603F value)
        uint8_t  errorRegister = 0;  // Error Register (= 0x1001 value)
        uint8_t  mfgField[5] = {};   // Manufacturer-specific raw bytes
        uint64_t timestamp = 0;      // QPC timestamp (QueryPerformanceCounter)

        // Decode common CiA402 error code meanings
        bool IsNoError()      const { return errorCode == 0x0000; }
        bool IsOverCurrent()  const { return errorCode == 0x2310 || errorCode == 0x3210; }
        bool IsOverVoltage()  const { return errorCode == 0x3110; }
        bool IsUnderVoltage() const { return errorCode == 0x3220; }
        bool IsOverTempMotor() const { return errorCode == 0x4310; }
        bool IsOverTempDrive() const { return errorCode == 0x4210; }
        bool IsFollowingError()const { return errorCode == 0x8611; }
        bool IsHomingError()  const { return errorCode >= 0x8A80 && errorCode <= 0x8A8F; }
        bool IsWatchdog()     const { return errorCode == 0x6010 || errorCode == 0x8130; }
        bool IsHallSensor()   const { return errorCode == 0x7380; }
        bool IsEncoderComms() const { return errorCode == 0x7388; }

        // Error Register bit tests (per DS301 Table 63)
        bool HasGenericError()      const { return (errorRegister & 0x01) != 0; }
        bool HasCurrentError()      const { return (errorRegister & 0x02) != 0; }
        bool HasVoltageError()      const { return (errorRegister & 0x04) != 0; }
        bool HasTemperatureError()  const { return (errorRegister & 0x08) != 0; }
        bool HasCommsError()        const { return (errorRegister & 0x10) != 0; }
        bool HasDeviceProfileError()const { return (errorRegister & 0x20) != 0; }
        bool HasManufacturerError() const { return (errorRegister & 0x80) != 0; }
    };

    // EMCY callback signature: invoked from CyclicTask (RT thread) when an
    // Emergency message is received.  Callbacks MUST be non-blocking and
    // allocation-free.  Use atomics or lock-free queues to forward to UI threads.
    using EmcyCallback = std::function<void(const EmcyInfo&)>;

    // Register an EMCY callback for a specific slave (1-based index).
    // Pass nullptr to unregister.  Callback is invoked from CyclicTask (RT core).
    void SetEmcyCallback(int slaveIndex, EmcyCallback callback);

    // Get the latest EMCY info for a slave (lock-free, safe from any thread).
    // Returns false if no EMCY has been received since last poll.
    // Pass consume=true to reset the "new data" flag after reading.
    bool GetLatestEmcyInfo(int slaveIndex, EmcyInfo& outInfo, bool consume = false);

    // Register a global EMCY callback (invoked for ALL slaves).
    void SetGlobalEmcyCallback(EmcyCallback callback);

    // Drain and process pending EMCY errors from SOEM's error ring buffer.
    // Called automatically from CyclicTask; exposed for manual diagnostics.
    void ProcessEmcyErrors();

    // Poll slave mailbox hardware (ESC SM0/SM1) for incoming CoE Emergency
    // messages.  Called by ProcessEmcyErrors with automatic throttling.
    void ServiceEmcyMailbox();

    // Dispatch a single EMCY event: update atomic cache + invoke callbacks.
    void DispatchEmcy(const EmcyInfo& info);

    // P1: Reset a slave's SDO mailbox counter to recover from persistent SDO failures.
    // Safe to call from any thread; acquires sdoCS internally.
    void ResetSlaveSDOState(uint16_t slave);
    // Enable/disable performing the SDO reads while reader thread runs
    void SetSdoReaderEnabled(bool enable) { sdoReaderEnabled.store(enable); }
    // Adjust polling period (ms) for the background SDO reader thread
    // SDO period is fixed at 10ms; ignore caller value to enforce constraint
    void SetSdoReaderPeriodMs(unsigned int /*ms*/) { sdoReaderPeriodMs.store(10); }
    unsigned int GetSdoReaderPeriodMs() const { return sdoReaderPeriodMs.load(); }

    // Network health / auto-disconnect controls
    // Enable automatic disconnect when repeated network problems (e.g., WKC==0) are detected
    // threshold: number of consecutive failing cycles before triggering auto-disconnect
    void SetAutoDisconnectOnNetworkFailure(bool enable, int threshold = 5);
    bool IsAutoDisconnectOnNetworkFailureEnabled() const { return autoDisconnectOnNetworkFailure.load(); }
    int GetNetworkFailureThreshold() const { return networkFailureThreshold; }

    // MMF 輸出控制（用於外部可視化）
    // 啟用/停用將 PDO 資料即時寫入 Memory-Mapped File
    // mappingName: MMF 名稱（例如 L"Local\\WuEtherCAT_PDO"）
    // 預設關閉，可由主視窗動態切換
    bool EnableMMFOutput(const std::wstring& mappingName = L"Local\\WuEtherCAT_PDO");
    void DisableMMFOutput();
    bool IsMMFOutputEnabled() const { return mmfOutputEnabled_.load(); }

    // ========================================================================
    // IK Actual 6-DOF Pose MMF Output ("BigCat")
    // 將正運動學計算的實際平台姿態即時寫入 Memory-Mapped File
    // mappingName: MMF 名稱，預設 L"Local\\BigCat"
    // 輸出頻率 = PDO 週期頻率，由 CyclicTask 在正運動學計算後寫入
    // ========================================================================
    bool EnablePoseMMFOutput(const std::wstring& mappingName = L"Local\\BigCat");
    void DisablePoseMMFOutput();
    bool IsPoseMMFOutputEnabled() const { return poseMmfEnabled_.load(std::memory_order_relaxed); }
    std::array<double, 6> GetPDOFilterInputAngles() const {
        std::array<double, 6> values{};
        for (size_t i = 0; i < values.size(); ++i) {
            values[i] = lastPdoFilterInputAngles_[i].load(std::memory_order_relaxed);
        }
        return values;
    }
    std::array<double, 6> GetPDOFilterInputVelocities() const {
        std::array<double, 6> values{};
        for (size_t i = 0; i < values.size(); ++i) {
            values[i] = lastPdoFilterInputVelocities_[i].load(std::memory_order_relaxed);
        }
        return values;
    }

    // Forward Kinematics: 6-DOF pose computed from actual PDO servo angles.
    // Written by PDOProcessingTask every cycle, read lock-free by SSE/MMF/UI.
    // outInitialHeight: Stewart home Z; subtract from outPose.z for relative height.
    bool GetForwardKinematicsResult(WuSOEM::Pose6D& outPose, bool& outValid, long double& outInitialHeight) const {
        outPose = forwardKinPose_;
        outValid = forwardKinValid_.load(std::memory_order_acquire);
        outInitialHeight = forwardKinInitialHeight_;
        return outValid;
    }
    bool IsForwardKinematicsValid() const {
        return forwardKinValid_.load(std::memory_order_acquire);
    }
    // Enable / disable per-cycle forward kinematics computation.
    // The result is meaningful only after all 6 axes complete homing;
    // set this to true once oHomeDone becomes true.
    void SetForwardKinematicsEnabled(bool enabled) {
        forwardKinEnabled_.store(enabled, std::memory_order_release);
    }
    bool IsForwardKinematicsEnabled() const {
        return forwardKinEnabled_.load(std::memory_order_acquire);
    }

    // Wait until PDO filters report velocities below `velThreshold` (mechanical units/sec)
    // or until `timeoutSec` seconds elapse. Returns true if settled before timeout.
    bool WaitForPDOFiltersToSettle(double velThreshold, double timeoutSec);

    // PDO 記錄（循環記錄於記憶體，單一寫入者：PDO 處理執行緒）
    // StartPDORecording: 開始在記憶體中循環記錄最新 PDO 值，預設容量 100MB
    // StopPDORecordingAndFlushToFile: 停止記錄、背景寫檔，完成後釋放錄製緩衝
    void StartPDORecording();
    void StopPDORecordingAndFlushToFile(const std::wstring& filePath);

private:
    // 內部函數
    static DWORD WINAPI CyclicThreadFunc(LPVOID param);
    void CyclicTask();
    // MMF→StewartIK→PDO filter pipeline executed on RT core inside CyclicTask.
    // Called between ecx_receive and ecx_send when lowLatencyMmfPath_ is active.
    void RunMmfIkPipelineInCyclicTask(const LARGE_INTEGER& qpcNow);
    // MMF receiver background thread: polls MMF1 on a non-RT core, writing
    // results via seqlock for lock-free reads by the RT cyclic path.
    static DWORD WINAPI MMFReceiverThreadFunc(LPVOID param);
    void MMFReceiverTask();
    void CheckSlaveStates();
    void SetError(const std::string& error);
    void InitializePDOData();  // 初始化 PDO 輸出數據
    // PDO recording helper: format and write one CSV line to circular buffer
    // Format: epoch_ms,uptime_ms,slave,MMF_IN,controlWord,statusWord,targetPos,targetVel,actualPos,actualVel,pdoFilterOutput,pdoFilterVel,pdoFilterAcc\n
    // Position and velocity are in mechanical units (mm, mm/s)
    void WritePDOLogEntry(int slaveIndex, uint64_t epochMs, uint64_t uptimeMs,
                          float mmfIn,
                          uint16_t controlWord, uint16_t statusWord,
                          double targetPos, double targetVel,
                          double actualPos, double actualVel,
                        double pdoFilterOutput, double pdoFilterVel, double pdoFilterAcc,
                        double filterTarget, int softStartActive, double filterTrackingError);
    
    // Per-slave cyclic thread support
    struct SlaveThreadData {
        uint16_t slaveIndex;  // 1-based slave index
        EtherCATMaster* master;
        HANDLE thread;
        HANDLE stopEvent;
        bool isRunning;
    };
    static DWORD WINAPI PerSlaveCyclicThreadFunc(LPVOID param);
    void PerSlaveCyclicTask(uint16_t slaveIndex, HANDLE slaveStopEvent);
    bool StartSlaveThread(uint16_t slaveIndex);
    void StopSlaveThread(uint16_t slaveIndex);

private:
    // SOEM Context (新版 API)
    ecx_contextt context;
    
    char ifname[256];
    int slaveCount;
    bool isRunning;
    bool isInitialized;
    // Per-slave matched ESI filename (1-indexed), empty if unknown
    std::vector<std::string> slaveEsiFiles_;
    // Per-slave ESI device name (1-indexed), empty if unknown
    std::vector<std::string> slaveEsiDeviceNames_;
    
    std::atomic<bool> lowLatencyMmfPath_{true}; // MMF→IK→PDO 預設在 CyclicTask RT 核心執行
    
    HANDLE cyclicThread;  // Legacy global cyclic thread (deprecated)
    HANDLE stopEvent;     // Legacy global stop event (deprecated)
    
    // Per-slave cyclic threads (new architecture)
    std::vector<SlaveThreadData> slaveThreads;
    
    uint32_t cycleTime;      // 微秒
    uint32_t desiredCycleTime; // 期望的週期時間（微秒）
    // internal working counter returned by SOEM ecx_receive_processdata
    // keep signed so SOEM's -1 can be handled; public getter converts negative to 0
    std::atomic<int32_t> workingCounter{0};  // atomic: read by UI/watchdog thread
    std::atomic<uint32_t> commsTime_{0}; // 通訊工作時間 (microseconds): SOEM send/receive only (atomic)
    std::atomic<uint64_t> pdosSentCount{0}; // PDO 發送計數 (atomic for lock-free increment)
    uint64_t operationStartTime; // 操作開始時間 (GetTickCount64())
    bool timePeriodActive; // whether timeBeginPeriod was requested
    // Previously recorded process priority class so we can restore it on stop
    DWORD prevProcessPriorityClass{0};
    bool prevProcessPriorityClassSet{false};
    // High-resolution timing for PDO sends (QueryPerformanceCounter ticks)
    uint64_t perfFreqTicks; // frequency returned by QueryPerformanceFrequency
    std::atomic<uint64_t> lastPdoPerfTicks{0}; // last observed perf counter tick at PDO send
    std::atomic<uint64_t> prevPdoPerfTicks{0}; // previous perf counter tick
    std::atomic<double> lastPdoHz{0.0}; // last computed PDO frequency in Hz (smoothed/instantaneous)

    // Atomic max tracker for commsTime, updated from cyclic thread to capture spikes
    std::atomic<uint32_t> maxObservedCommsTime{0};
    // Observed cycle-to-cycle period (microseconds), measured between consecutive PDO sends
    std::atomic<uint32_t> observedCycleMicros_{0};
    // Max observed cycle-to-cycle period (microseconds) - high-water mark
    std::atomic<uint32_t> maxObservedCycleMicros_{0};
    // CPU core mask assigned to the main CyclicTask thread (for avoidance by other threads)
    std::atomic<DWORD_PTR> cyclicThreadCoreMask_{0};
    
    std::string lastError;
    mutable CRITICAL_SECTION errorCS_;   // protects lastError from concurrent SetError/GetLastError
    std::function<void()> cyclicCallback;
    
    // IO 映射緩衝區
    uint8_t IOmap[4096];
    
    // ========================================================================
    // LOCK ORDERING HIERARCHY (must be followed by ALL code paths):
    //   1. cs            (general state / slave list)
    //   2. sdoCS         (SDO mailbox serialization)
    //   3. pdoFilterCS_  (PDO filter state, violation detection)
    //   4. pdoDataCS / pdoExchangeCS  (IOmap+stagedOutputs / SOEM port exchange)
    //   5. emcyCS_       (EMCY callback registration — UI thread only)
    //   6. errorCS_      (lastError diagnostic string)
    //   7. mmfOutputCS_  (MMF output writer — PDO slave data)
    //   8. poseMmfCS_    (IK actual pose MMF writer — BigCat)
    //   9. simulationCS_ (simulation adapter state)
    // NEVER acquire a lower-numbered lock while holding a higher-numbered one.
    // pdoFilterCS_ → pdoDataCS nesting is FORBIDDEN; PDOProcessingTask defers
    // all IOmap/stagedOutputs writes to after LeaveCriticalSection(&pdoFilterCS_).
    // pdoDataCS and pdoExchangeCS are the SAME level and MUST NEVER be held
    // simultaneously: CyclicTask takes pdoDataCS (IOmap), releases it, then
    // takes pdoExchangeCS (network I/O).
    // ========================================================================
    CRITICAL_SECTION cs;
    // SDO mailbox serialization - prevents concurrent SDO operations from different threads
    CRITICAL_SECTION sdoCS;
    std::atomic<bool> pdoExchangeActive_{false};
    std::atomic<bool> pdoPipelinePrimed_{false};  // set true after CyclicTask priming cycles succeed
    std::atomic<bool> inlinePdoProcessing_{true}; // Win11: merge PDO processing into CyclicTask (single-thread)
    std::atomic<uint64_t> pdoProcCycleCount_{0};   // PDO processing task completed cycles (diagnostic)
    std::atomic<uint32_t> pdoProcElapsedUs_{0};     // last PDO processing elapsed time (diagnostic)
    std::atomic<uint32_t> maxRecvTimeUs_{0};      // max SOEM recv time per cycle (diagnostic)
    std::atomic<uint32_t> pdoDiagCycleUs_{0};      // snapshot: current cycle time
    std::atomic<uint32_t> pdoDiagMaxCycleUs_{0};   // snapshot: max cycle time

    // ── Cycle spike phase tracking (updated by CyclicTask; read by UI/HTTP) ──
    std::atomic<uint32_t> lastSpikePhase_{0};       // 0=none, 1=comms-dominated, 2=idle-dominated
    std::atomic<uint32_t> lastSpikeCycleUs_{0};     // total cycle time during last spike (us)
    std::atomic<uint32_t> lastSpikeCommsUs_{0};     // comms component during last spike (us)
    std::atomic<uint32_t> spikeCount_{0};           // cumulative spike count since last reset
    std::atomic<long long> pdoIdleWindowDeadlineUs_{0};
    bool WaitForSdoIdleWindow(const std::chrono::steady_clock::time_point& deadline,
                              int minWindowUs,
                              int guardUs,
                              int& attemptTimeoutUs);
    bool ReadSDODirectLocked(uint16_t slave, uint16_t index, uint8_t subindex,
                             void* data, int& size, int timeoutUs,
                             bool reportFailure, std::string* errorMessage = nullptr);
    bool WriteSDODirectLocked(uint16_t slave, uint16_t index, uint8_t subindex,
                              void* data, int size, int timeoutUs,
                              bool reportFailure, std::string* errorMessage = nullptr);
    bool ReadSDODirect(uint16_t slave, uint16_t index, uint8_t subindex,
                       void* data, int& size, int timeoutUs,
                       bool reportFailure, std::string* errorMessage = nullptr);
    bool WriteSDODirect(uint16_t slave, uint16_t index, uint8_t subindex,
                        void* data, int size, int timeoutUs,
                        bool reportFailure, std::string* errorMessage = nullptr);
    bool ReadSDODuringIdleWindow(uint16_t slave, uint16_t index, uint8_t subindex,
                                 void* data, int& size, int timeoutUs,
                                 bool reportFailure, std::string* errorMessage = nullptr);
    bool WriteSDODuringIdleWindow(uint16_t slave, uint16_t index, uint8_t subindex,
                                  void* data, int size, int timeoutUs,
                                  bool reportFailure, std::string* errorMessage = nullptr);
    // P1: Internal reset of slave mailbox counter (caller must hold sdoCS)
    void ResetSlaveMbxcntLocked(uint16_t slave);
    
    // PDO 即時處理執行緒 (核心執行緒)
    HANDLE pdoProcessingThread;
    HANDLE pdoStopEvent;
    HANDLE pdoCycleEvent;   // auto-reset: CyclicTask signals after each PDO exchange
    bool pdoThreadRunning;
    static DWORD WINAPI PDOProcessingThreadFunc(LPVOID param);
    void PDOProcessingTask();
    struct MotionScaling {
        double scale = 1.0;
        bool invertSign = false;
    };
    void UpdateMechanicalUnitsForSlave(int slaveIndex, const std::vector<uint8_t>& snapshot,
                                       const std::vector<PDOChannelDescriptor>& descriptors,
                                       const MotionScaling& scaling);

    // Auto-tune affinity parameters
    // Auto-tune enabled by default: program will automatically adjust affinity
    bool autoTuneAffinity{true};
    uint32_t autoTuneJitterThreshold{200}; // microseconds (may be recomputed at init)
    uint32_t autoTuneIntervalMs{5000};
    uint64_t lastAffinityTuneMs{0};
    void AutoTuneAffinityIfNeeded();

    // 執行緒安全的 PDO 資料緩衝區
    CRITICAL_SECTION pdoDataCS;
    // Serializes the SOEM port PDO exchange (ecx_send/receive_processdata,
    // ecx_writestate, ecx_readstate) across threads.  CyclicTask holds it
    // across its (possibly blocking) send/receive so an external exchange
    // (connect-time OP request, ForceSlaveOperational) can never run
    // concurrently and corrupt the SOEM idxstack/tx/rx buffers.
    // MUST NOT be held together with pdoDataCS (see lock ordering above).
    CRITICAL_SECTION pdoExchangeCS;
    std::unordered_map<int, std::vector<uint8_t>> pdoDataSnapshots; // slaveIndex -> IO data snapshot
    std::unordered_map<std::string, std::pair<int, int32_t>> pendingPDOOutputs; // itemName -> (slaveIndex, value)
    std::unordered_map<int, std::vector<PDOChannelDescriptor>> pdoDisplayDescriptors;
    std::unordered_map<int, std::vector<PDOChannelValue>> pdoDisplayValues;
    std::unordered_map<int, MotionScaling> motionScalingFactors;
    std::unordered_map<int, MechanicalUnitMetrics> mechanicalUnitValues;
    // stagedOutputs: absolute IOmap offset -> data bytes that should be continuously applied by PDO task
    std::unordered_map<int, std::vector<uint8_t>> stagedOutputs;

    // ========================================================================
    // UI Performance Optimization: Lock-Free Double Buffer for UI Reads
    // ========================================================================
    // These buffers allow UI (OnTimer) to read PDO/Mechanical data without
    // blocking the real-time EtherCAT thread. The RT thread writes to write
    // buffers, then atomically swaps pointers. UI reads from read buffers.
    struct UIDisplayCache {
        std::unordered_map<int, std::vector<PDOChannelValue>> pdoValues;
        std::unordered_map<int, MechanicalUnitMetrics> mechMetrics;
        uint64_t updateCounter{0}; // incremented on each update
    };
    std::unique_ptr<UIDisplayCache> uiCacheBufferA;
    std::unique_ptr<UIDisplayCache> uiCacheBufferB;
    std::atomic<UIDisplayCache*> uiCacheRead{nullptr};  // UI reads from this (lock-free)
    std::atomic<UIDisplayCache*> uiCacheWrite{nullptr}; // RT thread writes to this
    std::atomic<uint64_t> uiCacheVersion{0}; // version counter for change detection

    // SDO reader background thread and synchronization (deprecated: 0x60FD now in PDO)
    std::thread sdoReaderThread;
    std::atomic<bool> sdoReaderRunning{false};
    std::atomic<bool> sdoReaderStop{false};
    std::mutex sdoReaderMutex;
    std::condition_variable sdoReaderCv;
    std::atomic<unsigned int> sdoReaderPeriodMs{10};
    std::atomic<bool> sdoReaderEnabled{true};
    std::atomic<bool> sdoReaderBusy{false};
    std::vector<uint8_t> sdoLastVal; // last observed SDO value (now populated from PDO)
    std::atomic<double> sdoLastObservedCycleMs{0.0};
    std::function<void(const std::string&)> sdoLogCb;
    // Last wall-clock ms when SDO reader performed an actual read
    std::atomic<uint64_t> sdoReaderLastRunMs{0};

    // PDO-based cache for 0x60FD Digital Inputs (updated every cycle from IOmap)
    // Slave-1 only; stored as raw 4-byte value (little-endian, as on wire)
    std::atomic<uint32_t> pdo60FDCache_{0};
    std::atomic<bool> pdo60FDValid_{false};

    // Per-slave PDO caches — updated every cycle by CyclicTask from IOmap inputs.
    // Indexed 1..slaveCount. Allocated in Initialize() after slave discovery.
    // These replace scattered per-call GetPDODataSnapshot + offset calc patterns.
    std::unique_ptr<std::atomic<uint16_t>[]> pdo603FCache_;   // 0x603F Error Code (2B)
    std::unique_ptr<std::atomic<uint8_t>[]>  pdo6061Cache_;   // 0x6061 Mode Display (1B)
    std::atomic<int> pdoCacheSlaveCount_{0};

    // ========================================================================
    // CoE Emergency (EMCY) per-slave cache — updated each cycle from SOEM's
    // error ring buffer.  Provides PDO-independent error diagnostics.
    // ========================================================================
    struct EmcySlot {
        std::atomic<uint16_t> errorCode{0};       // 0x603F Emergency Error Code
        std::atomic<uint8_t>  errorRegister{0};    // 0x1001 Error Register
        std::atomic<uint8_t>  mfgField[5]{};       // manufacturer-specific
        std::atomic<uint64_t> timestamp{0};        // QPC of last EMCY received
        std::atomic<bool>     hasNewData{false};    // true = unread EMCY available
    };
    std::unique_ptr<EmcySlot[]> emcyCache_;        // indexed 1..slaveCount
    EmcyCallback globalEmcyCallback_;              // invoked for all slaves
    std::vector<EmcyCallback> emcyCallbacks_;      // per-slave callbacks (1..slaveCount)
    CRITICAL_SECTION emcyCS_;                      // protects emcyCallbacks_ registration

    // Extract 0x60FD/0x603F/0x6061 from IOmap inputs into atomic caches.
    // Called from CyclicTask (after PDO receive) and SendProcessDataOnce.
    // Must be called while IOmap inputs are valid (inside or after pdoDataCS receive).
    void UpdatePdoInputCaches();

    // Runtime PDO byte-offset entry: read from actual driver via SDO after PDO config.
    struct PdoEntryOffset {
        int byteOffset = -1;  // -1 = not found
        int byteSize = 0;
    };

    // Per-slave runtime PDO byte offsets read from the actual driver via SDO
    // after PDO configuration. Key = CiA402 object index (e.g. 0x6040).
    // Negative byteOffset means "not in PDO".
    std::vector<std::unordered_map<uint16_t, PdoEntryOffset>> pdoRuntimeOffsets_;
    // Read actual PDO mapping from driver via SDO and populate pdoRuntimeOffsets_[slave].
    // Must be called while slave is in PRE_OP (SDO accessible).
    void ReadPdoOffsetsFromDriver(int slave);

public:
    // Const accessor for pdoRuntimeOffsets_
    const std::vector<std::unordered_map<uint16_t, PdoEntryOffset>>& GetPdoRuntimeOffsets() const { return pdoRuntimeOffsets_; }

    // PDO byte-offset constants for the 7-entry RxPDO / 7-entry TxPDO layout
    // (fallback when driver SDO read is unavailable; order matches Par/Startup.csv)
    static constexpr int kPdoRxByteSize = 17;
    static constexpr int kPdoTxByteSize = 19;
    // RxPDO (output) offsets:
    static constexpr int kPdoOutOff_TargetPos    = 0;   // 0x607A, 4B
    static constexpr int kPdoOutOff_TargetVel    = 4;   // 0x60FF, 4B
    static constexpr int kPdoOutOff_TargetTorque = 8;   // 0x6071, 2B
    static constexpr int kPdoOutOff_PosTorqueLim = 10;  // 0x60E0 (DVS) / 0x2019 (X1S), 2B
    static constexpr int kPdoOutOff_NegTorqueLim = 12;  // 0x60E1 (DVS) / 0x201A (X1S), 2B
    static constexpr int kPdoOutOff_Controlword  = 14;  // 0x6040, 2B
    static constexpr int kPdoOutOff_ModesOfOp    = 16;  // 0x6060, 1B
    // TxPDO (input) offsets:
    static constexpr int kPdoInOff_ActualPos     = 0;   // 0x6064, 4B
    static constexpr int kPdoInOff_ActualVel     = 4;   // 0x606C, 4B
    static constexpr int kPdoInOff_ActualTorque  = 8;   // 0x6077, 2B
    static constexpr int kPdoInOff_Statusword    = 10;  // 0x6041, 2B
    static constexpr int kPdoInOff_ErrorCode     = 12;  // 0x603F, 2B
    static constexpr int kPdoInOff_ModeDisplay   = 14;  // 0x6061, 1B
    static constexpr int kPdoInOff_DigitalInputs = 15;  // 0x60FD, 4B

    // X1S (Renesas ESC, vid=0x766) uses different SDO indices for torque limits
    // than DVS drives.  The 7-entry PDO byte layout is identical (offsets 10/12).
    static constexpr uint16_t kTorqueLimitPosIdxDvs = 0x60E0;
    static constexpr uint16_t kTorqueLimitNegIdxDvs = 0x60E1;
    static constexpr uint16_t kTorqueLimitPosIdxX1s = 0x2019;
    static constexpr uint16_t kTorqueLimitNegIdxX1s = 0x201A;
    static constexpr uint32_t kX1sVid = 0x766;
    static constexpr uint32_t kDvsVid = 0x445653;
    static bool IsX1sDrive(uint32_t eepMan) { return eepMan == kX1sVid; }
    static bool IsDvsDrive(uint32_t eepMan) { return eepMan == kDvsVid; }
    static uint16_t GetTorqueLimitPosIdx(uint32_t eepMan) { return IsX1sDrive(eepMan) ? kTorqueLimitPosIdxX1s : kTorqueLimitPosIdxDvs; }
    static uint16_t GetTorqueLimitNegIdx(uint32_t eepMan) { return IsX1sDrive(eepMan) ? kTorqueLimitNegIdxX1s : kTorqueLimitNegIdxDvs; }

    // CiA402 homing method (0x6098) per drive type.
    // DVS (0x445653): method 35 (homing on current position).
    // X1S (0x766):    method 35 (homing on current position).
    static constexpr int8_t kHomingMethodDvs = 35;
    static constexpr int8_t kHomingMethodX1s = 35;
    static int8_t GetHomingMethod(uint32_t eepMan) { return IsX1sDrive(eepMan) ? kHomingMethodX1s : kHomingMethodDvs; }

public:
    // Write 0x6060 Modes of Operation: prefers PDO descriptor-cache lookup
    // (drive-model-aware byte offset); falls back to Obytes heuristic, then SDO.
    bool WriteModeOfOperation(uint16_t slave, uint8_t mode);

    // Perform one tick for the SDO reader; intended to be called periodically
    // by an external thread (e.g. a consolidated error-check thread).
    // DEPRECATED: 0x60FD is now read from PDO every cycle; this is a no-op.
    void SdoReaderTick();

    // MMF1_IN reader: provide an API so UI can ask EtherCATMaster to start/stop the
    // background MMF reader. This centralizes lifetime and avoids duplicating
    // MMFReader between UI and master logic.
public:
    // Start/Stop MMF1_IN reader. Returns true when started successfully.
    bool StartMMF1In();
    void StopMMF1In();
    bool IsMMF1InRunning() const { return mmf1ReaderRunning.load(); }
    // Start/Stop the dedicated MMF receiver thread (non-RT core, runs PollOnce
    // independently of the PDO cycle so MMF arrival jitter never disturbs the
    // EtherCAT control loop).
    bool StartMMFReceiver();
    void StopMMFReceiver();
    bool IsMMFReceiverRunning() const { return mmfReceiverRunning_.load(std::memory_order_relaxed); }
    // MMF1 accessors (forwarded to internal MMFReader)
    std::array<float,6> GetMMF1Latest() const { return mmf1Reader.GetLatest(); }
    std::array<double,6> GetMMF1LatestVelocities() const { return mmf1Reader.GetLatestVelocities(); }
    void GetMMF1LatestWithVelocities(std::array<float,6>& outValues, std::array<double,6>& outVelocities) const {
        std::array<uint64_t, 6> ignoredUpdateCounts{};
        mmf1Reader.GetLatestWithUpdateCountsAndVelocities(outValues, ignoredUpdateCounts, outVelocities);
    }
    bool MMF1IsMapped() const { return mmf1Reader.IsMapped(); }
    bool MMF1GetRawSnapshot(std::vector<uint8_t> &out) const { return mmf1Reader.GetRawSnapshot(out); }
    uint64_t MMF1GetAndResetUpdateCount() { return mmf1Reader.GetAndResetUpdateCount(); }
    uint64_t MMF1GetAndResetReadCount() { return mmf1Reader.GetAndResetReadCount(); }
    uint64_t MMF1GetAndResetNonZeroReadCount() { return mmf1Reader.GetAndResetNonZeroReadCount(); }
    // Return last computed MMF1 update frequency in Hz
    double GetLastMMF1UpdateFrequencyHz() const { return mmf1Reader.GetLastUpdateFrequencyHz(); }

    // ========================================================================
    // MMF1 Input Mode & Stewart IK Parameters
    // ========================================================================
    // StewartParams snapshot for lock-free cyclic thread reads (seqlock pattern)
    struct StewartParamsSnapshot {
        WuSOEM::StewartParams params;
        long double initialHeight = 450.0L;
    };
    // Set Stewart machine params (writer side, non-cyclic thread). Recomputes initialHeight.
    void SetStewartParams(const WuSOEM::StewartParams& p);
    // Lock-free seqlock read for cyclic thread
    StewartParamsSnapshot ReadStewartParamsSnapshot() const;
    // MMF1 input mode (set once at startup, read by cyclic thread)
    void SetMmf1InputMode(Mmf1InputMode m) { mmf1InputMode_ = m; }
    Mmf1InputMode GetMmf1InputMode() const { return mmf1InputMode_; }
    void SetMmf1PoseCoordinateSystem(Mmf1PoseCoordinateSystem c) { mmf1PoseCoordinateSystem_ = c; }
    Mmf1PoseCoordinateSystem GetMmf1PoseCoordinateSystem() const { return mmf1PoseCoordinateSystem_; }

    // ========================================================================
    // Pose6D 前置驗證層 (硬限制)
    // ========================================================================
    // Set pose limits for the real-time validation layer. If the incoming Pose6D
    // exceeds any axis, the cyclic thread will freeze motion and set a warning flag.
    void SetPose6DLimits(const WuSOEM::Pose6DLimits& limits);
    WuSOEM::Pose6DLimits GetPose6DLimits() const;
    // Poll warning state from the UI thread (lock-free atomic reads).
    // Returns true if the pose limit warning is currently active (violation in
    // the most recent cycle). The UI thread should beep when this transitions to true.
    bool IsPose6DLimitWarningActive() const { return pose6dLimitWarning_.load(std::memory_order_relaxed); }
    // Retrieve and consume the last violation message (thread-safe, atomic swap with empty).
    // Returns a wstring that may be empty if no new violation occurred since last call.
    std::wstring ConsumePose6DLimitWarningMessage();

    // Pose6D 動態行程縮放（Z 位置觸發）警告狀態
    bool IsPose6DDynamicScalingActive() const { return pose6dDynamicScaling_.load(std::memory_order_relaxed); }
    std::wstring ConsumePose6DDynamicScalingMessage();

    // PDO濾波器控制 (加速度/減速度限制)
    // slaveIndex: 從站索引 (1-based)
    // maxVel: 最大速度 (角度/秒) - 對應 P3
    // maxAccel: 最大加速度 (角度/秒²) - 對應 P4
    // maxDecel: 最大減速度 (角度/秒²) - 對應 P5
    // jerk: 加加速度 (角度/秒³) - 對應 P6
    void ConfigurePDOFilter(int slaveIndex, double maxVel, double maxAccel, double maxDecel, double jerk,
                            double velocityViolationThresholdScale = PDOFilter::DEFAULT_VELOCITY_VIOLATION_THRESHOLD_SCALE,
                            double accelerationViolationThresholdScale = PDOFilter::DEFAULT_ACCEL_VIOLATION_THRESHOLD_SCALE,
                            double velocityViolationDelaySec = PDOFilter::DEFAULT_VELOCITY_VIOLATION_DELAY_SEC,
                            double accelerationViolationDelaySec = PDOFilter::DEFAULT_ACCEL_VIOLATION_DELAY_SEC);
    // Set per-slave PDO position limits (mechanical units converted by caller)
    void SetPDOFilterLimits(int slaveIndex, double minPos, double maxPos);
    // Set the target deceleration for the PDO filter of a slave (applies immediately).
    // If the slave has no PDOFilter configured yet, it will be created.
    // NOTE: ramp-time support was removed; callers must manage gradual changes externally.
    void SetPDOFilterTargetDeceleration(int slaveIndex, double targetDecel);
    // Query the configured max deceleration for a slave (the configured P5 value)
    double GetConfiguredPDOFilterMaxDeceleration(int slaveIndex) const;
    // Query whether the PDOFilter for a slave is currently in soft-start
    bool IsPDOFilterSoftStartActive(int slaveIndex) const;
    // Return the soft-start acceleration ratio used by PDOFilter (e.g. 0.2)
    double GetPDOFilterSoftStartAccelRatio() const;
    // Retrieve PDO filter violation thresholds for diagnostics. Returns false if invalid slaveIndex.
    bool GetPDOFilterViolationThresholds(int slaveIndex, double &velThreshold, double &accelPosThreshold, double &accelNegThreshold) const;
    bool GetPDOFilterTrackingErrorLimit(int slaveIndex, double &trackingErrorLimit) const;
    bool GetPDOFilterState(int slaveIndex, PDOFilter::ViolationType &violationType, bool &isError, bool &isTrackingErrorFatal) const;
    bool GetPDOLimits(int slaveIndex, double &minPos, double &maxPos) const;
    // Return a list of slaves that currently have hard-limit violations (min or max).
    // Each pair contains (slaveIndex, violationType). Returns true if any found.
    bool GetSlavesWithHardLimitViolations(std::vector<std::pair<int, PDOFilter::ViolationType>> &out) const;
    // 查詢全局違規狀態
    bool IsGlobalLimitingEnabled() const { return globalLimitingEnabled_.load(std::memory_order_relaxed); }
    // 重置指定從站的濾波器狀態
    void ResetPDOFilter(int slaveIndex);
    // 啟用/停用 PDO 濾波器功能
    // 安全機制：嘗試啟用時會檢查全域錯誤旗標、master 是否已初始化且從站處於 OP，
    // 以及是否至少有一個已配置的 PDOFilter。若任一檢查失敗，啟用請求會被忽略並
    // 以 SetError 記錄原因。停用則總是允許（且立即停用）。
    void EnablePDOFiltering(bool enable) {
        if (enable) {
            // Block enabling if persistent global error reported by filters
            if (persistentGlobalError_.load(std::memory_order_relaxed)) {
                SetError("EnablePDOFiltering: blocked due to persistent global PDO filter error");
                return;
            }
            // Master must be initialized
            if (!isInitialized) {
                SetError("EnablePDOFiltering: blocked because master not initialized");
                return;
            }
            // Refresh SOEM state and require main context to be OP.
            // Simulation mode publishes the state directly and must not call into SOEM.
            try {
                if (!simulationMode_.load(std::memory_order_relaxed)) {
                    ecx_readstate(&context);
                }
                int currentState = context.slavelist[0].state;
                if ((currentState & EC_STATE_OPERATIONAL) != EC_STATE_OPERATIONAL) {
                    SetError("EnablePDOFiltering: blocked because slaves not in OP state");
                    return;
                }
            } catch (...) {
                SetError("EnablePDOFiltering: exception while checking slave state");
                return;
            }

            // Require at least one configured filter
            bool hasFilters = false;
            EnterCriticalSection(&pdoFilterCS_);
            try {
                hasFilters = !pdoFilters_.empty();
            } catch (...) {
                hasFilters = false;
            }
            LeaveCriticalSection(&pdoFilterCS_);
            if (!hasFilters) {
                SetError("EnablePDOFiltering: blocked because no PDO filters configured");
                return;
            }
        }

        // Apply requested state
        pdoFilteringEnabled_.store(enable, std::memory_order_relaxed);
        if (!enable) {
            // Online OFF: clear stale-input monitoring/latches so the next
            // online session starts fresh.
            EnterCriticalSection(&pdoFilterCS_);
            try {
                for (auto &kv : pdoFilters_) {
                    kv.second.ResetStaleInputMonitoring();
                }
            } catch (...) {
                // best-effort cleanup
            }
            LeaveCriticalSection(&pdoFilterCS_);

            // 清除 MMF 前置濾波 EMA 狀態，確保下次連線時從當前 MMF 值重新初始化，
            // 而非沿用前次連線遺留的 EMA 值。若前次 EMA 值與當前實際位置 / MMF 輸入
            // 差異過大，會導致濾波器在軟啟動期間先朝錯誤方向運動再修正，引發震盪。
            for (int i = 0; i < 6; ++i) {
                mmfPreFilterInit_[i] = false;
                mmfPreFilterEma_[i] = 0.0f;
                mmfPreFilterPrevRaw_[i] = 0.0f;
            }

            // If disabling, also clear soft-start cache conservatively
            if (softStartReadyCache_) {
                for (int i = 0; i < softStartReadyCacheSize_; ++i) softStartReadyCache_[i].store(0, std::memory_order_relaxed);
            }
            // Clear staged outputs for the cyclic motion command PDOs to prevent
            // PDO filter values from overwriting manual inputs after filtering is disabled.
            // If the cyclic path is active, immediately send the cleared IOmap so
            // physical CSV drives receive a final 0x60FF=0 command before the filter stops.
            ClearStagedOutputsForCyclicMotionCommands();
            if (IsRunning()) {
                SendProcessDataOnce();
            }
        }
    }
    // Clear staged outputs for the cyclic motion command PDOs across all slaves
    void ClearStagedOutputsForCyclicMotionCommands();
    bool IsPDOFilteringEnabled() const { return pdoFilteringEnabled_.load(std::memory_order_relaxed); }
    // 設定指定從站的 PDOFilter 違規狀態回調函數
    void SetPDOFilterViolationCallback(int slaveIndex, PDOFilter::ViolationCallback callback);
    // 設定指定從站的 PDOFilter 輸入異常（跳變）回調函數
    void SetPDOFilterInputAnomalyCallback(int slaveIndex, PDOFilter::InputAnomalyCallback callback);
    // 設置緩啟動時間（秒），預設 5.0 秒
    // 觸發緩啟動（應在連線運轉開始、異常恢復等情況下調用）
    void TriggerPDOFilterSoftStart(int slaveIndex);
    // 觸發所有從站的緩啟動
    void TriggerAllPDOFiltersSoftStart();

    // Query whether a specific slave's PDOFilter reports soft-start ready
    bool IsPDOFilterSoftStartReady(int slaveIndex);
    // Query whether all configured PDOFilters report soft-start ready
    bool AreAllPDOFiltersSoftStartReady();
    // Complete soft-start for all PDOFilters (called when master coordinator decides to switch)
    void CompleteAllPDOFiltersSoftStart();
    // Request that master complete soft-start for all PDOFilters from master's context.
    // This sets an atomic request flag that the master's cyclic loop will service.
    void RequestCompleteAllPDOFiltersSoftStart();
    // Hold current position for all PDO filters - makes filters ignore MMF setpoint
    // and track current filtered output position. Use during emergency/smooth stop.
    void HoldAllPDOFiltersPosition();
    // Hold current position for a single PDO filter
    void HoldPDOFilterPosition(int slaveIndex);
    // Release hold-position mode for all PDO filters
    void ReleaseAllPDOFiltersHoldPosition();
    // Get the maximum absolute output velocity across all PDO filters.
    // Call BEFORE HoldAllPDOFiltersPosition() to capture pre-stop speed.
    double GetMaxFilterVelocity();
    // Get the minimum configured max deceleration across all PDO filters.
    double GetMinConfiguredMaxDeceleration();
    // Set/clear powerOnInProgress flag - blocks CyclicTask from writing cyclic motion commands
    void SetPowerOnInProgress(bool inProgress) { powerOnInProgress_.store(inProgress, std::memory_order_release); }
    bool IsPowerOnInProgress() const { return powerOnInProgress_.load(std::memory_order_acquire); }
    // Lock-free cached queries (populated by the master cyclic loop). Use these from UI threads
    // to avoid entering the pdoFilterCS_.
    bool IsPDOFilterSoftStartReady_Cached(int slaveIndex) const;
    bool AreAllPDOFiltersSoftStartReady_Cached() const;
    
    // === 全局違規狀態管理私有方法 ===
    // 檢查是否有任何從站正在違規（需在 pdoFilterCS_ 鎖定內呼叫）
    bool IsAnySlaveViolating() const;
    // 更新全局違規狀態（需在 pdoFilterCS_ 鎖定內呼叫）
    void UpdateGlobalViolationState();
    void ResetDcDriftCompensationState();
    void UpdateDcDriftCompensation(int64_t dctimeNs, uint32_t cycleMicros);
    
private:
    struct SimulationAxisState {
        std::string name;
        uint16_t controlword{0x0006};
        uint16_t lastControlword{0x0006};
        int8_t operationMode{CoreHelpers::kDefaultCyclicOperationMode};
        int8_t modeDisplay{CoreHelpers::kDefaultCyclicOperationMode};
        uint16_t errorCode{0};
        int32_t targetPosition{0};
        int32_t targetVelocity{0};
        int32_t homeOffset{0};
        int8_t homingMethod{35};
        uint32_t softwareLimitMin{0};
        uint32_t softwareLimitMax{0};
        int32_t softwarePositionLimitMin{0};
        int32_t softwarePositionLimitMax{0};
        double actualPositionCounts{0.0};
        double actualVelocityCounts{0.0};
        double positionErrorIntegralCountsSec{0.0};
        double velocityErrorIntegralCounts{0.0};
        double servoAccelerationCounts{0.0};
        double positionVelocityDemandCounts{0.0};
        bool homingActive{false};
        double homingRemainingSec{0.0};
        double homingElapsedSec{0.0};
        double csvSettleStableSec{0.0};
        int32_t lastCsvTargetPosition{0};
        bool haveLastCsvTargetPosition{false};
        std::array<uint8_t, 8> digitalInputs{{0x01, 0, 0, 0, 0, 0, 0, 0}};
    };

    bool InitializeSimulationLocked(const char* ifnameParam);
    bool ConfigureSimulationSlavesLocked();
    bool StartSimulationCyclicOperationLocked();
    void ShutdownSimulationLocked(const std::function<void(const std::string&)>& logCallback);
    bool ReadSimulationSdo(uint16_t slave, uint16_t index, uint8_t subindex, void* data, int& size);
    bool WriteSimulationSdo(uint16_t slave, uint16_t index, uint8_t subindex, const void* data, int size);
    void AdvanceSimulation(double dtSeconds);
    void SimulationCyclicTask();
    uint16_t ComputeSimulationStatusword(const SimulationAxisState& axis) const;

    MMFReader mmf1Reader;
    std::atomic<bool> mmf1ReaderRunning{false};
    // Dedicated MMF receiver thread: non-RT core, polls MMF data independently.
    std::thread mmfReceiverThread_;
    HANDLE mmfReceiverStopEvent_{NULL};
    std::atomic<bool> mmfReceiverRunning_{false};

    // MMF1 input mode: set once at startup, cyclic thread reads without synchronization
    Mmf1InputMode mmf1InputMode_{Mmf1InputMode::ServoAngle};
    Mmf1PoseCoordinateSystem mmf1PoseCoordinateSystem_{Mmf1PoseCoordinateSystem::World};
    // Stewart IK params for Pose6D mode (seqlock for lock-free cyclic reads)
    std::atomic<uint32_t> stewartParamsSeq_{0};
    StewartParamsSnapshot stewartParamsData_;
    // Last valid servo angles from IK (fallback on IK failure); only accessed by cyclic thread
    std::array<float, 6> lastValidServoAngles_{};

    // Forward Kinematics storage: written by PDOProcessingTask, read lock-free by others.
    // Enabled flag is set by AppController after homing completes.
    std::atomic<bool> forwardKinEnabled_{false};
    std::atomic<bool> forwardKinValid_{false};
    WuSOEM::Pose6D forwardKinPose_{};
    long double forwardKinInitialHeight_ = 450.0L;  // set alongside forwardKinPose_

    // === MMF 前置濾波器 (EMA + spike clamp) ===
    // 在 IK 轉換前淨化原始 MMF 輸入，避免雜訊/突波被 IK 非線性放大。
    std::array<float, 6> mmfPreFilterPrevRaw_{};   // 前次原始值（突波偵測用）
    std::array<float, 6> mmfPreFilterEma_{};        // EMA 平滑值
    std::array<bool, 6> mmfPreFilterInit_{};        // 是否已初始化

    // === Pose6D 前置驗證層狀態 ===
    // Pose limits for the real-time validation layer (set by UI/AppController)
    WuSOEM::Pose6DLimits pose6dLimits_{};
    // Atomic flag: set by cyclic thread when pose limit is violated, cleared when OK
    std::atomic<bool> pose6dLimitWarning_{false};
    // Most recent violation message (UTF-16) - swapped atomically by UI thread
    std::wstring pose6dLimitWarningMsg_;
    std::mutex pose6dLimitWarningMsgMutex_;
    // Track whether the previous cycle had a violation (for edge-triggered beep)
    bool pose6dPrevCycleViolated_ = false;

    // === Pose6D 動態行程縮放（Z 觸發）狀態 ===
    std::atomic<bool> pose6dDynamicScaling_{false};
    std::wstring pose6dDynamicScalingMsg_;
    std::mutex pose6dDynamicScalingMsgMutex_;
    bool pose6dPrevDynamicScaling_ = false;

    // Lock-free flag: any PDO filter (slave 1-6) in soft-start.
    // Updated inside pdoFilterCS_ after filter processing; read lock-free
    // by Pose6D block to disable dynamic scaling during soft-start.
    std::atomic<bool> pose6dAnySoftStartActive_{false};

    // PDO濾波器: 每個從站一個濾波器實例
    std::map<int, PDOFilter> pdoFilters_;
    std::atomic<bool> pdoFilteringEnabled_{false};

    // CSV velocity-command state per axis (shared between CyclicTask RT path
    // and PDOProcessingTask fallback; accessed only by one thread at a time
    // depending on lowLatencyMmfPath_ flag).
    struct CsvVelocityCmdState {
        bool initialized{false};
        double smoothedPositionError{0.0};
        double smoothedActualVelocity{0.0};
        double correctionVelocity{0.0};
        double commandVelocity{0.0};
        double integralError{0.0};
        int trackingClampIntentDir{0};
        bool wasSoftStartActive{false};
        int prevClampDir{0};
        void Reset() { *this = CsvVelocityCmdState{}; }
    };
    CsvVelocityCmdState csvVelocityCmdStates_[6]{};

    // Cyclic operation mode: CSP=8 / CSV=9.  Set from INI [Motion] OperationMode
    // before connect; read lock-free by cyclic path and PowerOn sequences.
    std::atomic<int8_t> cyclicOperationMode_{CoreHelpers::kDefaultCyclicOperationMode};
    std::atomic<bool> powerOnInProgress_{false};  // Blocks CyclicTask motion-command writes during PowerOnAllAsync
    std::array<std::atomic<double>, 6> lastPdoFilterInputAngles_{};
    std::array<std::atomic<double>, 6> lastPdoFilterInputVelocities_{};  // deg/s, cyclic-thread computed
    double lastPdoFilterInputPrevAngles_[6] = {};  // previous angle for velocity calc
    LONGLONG lastPdoFilterInputTicks_[6] = {};     // QPC ticks, cyclic-thread only
    CRITICAL_SECTION pdoFilterCS_;
    // Lightweight lock-free cache of per-slave soft-start ready flags updated by master loop.
    // Use a heap-allocated array of atomics to avoid issues with copying/moving std::atomic inside std::vector
    std::unique_ptr<std::atomic<char>[]> softStartReadyCache_;
    int softStartReadyCacheSize_{0};
    // Request flag set by UI to ask master to complete all soft-starts in master's context.
    std::atomic<bool> completeSoftStartRequested_{false};
    
    // === 全局違規狀態管理 ===
    // 任一從軸動態違規（速度/加減速）時強制全部從軸啟用限制；
    // 違規消失後立即解除。
    std::atomic<bool> globalLimitingEnabled_{false};

    // DC（Distributed Clocks）相關：可選的週期漂移補償與狀態
    std::atomic<bool> enableDcDriftComp{false};
    std::atomic<long long> dcToffNs{0}; // 校正偏移（ns），供 cyclic/PDO thread lock-free 共享
    std::atomic<bool> useDcPiController{true};
    double dcPiIntegralNs{0.0};
    double dcPiProportionalGain{0.20};
    double dcPiIntegralGain{0.02};
    // 循環內日誌（可能造成抖動），預設關閉
    bool verboseCyclicLog{true};  // enabled for Win11 diagnostics
    // SYNC0 相位百分比，預設 15%，限制在 10~20% 之間
    int dcSyncPhasePercent{60};  // SYNC0 fires at 60% of cycle (frame arrives ~40-50%)
    // When repeated WKC failures occur, the master's cyclic thread increments this counter.
    // If it reaches the threshold, the master will notify UI/HTTP server to disconnect.
    std::atomic<int> networkFailureCounter{0};

    // MMF 輸出控制（外部可視化）
    MMFWriter mmfWriter_;
    std::atomic<bool> mmfOutputEnabled_{false};
    CRITICAL_SECTION mmfOutputCS_;

    // ========================================================================
    // IK Actual 6-DOF Pose MMF Output (\"BigCat\")
    // ========================================================================
    struct Pose6DMMF {
        // Backward-compatible: pose[6] at offset 0 for existing consumers.
        // [Sway, Surge, Heave, Yaw, Roll, Pitch]
        // Platform-local linear axes; Yaw/Pitch swapped per FlyPT convention.
        double pose[6];
        uint64_t sequence;    // 單調遞增序號，consumer 可偵測更新
        uint32_t valid;       // 1 = forward kinematics 收斂
        uint32_t reserved;    // 對齊保留
        uint64_t timestamp;   // QPC 時間戳，consumer 可量測延遲
    };
    static constexpr size_t POSE_MMF_SIZE = sizeof(Pose6DMMF);

private:
    void WritePoseMMF(const WuSOEM::Pose6D& pose, bool valid, long double initialHeight, uint64_t epochMs);

    HANDLE poseMmfHandle_{NULL};
    std::atomic<Pose6DMMF*> poseMmfView_{nullptr};  // lock-free 讀取（cyclic thread 不阻塞）
    std::atomic<uint64_t> poseMmfSeq_{0};
    std::atomic<bool> poseMmfEnabled_{false};
    CRITICAL_SECTION poseMmfCS_;  // 僅保護 Enable/Disable，Write 不使用

public:
    // PDO in-memory circular log buffer (single-writer: PDOProcessingTask)
        // raw pointer to circular log buffer managed with VirtualAlloc
        char* pdoLogBufferBase_{nullptr};
        size_t pdoLogCapacity_{100 * 1024 * 1024}; // 100 MB
    size_t pdoLogWritePos_{0}; // next write position (mod capacity)
    std::atomic<bool> pdoRecordingEnabled_{false};
    std::atomic<bool> pdoLogWrapped_{false};
    std::atomic<bool> pdoWriterActive_{false};
    // mutex used only when flushing the buffer to file (stop/flush path)
    std::mutex pdoLogMutex_;
    std::atomic<bool> pdoPreTouching_{false};
    // Threshold for automatic network-failure-triggered disconnect
    int networkFailureThreshold{5};
    // Flag: if true, automatically issue disconnect command on network failure
    std::atomic<bool> autoDisconnectOnNetworkFailure{true};

    // Persistent global error flag for PDO filter errors
    std::atomic<bool> persistentGlobalError_{false};
    // Diagnostics: enable recording of PDO 0x607A values (logical target position)
    std::atomic<bool> pdo607aRecording_{false};
    std::atomic<bool> simulationMode_{false};
    CRITICAL_SECTION simulationCS_;
    std::vector<SimulationAxisState> simulationAxes_;
};

#endif // ETHERCAT_MASTER_H
