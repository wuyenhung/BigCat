#include "EtherCATMaster.h"
#include "core/AppConfig.h"
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <eh.h>
#include <iomanip>
#include <bitset>
#include <chrono>
#include <cmath>
#include <ctime>
#include <intrin.h>
#include <mmsystem.h>
#include <soem/ec_print.h>
#include <fstream>
#include "SafeMem.h"
#include "StewartIK.h"

namespace {

const wchar_t* GetMmf1InputMappingName(Mmf1InputMode mode)
{
    return (mode == Mmf1InputMode::Pose6D)
        ? L"OUTPUT_MMF_6DOF"
        : L"OUTPUT_MMF_DEG";
}

void ReleasePdoLogBuffer(char*& buffer)
{
    if (buffer) {
        VirtualFree(buffer, 0, MEM_RELEASE);
        buffer = nullptr;
    }
}

} // namespace

bool EtherCATMaster::StartMMF1In()
{
    // Prevent repeated starts
    if (mmf1ReaderRunning.load()) return true;
    // Select the MMF input mapping name from the configured MMF1 input mode.
    // An INI override ([MMF] InputName) takes precedence; otherwise the
    // mode-dependent hardcoded name is used (default behavior).
    const std::wstring mappingName = AppConfig::ReadMmf1InputName(GetMmf1InputMappingName(mmf1InputMode_));
    bool ok = mmf1Reader.Start(mappingName, 0);
    if (ok) {
        mmf1ReaderRunning.store(true);
        // Start the dedicated receiver thread on a non-RT (Worker) core.
        // This thread polls MMF independently of the PDO cycle so MMF arrival
        // jitter never disturbs the EtherCAT control loop.
        StartMMFReceiver();
    }
    return ok;
}


void EtherCATMaster::StopMMF1In()
{
    if (!mmf1ReaderRunning.load()) return;
    // Stop the receiver thread before the reader itself
    StopMMFReceiver();
    mmf1Reader.Stop();
    mmf1ReaderRunning.store(false);
}


// ── MMF Receiver Thread (non-RT Worker Core) ──
// Periodically calls mmf1Reader.PollOnce() so the RT cyclic thread only
// needs to read the latest seqlock-protected snapshot via GetLatest().
// This eliminates non-deterministic MMF I/O latency from the PDO cycle.

DWORD WINAPI EtherCATMaster::MMFReceiverThreadFunc(LPVOID param)
{
    EtherCATMaster* master = static_cast<EtherCATMaster*>(param);
    master->MMFReceiverTask();
    return 0;
}


void EtherCATMaster::MMFReceiverTask()
{
    // Pin this thread AWAY from the RT core — it runs on the Worker core.
    CoreHelpers::CpuIsolation::ExcludeCurrentThreadFromRtCores();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    constexpr DWORD kPollIntervalMs = 1; // 1 ms poll rate, faster than PDO cycle

    while (WaitForSingleObject(mmfReceiverStopEvent_, kPollIntervalMs) == WAIT_TIMEOUT) {
        if (mmf1ReaderRunning.load(std::memory_order_relaxed)) {
            (void)mmf1Reader.PollOnce();
        }
    }
}


bool EtherCATMaster::StartMMFReceiver()
{
    if (mmfReceiverRunning_.load(std::memory_order_relaxed)) return true;

    // In simulation mode the PDO cycle itself calls PollOnce() inline —
    // an async receiver thread would race with the deterministic
    // simulated time steps and destabilise simulation-based tests.
    if (simulationMode_.load(std::memory_order_relaxed)) return true;

    mmfReceiverStopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!mmfReceiverStopEvent_) {
        SetError("StartMMFReceiver: CreateEvent failed");
        return false;
    }

    mmfReceiverThread_ = std::thread(MMFReceiverThreadFunc, this);
    mmfReceiverRunning_.store(true, std::memory_order_relaxed);
    return true;
}


void EtherCATMaster::StopMMFReceiver()
{
    if (!mmfReceiverRunning_.load(std::memory_order_relaxed)) return;

    if (mmfReceiverStopEvent_) {
        SetEvent(mmfReceiverStopEvent_);
    }
    if (mmfReceiverThread_.joinable()) {
        mmfReceiverThread_.join();
    }
    if (mmfReceiverStopEvent_) {
        CloseHandle(mmfReceiverStopEvent_);
        mmfReceiverStopEvent_ = nullptr;
    }
    mmfReceiverRunning_.store(false, std::memory_order_relaxed);
}

