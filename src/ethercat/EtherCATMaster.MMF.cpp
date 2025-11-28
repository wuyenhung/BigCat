#include "EtherCATMaster.h"
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <eh.h>
#include <iomanip>
#include <bitset>
#include <chrono>
#include <cmath>
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
    const wchar_t* mappingName = GetMmf1InputMappingName(mmf1InputMode_);
    bool ok = mmf1Reader.Start(std::wstring(mappingName), 0);
    if (ok) {
        mmf1ReaderRunning.store(true);
    }
    return ok;
}


void EtherCATMaster::StopMMF1In()
{
    if (!mmf1ReaderRunning.load()) return;
    mmf1Reader.Stop();
    mmf1ReaderRunning.store(false);
}

// Enable writing PDO data to a named Memory-Mapped File for external visualization

bool EtherCATMaster::EnableMMFOutput(const std::wstring& mappingName)
{
    EnterCriticalSection(&mmfOutputCS_);
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
    mmfWriter_.Close();
    mmfOutputEnabled_.store(false);
    LeaveCriticalSection(&mmfOutputCS_);
}

// WritePDOLogEntry: format and append one CSV line to the circular log buffer
// Format: epoch_ms,uptime_ms,slave,MMF_IN,controlWord,statusWord,targetPos,targetVel,actualPos,actualVel,pdoFilterOutput,pdoFilterVel,pdoFilterAcc\n
// Position and velocity are in mechanical units (mm, mm/s)
// Called from PDOProcessingTask (single writer), mutex protects buffer writes

void EtherCATMaster::WritePDOLogEntry(int slaveIndex, uint64_t epochMs, uint64_t uptimeMs,
                                       float mmfIn,
                                       uint16_t controlWord, uint16_t statusWord,
                                       double targetPos, double targetVel,
                                       double actualPos, double actualVel,
                                       double pdoFilterOutput, double pdoFilterVel, double pdoFilterAcc)
{
    if (!pdoLogBufferBase_ || !pdoRecordingEnabled_.load(std::memory_order_relaxed)) return;
    
    // Format CSV line with mechanical units (6 decimal places for precision)
    char line[256];
    int len = snprintf(line, sizeof(line), "%llu,%llu,%d,%.6f,%u,%u,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n",
                       (unsigned long long)epochMs, (unsigned long long)uptimeMs,
                       slaveIndex, mmfIn,
                       (unsigned)controlWord, (unsigned)statusWord,
                       targetPos, targetVel, actualPos, actualVel, pdoFilterOutput, pdoFilterVel, pdoFilterAcc);
                       
    
    if (len <= 0 || len >= (int)sizeof(line)) return; // format error or truncation
    
    // Write to circular buffer with mutex protection
    std::lock_guard<std::mutex> lk(pdoLogMutex_);
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
