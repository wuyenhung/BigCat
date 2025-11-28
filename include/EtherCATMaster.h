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
    bool HasActiveSlaveThreads() const;
    bool HasGlobalError() const { return persistentGlobalError_.load(); }
    // Enable/disable recording of PDO 0x607A (logical target position) for diagnostics
    void EnablePDO607ARecording(bool enable) { pdo607aRecording_.store(enable, std::memory_order_relaxed); }
    void SetGlobalError() { persistentGlobalError_.store(true, std::memory_order_relaxed); }
    void ClearGlobalError() { persistentGlobalError_.store(false, std::memory_order_relaxed); }
    std::vector<SlaveInfo> GetSlaveInfo();
    std::string GetLastError() const { return lastError; }
    
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
    // 遊戲共存模式 (Game Coexistence Mode)
    // ========================================================================
    // 當與 DCS、IL-2、MSFS 等飛行模擬遊戲同時運行時，設為 true。
    // 此模式會：
    // 1. 使用 HIGH_PRIORITY_CLASS 而非 REALTIME_PRIORITY_CLASS（避免遊戲卡頓）
    // 2. 保持 TIME_CRITICAL 執行緒優先級（維持 EtherCAT 即時性）
    // 預設為 true（推薦用於模擬器場景）
    void SetGameCoexistenceMode(bool enable) { gameCoexistenceMode_ = enable; }
    bool IsGameCoexistenceMode() const { return gameCoexistenceMode_; }
    
    // 診斷
    uint32_t GetCycleTime() const { return desiredCycleTime; }  // 返回期望的週期時間
    uint32_t GetActualCycleTime() const { return cycleTime; }   // 返回實際測量的週期時間
    uint32_t GetWorkingCounter() const { return (workingCounter < 0) ? 0u : (uint32_t)workingCounter; }
    bool IsPDO607ARecordingEnabled() const { return pdo607aRecording_.load(std::memory_order_relaxed); }
    uint32_t GetCommsTime() const { return commsTime_.load(std::memory_order_relaxed); }        // 返回通訊工作時間 (send/receive only)
    uint64_t GetPDOSentCount() const { return pdosSentCount.load(std::memory_order_relaxed); }
    // Max observed comms time (updated from cyclic thread in a lock-free manner)
    uint32_t GetMaxObservedCommsTime() const { return maxObservedCommsTime.load(std::memory_order_relaxed); }
    void ResetMaxObservedCommsTime() { maxObservedCommsTime.store(0, std::memory_order_relaxed); }
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
    // Dump ESI / EEPROM for a specific slave. Returns dump as UTF-8 string.
    bool DumpESI(uint16_t slave, std::string &outDump);
    // Try runtime initialization for a slave (SDO/runtime steps) to reach OP without writing EEPROM.
    // Returns true if slave reached OP.
    bool ForceSlaveOperational(uint16_t slave, std::string &outLog);
    // Configure slave runtime PDO but don't transition to OP (stays in SAFE_OP)
    bool ConfigureSlaveRuntime(uint16_t slave, std::string &outLog);
    // Safely transition all slaves from OP to INIT before disconnecting
    void StopAllSlaves();
    // Send a single processdata roundtrip (SOEM standard practice before requesting OP)
    void SendProcessDataOnce();

    // SDO reader: background reader that polls a configured SDO and reports changes.
    // StartSdoReader: spawns a background thread to periodically read SDO 0x60FD from slave 1
    //   periodMs: polling period in milliseconds (>=1)
    //   logCb: optional callback to receive human-readable change/diagnostic messages
    bool StartSdoReader(unsigned int periodMs, const std::function<void(const std::string&)> &logCb);
    // Stop the background SDO reader and join thread
    void StopSdoReader();
    bool IsSdoReaderRunning() const { return sdoReaderRunning.load(); }
    double GetSdoReaderLastObservedCycleMs() const { return sdoLastObservedCycleMs.load(); }
    // Return a copy of the last-read SDO 0x60FD value (bytes). Thread-safe.
    // Returns true if a value is available (non-empty), false otherwise.
    bool GetLastSdo60FD(std::vector<uint8_t>& out);
    // Return the low byte of the last-read SDO 0x60FD value without copying the full buffer.
    bool GetLastSdo60FDLowByte(uint8_t& outLowByte);
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
    std::array<double, 6> GetPDOFilterInputAngles() const {
        std::array<double, 6> values{};
        for (size_t i = 0; i < values.size(); ++i) {
            values[i] = lastPdoFilterInputAngles_[i].load(std::memory_order_relaxed);
        }
        return values;
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
                        double pdoFilterOutput, double pdoFilterVel, double pdoFilterAcc);
    
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
    
    // 遊戲共存模式：與模擬器遊戲同時運行時，避免使用 REALTIME_PRIORITY_CLASS
    // 預設停用（專用機模式），使用 REALTIME_PRIORITY_CLASS 獲得最佳即時性
    bool gameCoexistenceMode_{false};
    
    HANDLE cyclicThread;  // Legacy global cyclic thread (deprecated)
    HANDLE stopEvent;     // Legacy global stop event (deprecated)
    
    // Per-slave cyclic threads (new architecture)
    std::vector<SlaveThreadData> slaveThreads;
    
    uint32_t cycleTime;      // 微秒
    uint32_t desiredCycleTime; // 期望的週期時間（微秒）
    // internal working counter returned by SOEM ecx_receive_processdata
    // keep signed so SOEM's -1 can be handled; public getter converts negative to 0
    int32_t workingCounter;
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
    std::function<void()> cyclicCallback;
    
    // IO 映射緩衝區
    uint8_t IOmap[4096];
    
    // 執行緒同步
    CRITICAL_SECTION cs;
    // SDO mailbox serialization - prevents concurrent SDO operations from different threads
    CRITICAL_SECTION sdoCS;
    std::atomic<bool> pdoExchangeActive_{false};
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
    
    // PDO 即時處理執行緒 (核心執行緒)
    HANDLE pdoProcessingThread;
    HANDLE pdoStopEvent;
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

    // SDO reader background thread and synchronization
    std::thread sdoReaderThread;
    std::atomic<bool> sdoReaderRunning{false};
    std::atomic<bool> sdoReaderStop{false};
    std::mutex sdoReaderMutex;
    std::condition_variable sdoReaderCv;
    std::atomic<unsigned int> sdoReaderPeriodMs{10};
    std::atomic<bool> sdoReaderEnabled{true};
    std::atomic<bool> sdoReaderBusy{false};
    std::vector<uint8_t> sdoLastVal; // last observed SDO value
    std::atomic<double> sdoLastObservedCycleMs{0.0};
    std::function<void(const std::string&)> sdoLogCb;
    // Last wall-clock ms when SDO reader performed an actual read
    std::atomic<uint64_t> sdoReaderLastRunMs{0};