// Enable writing PDO data to a named Memory-Mapped File for external visualization

bool EtherCATMaster::EnableMMFOutput(const std::wstring& mappingName)
{
    EnterCriticalSection(&mmfOutputCS_);
    // Re-entry guard: if already enabled, skip to avoid double Create() which
    // would race with the PDO cycle's WriteSlave/UpdateHeader on the same view.
    if (mmfOutputEnabled_.load(std::memory_order_relaxed)) {
        LeaveCriticalSection(&mmfOutputCS_);
        SetError("MMF output already enabled");
        return true; // already enabled is not a failure for the caller
    }
    bool ok = mmfWriter_.Create(mappingName);
    if (ok) {
        mmfOutputEnabled_.store(true);
        SetError("MMF output enabled");
    } else {
        mmfOutputEnabled_.store(false);
        SetError("EnableMMFOutput: failed to create mapping");
    }
    LeaveCriticalSection(&mmfOutputCS_);
    return ok;
}


void EtherCATMaster::DisableMMFOutput()
{
    EnterCriticalSection(&mmfOutputCS_);
    if (!mmfOutputEnabled_.load(std::memory_order_relaxed)) {
        // Already disabled; ensure writer is closed anyway (defense in depth)
        mmfWriter_.Close();
        LeaveCriticalSection(&mmfOutputCS_);
        return;
    }
    mmfWriter_.Close();
    mmfOutputEnabled_.store(false);
    LeaveCriticalSection(&mmfOutputCS_);
}

// WritePDOLogEntry: format and append one CSV line to the circular log buffer
// Format: time,epoch_ms,uptime_ms,slave,MMF_IN,controlWord(hex),statusWord(hex),targetPos,targetVel,actualPos,actualVel,pdoFilterOutput,pdoFilterVel,pdoFilterAcc,filterTarget,softStartActive,filterTrackingError\n
// time is human-readable local time: YYYY-MM-DD HH:MM:SS.mmm
// Position and velocity are in mechanical units (mm, mm/s)
// Called from PDOProcessingTask (single writer), mutex protects buffer writes

void EtherCATMaster::WritePDOLogEntry(int slaveIndex, uint64_t epochMs, uint64_t uptimeMs,
                                       float mmfIn,
                                       uint16_t controlWord, uint16_t statusWord,
                                       double targetPos, double targetVel,
                                       double actualPos, double actualVel,
                                       double pdoFilterOutput, double pdoFilterVel, double pdoFilterAcc,
                                       double filterTarget, int softStartActive, double filterTrackingError)
{
    if (!pdoLogBufferBase_ || !pdoRecordingEnabled_.load(std::memory_order_relaxed)) return;
    
    // Convert epoch ms to human-readable local time
    time_t sec = static_cast<time_t>(epochMs / 1000);
    int ms = static_cast<int>(epochMs % 1000);
    struct tm tmBuf;
    localtime_s(&tmBuf, &sec);
    char timeStr[24];
    int timeLen = snprintf(timeStr, sizeof(timeStr), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                           tmBuf.tm_year + 1900, tmBuf.tm_mon + 1, tmBuf.tm_mday,
                           tmBuf.tm_hour, tmBuf.tm_min, tmBuf.tm_sec, ms);
    
    // Format CSV line with mechanical units (6 decimal places for precision)
    char line[384];
    int len = snprintf(line, sizeof(line), "%.*s,%llu,%llu,%d,%.6f,0x%04X,0x%04X,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%d,%.6f\n",
                       timeLen, timeStr,
                       (unsigned long long)epochMs, (unsigned long long)uptimeMs,
                       slaveIndex, mmfIn,
                       (unsigned)controlWord, (unsigned)statusWord,
                       targetPos, targetVel, actualPos, actualVel,
                       pdoFilterOutput, pdoFilterVel, pdoFilterAcc,
                       filterTarget, softStartActive, filterTrackingError);
                       
    
    if (len <= 0 || len >= (int)sizeof(line)) return; // format error or truncation
    
    // Write to circular buffer with mutex protection
    std::lock_guard<std::mutex> lk(pdoLogMutex_);
    // Re-check after acquiring mutex: StopPDORecordingAndFlushToFile may have
    // detached the buffer between the fast-path check above and this lock.
    if (!pdoLogBufferBase_ || !pdoRecordingEnabled_.load(std::memory_order_relaxed)) return;
    size_t remaining = pdoLogCapacity_ - pdoLogWritePos_;
    if ((size_t)len <= remaining) {
        // Fits without wrapping
        memcpy(pdoLogBufferBase_ + pdoLogWritePos_, line, len);
        pdoLogWritePos_ += len;
    } else {
        // Wrap around: write partial to end, rest to beginning
        if (remaining > 0) {
            memcpy(pdoLogBufferBase_ + pdoLogWritePos_, line, remaining);
        }
        size_t leftover = len - remaining;
        if (leftover > 0 && leftover < pdoLogCapacity_) {
            memcpy(pdoLogBufferBase_, line + remaining, leftover);
            pdoLogWritePos_ = leftover;
            pdoLogWrapped_.store(true, std::memory_order_relaxed);
        } else {
            pdoLogWritePos_ = 0; // buffer full or error
        }
    }
}


