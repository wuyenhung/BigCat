#pragma once
#include <vector>
#include <string>
#include <thread>
#include <atomic>

namespace CoreHelpers {

    // CPU Isolation Manager
    // This class handles shielding the EtherCAT core (Core 4) from high-load gaming/VR processes.
    class CpuIsolation {
    public:
        // Initialize isolation:
        // 1. Sets current process (EtherCAT) to High Priority & Core 4.
        // 2. Starts a background thread to monitor and repel other processes from Core 4.
        static void StartIsolationMonitor();

        // Stop the background monitor thread
        static void StopIsolationMonitor();

    private:
        static void MonitorThreadLoop();
        static void RepelOtherProcesses(int etherCatCoreIndex);
        
        static std::atomic<bool> monitorRunning;
        static std::thread monitorThread;
        
        // List of process names to repel from the EtherCAT core
        static const std::vector<std::wstring> targetProcessNames;
    };

}
