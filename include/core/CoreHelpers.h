// Lightweight forwarding header to centralize non-UI helpers for gradual migration.
#pragma once

#include <string>
#include <vector>
#include <cstdint>

// Include winsock2.h before windows.h to avoid conflicts
#ifndef _WINSOCKAPI_
#define _WINSOCKAPI_
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "../Win32Utils.h"
#include "../Win32WindowCore.h"

namespace CoreHelpers {

    enum class LinkLossMonitoringPhase {
        Disabled = 0,
        ConnectInProgress,
        Connected,
    };

    inline LinkLossMonitoringPhase ResolveLinkLossMonitoringPhase(bool disconnecting,
                                                                 bool hasInterface,
                                                                 bool masterInitialized,
                                                                 bool workerRunning) {
        if (disconnecting || !hasInterface) {
            return LinkLossMonitoringPhase::Disabled;
        }
        if (masterInitialized) {
            return LinkLossMonitoringPhase::Connected;
        }
        if (workerRunning) {
            return LinkLossMonitoringPhase::ConnectInProgress;
        }
        return LinkLossMonitoringPhase::Disabled;
    }

    inline const char* DescribeLinkLossMonitoringPhase(LinkLossMonitoringPhase phase) {
        switch (phase) {
        case LinkLossMonitoringPhase::ConnectInProgress:
            return "connect-in-progress";
        case LinkLossMonitoringPhase::Connected:
            return "connected";
        case LinkLossMonitoringPhase::Disabled:
        default:
            return "disabled";
        }
    }

    struct LinkLossState {
        uint64_t down_since_tick = 0;
        bool disconnect_requested = false;
    };

    enum class LinkLossDecision {
        None = 0,
        StartedTiming,
        Recovered,
        TriggerDisconnect,
        AlreadyTriggered,
        ResetWhileDisabled,
    };

    struct LinkLossDecisionResult {
        LinkLossDecision decision = LinkLossDecision::None;
        uint64_t down_duration_ms = 0;
    };

    struct SustainedFailureState {
        uint64_t failure_since_tick = 0;
        bool disconnect_requested = false;
    };

    enum class SustainedFailureDecision {
        None = 0,
        StartedTiming,
        Recovered,
        TriggerDisconnect,
        AlreadyTriggered,
    };

    struct SustainedFailureDecisionResult {
        SustainedFailureDecision decision = SustainedFailureDecision::None;
        uint64_t failure_duration_ms = 0;
    };

    inline SustainedFailureDecisionResult EvaluateSustainedFailure(bool failureActive,
                                                                  uint64_t nowTick,
                                                                  uint64_t disconnectDelayMs,
                                                                  SustainedFailureState& state) {
        if (!failureActive) {
            const uint64_t durationMs = (state.failure_since_tick != 0 && nowTick >= state.failure_since_tick)
                ? (nowTick - state.failure_since_tick)
                : 0;
            const bool hadPendingState = (state.failure_since_tick != 0) || state.disconnect_requested;
            state.failure_since_tick = 0;
            state.disconnect_requested = false;
            return {hadPendingState ? SustainedFailureDecision::Recovered : SustainedFailureDecision::None, durationMs};
        }

        if (state.failure_since_tick == 0) {
            state.failure_since_tick = nowTick;
            state.disconnect_requested = false;
            return {SustainedFailureDecision::StartedTiming, 0};
        }

        const uint64_t durationMs = (nowTick >= state.failure_since_tick) ? (nowTick - state.failure_since_tick) : 0;
        if (durationMs < disconnectDelayMs) {
            return {SustainedFailureDecision::None, durationMs};
        }

        if (state.disconnect_requested) {
            return {SustainedFailureDecision::AlreadyTriggered, durationMs};
        }

        state.disconnect_requested = true;
        return {SustainedFailureDecision::TriggerDisconnect, durationMs};
    }

    struct CommunicationFailureSnapshot {
        int failed_slave_count = 0;
        int total_slave_count = 0;
        bool link_up = true;
        bool global_error_active = false;
        int recent_sdo_failure_streak = 0;
        bool recent_sdo_failure_active = false;
    };

    enum class WatchdogRecoveryDecision {
        AttemptRecovery = 0,
        DisconnectImmediately,
    };

    inline WatchdogRecoveryDecision EvaluateWatchdogRecoveryDecision(const CommunicationFailureSnapshot& snapshot) {
        if (!snapshot.link_up) {
            return WatchdogRecoveryDecision::DisconnectImmediately;
        }

        if (snapshot.recent_sdo_failure_active && snapshot.recent_sdo_failure_streak >= 3) {
            return WatchdogRecoveryDecision::DisconnectImmediately;
        }

        if (!snapshot.global_error_active || snapshot.failed_slave_count <= 0 || snapshot.total_slave_count <= 0) {
            return WatchdogRecoveryDecision::AttemptRecovery;
        }

        const int severe_failure_threshold = (snapshot.total_slave_count <= 2) ? 1 : 2;
        if (snapshot.failed_slave_count >= snapshot.total_slave_count) {
            return WatchdogRecoveryDecision::DisconnectImmediately;
        }
        if (snapshot.failed_slave_count >= severe_failure_threshold) {
            return WatchdogRecoveryDecision::DisconnectImmediately;
        }

        return WatchdogRecoveryDecision::AttemptRecovery;
    }