public:
    // Perform one tick for the SDO reader; intended to be called periodically
    // by an external thread (e.g. a consolidated error-check thread). This
    // will perform an SDO read only when the configured period elapsed.
    void SdoReaderTick();

    // MMF1_IN reader: provide an API so UI can ask EtherCATMaster to start/stop the
    // background MMF reader. This centralizes lifetime and avoids duplicating
    // MMFReader between UI and master logic.
public:
    // Start/Stop MMF1_IN reader. Returns true when started successfully.
    bool StartMMF1In();
    void StopMMF1In();
    bool IsMMF1InRunning() const { return mmf1ReaderRunning.load(); }
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
    bool IsGlobalViolationActive() const { return globalViolationActive_.load(std::memory_order_relaxed); }
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
    // Release hold-position mode for all PDO filters
    void ReleaseAllPDOFiltersHoldPosition();
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
        std::array<uint8_t, 8> digitalInputs{{0x20, 0, 0, 0, 0, 0, 0, 0}};
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

    // MMF1 input mode: set once at startup, cyclic thread reads without synchronization
    Mmf1InputMode mmf1InputMode_{Mmf1InputMode::ServoAngle};
    Mmf1PoseCoordinateSystem mmf1PoseCoordinateSystem_{Mmf1PoseCoordinateSystem::World};
    // Stewart IK params for Pose6D mode (seqlock for lock-free cyclic reads)
    std::atomic<uint32_t> stewartParamsSeq_{0};
    StewartParamsSnapshot stewartParamsData_;
    // Last valid servo angles from IK (fallback on IK failure); only accessed by cyclic thread
    std::array<float, 6> lastValidServoAngles_{};

    // PDO濾波器: 每個從站一個濾波器實例
    std::map<int, PDOFilter> pdoFilters_;
    std::atomic<bool> pdoFilteringEnabled_{false};
    std::atomic<bool> powerOnInProgress_{false};  // Blocks CyclicTask motion-command writes during PowerOnAllAsync
    std::array<std::atomic<double>, 6> lastPdoFilterInputAngles_{};
    CRITICAL_SECTION pdoFilterCS_;
    // Lightweight lock-free cache of per-slave soft-start ready flags updated by master loop.
    // Use a heap-allocated array of atomics to avoid issues with copying/moving std::atomic inside std::vector
    std::unique_ptr<std::atomic<char>[]> softStartReadyCache_;
    int softStartReadyCacheSize_{0};
    // Request flag set by UI to ask master to complete all soft-starts in master's context.
    std::atomic<bool> completeSoftStartRequested_{false};
    
    // === 全局違規狀態管理 (Global Violation State) ===
    // 任一從軸違規時，全部都當作違規；只有當全部沒有違規且持續 RECOVERY_DELAY_SEC 後才恢復
    std::atomic<bool> globalViolationActive_{false};      // 全局違規狀態
    std::atomic<bool> globalLimitingEnabled_{false};      // 全局限制已啟用
    std::chrono::steady_clock::time_point globalNoViolationStartTime_;  // 全局無違規開始時間
    bool globalNoViolationTimerActive_{false};            // 無違規計時器是否啟動

    // DC（Distributed Clocks）相關：可選的週期漂移補償與狀態
    std::atomic<bool> enableDcDriftComp{false};
    std::atomic<long long> dcToffNs{0}; // 校正偏移（ns），供 cyclic/PDO thread lock-free 共享
    std::atomic<bool> useDcPiController{true};
    double dcPiIntegralNs{0.0};
    double dcPiProportionalGain{0.20};
    double dcPiIntegralGain{0.02};
    // 循環內日誌（可能造成抖動），預設關閉
    bool verboseCyclicLog{false};
    // SYNC0 相位百分比，預設 15%，限制在 10~20% 之間
    int dcSyncPhasePercent{15};
    // When repeated WKC failures occur, the master's cyclic thread increments this counter.
    // If it reaches the threshold, the master will notify UI/HTTP server to disconnect.
    std::atomic<int> networkFailureCounter{0};

    // MMF 輸出控制（外部可視化）
    MMFWriter mmfWriter_;
    std::atomic<bool> mmfOutputEnabled_{false};
    CRITICAL_SECTION mmfOutputCS_;
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
