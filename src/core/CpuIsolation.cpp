#include "../../include/core/CpuIsolation.h"
#include <windows.h>
#include <tlhelp32.h>
#include <iostream>
#include <algorithm>
#include <vector>
#include <string>

namespace CoreHelpers {

    // Using fully qualified name for static members inside implementation
    std::atomic<bool> CpuIsolation::monitorRunning(false);
    std::thread CpuIsolation::monitorThread;

    const std::vector<std::wstring> CpuIsolation::targetProcessNames = {
        L"DCS.exe",
        L"vrserver.exe",
        L"vrmonitor.exe",
        L"OVRServer_x64.exe",
        L"SteamVR.exe",
        L"VirtualDesktop.Streamer.exe"
    };

    void CpuIsolation::StartIsolationMonitor() {
        if (monitorRunning.load()) {
            return; 
        }
        monitorRunning.store(true);
        monitorThread = std::thread(MonitorThreadLoop);
    }

    void CpuIsolation::StopIsolationMonitor() {
        if (monitorRunning.load()) {
            monitorRunning.store(false);
            if (monitorThread.joinable()) {
                monitorThread.join();
            }
        }
    }

    void CpuIsolation::MonitorThreadLoop() {
        // Assume Core 4 (mask 0x10) is the target realtime core
        const int TARGET_ETHERCAT_CORE = 4;
        const DWORD_PTR ETHERCAT_MASK = ((DWORD_PTR)1 << TARGET_ETHERCAT_CORE);

        // Get system affinity mask
        DWORD_PTR processAffinity = 0;
        DWORD_PTR systemAffinity = 0;
        GetProcessAffinityMask(GetCurrentProcess(), &processAffinity, &systemAffinity);
        
        // If system has no core 4, abort monitor
        if ((systemAffinity & ETHERCAT_MASK) == 0) {
            return;
        }

        while (monitorRunning.load()) {
            RepelOtherProcesses(TARGET_ETHERCAT_CORE);
            
            // Sleep 2s
            for (int i = 0; i < 20; ++i) {
                if (!monitorRunning.load()) break;
                Sleep(100);
            }
        }
    }

    void CpuIsolation::RepelOtherProcesses(int etherCatCoreIndex) {
        HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (hSnapshot == INVALID_HANDLE_VALUE) return;

        PROCESSENTRY32W pe32;
        pe32.dwSize = sizeof(PROCESSENTRY32W);

        const DWORD_PTR ETHERCAT_MASK = ((DWORD_PTR)1 << etherCatCoreIndex);

        if (Process32FirstW(hSnapshot, &pe32)) {
            do {
                // Check if target
                bool isTarget = false;
                for (const auto& target : targetProcessNames) {
                    if (_wcsicmp(pe32.szExeFile, target.c_str()) == 0) {
                        isTarget = true;
                        break;
                    }
                }

                if (isTarget) {
                    HANDLE hProcess = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION, FALSE, pe32.th32ProcessID);
                    if (hProcess) {
                        DWORD_PTR pMask, sMask;
                        if (GetProcessAffinityMask(hProcess, &pMask, &sMask)) {
                            // If running on our core
                            if ((pMask & ETHERCAT_MASK) != 0) {
                                // Exclude our core
                                DWORD_PTR newMask = pMask & ~ETHERCAT_MASK;
                                // If mask becomes empty (was only on our core), default to allowed system mask without our core
                                if (newMask == 0) {
                                    newMask = sMask & ~ETHERCAT_MASK;
                                }
                                
                                if (newMask != 0) {
                                    SetProcessAffinityMask(hProcess, newMask);
                                }
                            }
                        }
                        CloseHandle(hProcess);
                    }
                }
            } while (Process32NextW(hSnapshot, &pe32));
        }
        CloseHandle(hSnapshot);
    }
}