    inline LinkLossDecisionResult EvaluateSustainedLinkLoss(bool monitoringEnabled,
                                                           bool linkUp,
                                                           uint64_t nowTick,
                                                           uint64_t disconnectDelayMs,
                                                           LinkLossState& state) {
        if (!monitoringEnabled) {
            const bool hadPendingState = (state.down_since_tick != 0) || state.disconnect_requested;
            state.down_since_tick = 0;
            state.disconnect_requested = false;
            return {hadPendingState ? LinkLossDecision::ResetWhileDisabled : LinkLossDecision::None, 0};
        }

        if (linkUp) {
            const uint64_t downDurationMs = (state.down_since_tick != 0 && nowTick >= state.down_since_tick)
                ? (nowTick - state.down_since_tick)
                : 0;
            const bool hadPendingState = (state.down_since_tick != 0) || state.disconnect_requested;
            state.down_since_tick = 0;
            state.disconnect_requested = false;
            return {hadPendingState ? LinkLossDecision::Recovered : LinkLossDecision::None, downDurationMs};
        }

        if (state.down_since_tick == 0) {
            state.down_since_tick = nowTick;
            state.disconnect_requested = false;
            return {LinkLossDecision::StartedTiming, 0};
        }

        const uint64_t downDurationMs = (nowTick >= state.down_since_tick) ? (nowTick - state.down_since_tick) : 0;
        if (downDurationMs < disconnectDelayMs) {
            return {LinkLossDecision::None, downDurationMs};
        }

        if (state.disconnect_requested) {
            return {LinkLossDecision::AlreadyTriggered, downDurationMs};
        }

        state.disconnect_requested = true;
        return {LinkLossDecision::TriggerDisconnect, downDurationMs};
    }

    // Re-export the AdapterEntry type from Win32Utils to avoid type-mismatch.
    using AdapterEntry = ::AdapterEntry;

    // Encoding helpers (forward to Win32Utils implementations)
    std::string WideToUtf8(const std::wstring &w);
    std::wstring Utf8ToWide(const std::string &s);

    // XML / JSON helpers
    std::string EscapeXml(const std::string &s);
    std::string EscapeJsonString(const std::string &s);

    // Hex/byte helpers
    std::wstring BytesToHexDisplay(const uint8_t* data, size_t len, bool bigEndian = true, bool prefix0x = true, bool spaceSeparated = true, bool uppercase = true);
    void ParseTokensToBytes(const std::wstring &tokenStr, std::vector<uint8_t> &outBytes, bool &anyHexToken);

    // Adapter helpers (forward to Win32Utils)
    std::vector<AdapterEntry> EnumerateAdapters(std::string &errOut);
    std::string EnumerateAdaptersJson();
    std::string BuildAdaptersJson(const std::vector<AdapterEntry> &list);
    std::string MakeAdapterDisplayString(const AdapterEntry &d);

    // Link status helper
    bool CheckLinkStatus(const char* ifname);
    // System theme helper
    bool IsSystemDarkMode();

    // Column distribution (forward to Win32WindowCore)
    std::vector<int> DistributeColumns(int totalWidth, int colCount);

    // Process affinity helper
    // Sets process priority to HIGH and locks affinity to a specific core (Core 4/3/2/1)
    void SetProcessHighPriorityAndCore4Affinity();

    // NIC IRQ affinity optimization helper
    // Optimizes network adapter RSS settings to reduce interrupt interference
    // adapterName: friendly name or description of the adapter to optimize
    // Returns: true if optimization succeeded or was skipped, false on error
    bool OptimizeNicIrqAffinity(const std::string &adapterName, std::string &logOut);

    // Best-effort wireless IRQ rebalance helper
    // Moves active WiFi/Bluetooth PAN RSS processing away from the protected RT core when supported
    // Returns: true if the step completed or was skipped, false on local setup error
    bool RebalanceWirelessIrqAffinityAwayFromRtCore(std::string &logOut);

    // Isolate network adapter for EtherCAT exclusive use
    // Disables all TCP/IP protocol bindings to prevent interference from other software
    // adapterName: friendly name or description of the adapter
    // restore: if true, re-enable all bindings (undo isolation)
    // Returns: true if operation succeeded
    bool IsolateEtherCATAdapter(const std::string &adapterName, bool restore, std::string &logOut);

    // Other small helpers can be added here as needed during migration.

}