void EtherCATMaster::StartPDORecording()
{
    std::lock_guard<std::mutex> lk(pdoLogMutex_);
    if (pdoRecordingEnabled_.load()) return;
    if (!pdoLogBufferBase_) {
        pdoLogBufferBase_ = static_cast<char*>(VirtualAlloc(nullptr, pdoLogCapacity_, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!pdoLogBufferBase_) {
            SetError("StartPDORecording: Failed to allocate recording buffer");
            return;
        }
        memset(pdoLogBufferBase_, 0, pdoLogCapacity_);
    }
    pdoLogWritePos_ = 0;
    pdoLogWrapped_ = false;
    pdoRecordingEnabled_.store(true);
    SetError("PDO recording started");
}


void EtherCATMaster::StopPDORecordingAndFlushToFile(const std::wstring& filePath)
{
    // Disable recording first
    pdoRecordingEnabled_.store(false);

    char* buf = nullptr;
    size_t pos = 0;
    bool wrapped = false;
    size_t cap = 0;

    {
        std::lock_guard<std::mutex> lk(pdoLogMutex_);
        if (pdoWriterActive_.load()) {
            SetError("StopPDORecordingAndFlushToFile: writer already active");
            return;
        }

        if (!pdoLogBufferBase_ || pdoLogWritePos_ == 0) {
            ReleasePdoLogBuffer(pdoLogBufferBase_);
            pdoLogWritePos_ = 0;
            pdoLogWrapped_.store(false, std::memory_order_relaxed);
            SetError("StopPDORecordingAndFlushToFile: no recorded data; buffer released");
            return;
        }

        // Detach the current recording buffer from the master so a later
        // restart cannot overwrite the data that the background flush thread owns.
        buf = pdoLogBufferBase_;
        pos = pdoLogWritePos_;
        wrapped = pdoLogWrapped_.load(std::memory_order_relaxed);
        cap = pdoLogCapacity_;
        pdoLogBufferBase_ = nullptr;
        pdoLogWritePos_ = 0;
        pdoLogWrapped_.store(false, std::memory_order_relaxed);
        pdoWriterActive_.store(true);
    }

    std::thread writer([this, filePath, buf, pos, wrapped, cap]() {
        struct WriterGuard {
            std::atomic<bool>* active;
            char* buffer;

            ~WriterGuard()
            {
                if (buffer) {
                    VirtualFree(buffer, 0, MEM_RELEASE);
                }
                if (active) {
                    active->store(false);
                }
            }
        } writerGuard{&pdoWriterActive_, buf};

        std::ofstream ofs;
        // Convert wide path to narrow (UTF-8) using simple wstring conversion
        // Note: windows ofstream accepts wchar_t path in recent VS, but to be portable, use wofstream
        std::wofstream wofs(filePath.c_str());
        if (!wofs) {
            // fallback to narrow path write
            std::ofstream ofs2;
            std::string path;
            try {
                path.assign(filePath.begin(), filePath.end());
                ofs2.open(path, std::ios::binary);
                if (!ofs2) {
                    SetError("StopPDORecordingAndFlushToFile: Failed to open file for write");
                    return;
                }
                if (wrapped) {
                    // write from pos .. cap-1 then 0..pos-1
                    ofs2.write(writerGuard.buffer + pos, static_cast<std::streamsize>(cap - pos));
                    ofs2.write(writerGuard.buffer, static_cast<std::streamsize>(pos));
                } else {
                    ofs2.write(writerGuard.buffer, static_cast<std::streamsize>(pos));
                }
                ofs2.close();
            } catch (...) {
                SetError("StopPDORecordingAndFlushToFile: exception during file write");
                return;
            }
        } else {
            // If wofstream succeeded, write binary using narrow conversion
            try {
                wofs.close();
                std::ofstream ofs3;
                std::string path(filePath.begin(), filePath.end());
                ofs3.open(path, std::ios::binary);
                if (!ofs3) {
                    SetError("StopPDORecordingAndFlushToFile: Failed to open file for write");
                    return;
                }
                if (wrapped) {
                    ofs3.write(writerGuard.buffer + pos, static_cast<std::streamsize>(cap - pos));
                    ofs3.write(writerGuard.buffer, static_cast<std::streamsize>(pos));
                } else {
                    ofs3.write(writerGuard.buffer, static_cast<std::streamsize>(pos));
                }
                ofs3.close();
            } catch (...) {
                SetError("StopPDORecordingAndFlushToFile: exception during file write");
                return;
            }
        }

        SetError("StopPDORecordingAndFlushToFile: flush completed; buffer released");
    });
    writer.detach();
}


// ========================================================================
// Stewart IK Parameters: Seqlock writer / reader
// ========================================================================

void EtherCATMaster::SetStewartParams(const WuSOEM::StewartParams& p) {
    // Writer side: increment seq to odd (writing), copy data, increment to even (done)
    uint32_t seq = stewartParamsSeq_.load(std::memory_order_relaxed);
    stewartParamsSeq_.store(seq + 1, std::memory_order_release); // odd = writing
    stewartParamsData_.params = p;
    stewartParamsData_.initialHeight =
        WuSOEM::StewartIK::CalculateInitialHeight(p, p.servoInitialAngle);
    stewartParamsSeq_.store(seq + 2, std::memory_order_release); // even = done

    // Initialize lastValidServoAngles to home angle
    float homeAngle = static_cast<float>(p.servoInitialAngle);
    for (auto& v : lastValidServoAngles_) v = homeAngle;
}

EtherCATMaster::StewartParamsSnapshot EtherCATMaster::ReadStewartParamsSnapshot() const {
    // Reader side: spin until we get a consistent read (seq unchanged and even)
    StewartParamsSnapshot snap;
    for (int attempt = 0; attempt < 64; ++attempt) {
        uint32_t seq1 = stewartParamsSeq_.load(std::memory_order_acquire);
        if (seq1 & 1) { _mm_pause(); continue; } // writer in progress
        snap = stewartParamsData_;
        uint32_t seq2 = stewartParamsSeq_.load(std::memory_order_acquire);
        if (seq1 == seq2) return snap;
        _mm_pause();
    }
    // Fallback: return whatever we have (extremely unlikely to reach here)
    return snap;
}

// ========================================================================
// Pose6D 前置驗證層 API
// ========================================================================

void EtherCATMaster::SetPose6DLimits(const WuSOEM::Pose6DLimits& limits) {
    pose6dLimits_ = limits;
}

WuSOEM::Pose6DLimits EtherCATMaster::GetPose6DLimits() const {
    return pose6dLimits_;
}

std::wstring EtherCATMaster::ConsumePose6DLimitWarningMessage() {
    std::lock_guard<std::mutex> lk(pose6dLimitWarningMsgMutex_);
    std::wstring msg;
    std::swap(msg, pose6dLimitWarningMsg_);
    return msg;
}

std::wstring EtherCATMaster::ConsumePose6DDynamicScalingMessage() {
    std::lock_guard<std::mutex> lk(pose6dDynamicScalingMsgMutex_);
    std::wstring msg;
    std::swap(msg, pose6dDynamicScalingMsg_);
    return msg;
}

// ========================================================================
// IK 實際 6-DOF 姿態 MMF 輸出 ("BigCat")
// ========================================================================

bool EtherCATMaster::EnablePoseMMFOutput(const std::wstring& mappingName)
{
    EnterCriticalSection(&poseMmfCS_);
    if (poseMmfEnabled_.load(std::memory_order_relaxed)) {
        LeaveCriticalSection(&poseMmfCS_);
        SetError("Pose MMF output already enabled");
        return true;
    }

    // Clean up any stale handles (old view already nullptr after Disable)
    Pose6DMMF* oldView = poseMmfView_.exchange(nullptr, std::memory_order_acq_rel);
    if (oldView) {
        UnmapViewOfFile(oldView);
    }
    if (poseMmfHandle_) {
        CloseHandle(poseMmfHandle_);
        poseMmfHandle_ = NULL;
    }

    poseMmfHandle_ = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        NULL,
        PAGE_READWRITE,
        0,
        static_cast<DWORD>(POSE_MMF_SIZE),
        mappingName.c_str());

    if (!poseMmfHandle_) {
        LeaveCriticalSection(&poseMmfCS_);
        SetError("EnablePoseMMFOutput: CreateFileMappingW failed");
        return false;
    }

    Pose6DMMF* newView = static_cast<Pose6DMMF*>(MapViewOfFile(
        poseMmfHandle_,
        FILE_MAP_ALL_ACCESS,
        0, 0,
        POSE_MMF_SIZE));

    if (!newView) {
        CloseHandle(poseMmfHandle_);
        poseMmfHandle_ = NULL;
        LeaveCriticalSection(&poseMmfCS_);
        SetError("EnablePoseMMFOutput: MapViewOfFile failed");
        return false;
    }

    std::memset(newView, 0, POSE_MMF_SIZE);
    poseMmfSeq_.store(0, std::memory_order_relaxed);

    // Publish view before enabled flag so cyclic thread sees valid pointer
    poseMmfView_.store(newView, std::memory_order_release);
    poseMmfEnabled_.store(true, std::memory_order_release);
    LeaveCriticalSection(&poseMmfCS_);
    SetError("Pose MMF output enabled (BigCat)");
    return true;
}

void EtherCATMaster::DisablePoseMMFOutput()
{
    EnterCriticalSection(&poseMmfCS_);
    // Disable flag first so cyclic thread stops writing before we unmap
    poseMmfEnabled_.store(false, std::memory_order_release);

    Pose6DMMF* oldView = poseMmfView_.exchange(nullptr, std::memory_order_acq_rel);
    if (oldView) {
        UnmapViewOfFile(oldView);
    }
    if (poseMmfHandle_) {
        CloseHandle(poseMmfHandle_);
        poseMmfHandle_ = NULL;
    }
    LeaveCriticalSection(&poseMmfCS_);
}

void EtherCATMaster::WritePoseMMF(const WuSOEM::Pose6D& pose, bool valid, long double initialHeight, uint64_t epochMs)
{
    // Lock-free read: acquire pairs with release in Enable, ensuring
    // we see a fully-initialized view or nullptr.
    if (!poseMmfEnabled_.load(std::memory_order_acquire)) return;

    Pose6DMMF* view = poseMmfView_.load(std::memory_order_acquire);
    if (!view) return;

    const uint64_t seq = poseMmfSeq_.fetch_add(1, std::memory_order_relaxed) + 1;

    LARGE_INTEGER qpc;
    const uint64_t ts = QueryPerformanceCounter(&qpc) ? static_cast<uint64_t>(qpc.QuadPart) : 0;

    // Convert world pose to platform coordinate system (outside any lock)
    const std::array<long double, 3> worldDelta{{
        pose.x,
        pose.y,
        pose.z - initialHeight,
    }};
    const auto platformDelta = WuSOEM::StewartIK::TransformWorldToPlatformDelta(worldDelta, pose);

    // FlyPT order: [Sway, Surge, Heave, Yaw, Roll, Pitch]
    // Write pose first, then metadata — consumer can check sequence change
    // to know pose[6] is consistent.
    view->pose[0] = static_cast<double>(platformDelta[0]);   // Sway (platform)
    view->pose[1] = static_cast<double>(platformDelta[1]);   // Surge (platform)
    view->pose[2] = static_cast<double>(platformDelta[2]);   // Heave (platform)
    view->pose[3] = static_cast<double>(pose.yaw);            // FYaw (索引對調)
    view->pose[4] = static_cast<double>(pose.pitch);          // Roll (左右傾斜)
    view->pose[5] = static_cast<double>(pose.roll);           // Pitch (前後傾斜, 索引對調)

    // Write metadata last as "commit" signal
    view->valid    = valid ? 1 : 0;
    view->timestamp = ts;
    // Sequence written last so consumer can use it as a tear-detection fence
    view->sequence = seq;

    (void)epochMs; // reserved for future use (e.g. wall-clock timestamp)
}
