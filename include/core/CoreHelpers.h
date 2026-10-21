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

    // ── Bounded cross-thread UI message send ─────────────────────────────────
    // RULE: code that can run on a non-UI thread (HTTP server workers, SSE
    // broadcaster, MMF/connect workers, watchdogs) must never issue an unbounded
    // SendMessageW to a window or control owned by the UI thread. If the UI thread
    // is not pumping messages — e.g. still inside MainWindow::Create() before
    // MessageLoop() starts, or blocked on a server-side lock — the caller would
    // block forever and can deadlock the whole application (tray-only, unresponsive;
    // see .github/project-memory/repo-facts.md 2026-10-03).
    //
    // Use this helper for queries/mutations that must be applied synchronously when
    // the UI thread is alive. When it times out the return value is 0 and
    // *timedOut (if provided) is true, so callers must have a safe fallback.
    // Same-thread sends still dispatch directly, so UI-thread callers are unaffected.
    inline LRESULT SendUiMessageBounded(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                        UINT timeoutMs = 100, bool* timedOut = nullptr) {
        if (timedOut) *timedOut = false;
        if (!hWnd || !IsWindow(hWnd)) {
            if (timedOut) *timedOut = true;
            return 0;
        }
        DWORD_PTR result = 0;
        if (SendMessageTimeoutW(hWnd, msg, wParam, lParam,
                                SMTO_ABORTIFHUNG | SMTO_BLOCK, timeoutMs, &result) == 0) {
            if (timedOut) *timedOut = true;
            return 0;
        }
        return static_cast<LRESULT>(result);
    }

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
        // EC_NOFRAME: the NIC received no EtherCAT frame at all (SOEM WKC < 0).
        // No slave is reachable, so SDO-based recovery cannot succeed and would
        // just block the master lock for tens of seconds.  Treat as fatal.
        bool no_frame = false;
    };

    enum class WatchdogRecoveryDecision {
        AttemptRecovery = 0,
        DisconnectImmediately,
    };

    inline WatchdogRecoveryDecision EvaluateWatchdogRecoveryDecision(const CommunicationFailureSnapshot& snapshot) {
        if (!snapshot.link_up) {
            return WatchdogRecoveryDecision::DisconnectImmediately;
        }

        if (snapshot.no_frame) {
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

    // Disable NIC interrupt moderation, maximize buffers, and disable
    // latency-inducing offloads for EtherCAT real-time operation.
    // This is the single most impactful userspace optimization for NIC IRQ
    // responsiveness — interrupt moderation alone can add 50-200us of delay.
    // adapterName: friendly name or description of the adapter
    // Returns: true if the PowerShell-based optimization completed (partial
    // failures for individual properties are non-fatal)
    bool OptimizeNicForRealtime(const std::string &adapterName, std::string &logOut);

    // Apply Npcap immediate-mode + mintocopy optimizations to an open pcap handle.
    // These reduce kernel-to-userspace packet delivery latency by avoiding
    // batch buffering in the Npcap driver (saves ~1ms in worst case).
    // Uses runtime dynamic resolution from wpcap.dll — safe to call without
    // Npcap SDK at link time.
    // handle: open pcap_t* from pcap_open / pcap_create / ecx_init
    // logOut: diagnostic output
    // Returns true if at least one optimization was applied
    bool NpcapApplyImmediateMode(void *pcapHandle, std::string &logOut);

    // Disable RSS (Receive Side Scaling) on the specified network adapter.
    // RSS distributes packet processing across multiple CPU cores — harmful for
    // real-time EtherCAT because it introduces cross-core interrupt jitter.
    // friendlyName: Windows adapter display name (e.g. "乙太網路 4")
    // logOut: diagnostic output
    // Returns true on success (including already-disabled state)
    bool DisableAdapterRss(const std::string &friendlyName, std::string &logOut);

    // Lock NIC hardware settings for real-time EtherCAT stability.
    // Writes registry values to disable power management (D3 sleep), interrupt
    // moderation, EEE, DMA coalescing, and flow control on the EtherCAT NIC.
    // Works even when TCP/IP is unbound — resolves WKC instability.
    //   ifname: Npcap device name (e.g. "\\Device\\NPF_{GUID}")
    //   logOut: diagnostic output
    // Returns true if the adapter was found and at least one setting was applied.
    bool LockNicHardwareRealtime(const std::string &ifname, std::string &logOut);

    // Ensure the EtherCAT NIC has a static IPv4 address so Windows keeps the
    // NDIS miniport active (prevents WKC instability from NIC micro-sleep
    // between PDO frames).  Uses link-local 169.254.x.x range with ARP-based
    // conflict avoidance via AddIPAddress.
    //   ifname: Npcap device name (e.g. "rpcap://\\Device\\NPF_{GUID}")
    //   logOut: diagnostic output
    // Returns true if a static IP was assigned or already existed.
    bool EnsureStaticIpOnEthercatNic(const std::string &ifname, std::string &logOut);

    // Ensure TCP/IPv4 (ms_tcpip) is bound to the EtherCAT NIC.
    // On Win11, a NIC without TCP/IP binding causes NDIS miniport idle
    // → NIC micro-sleep between PDO frames → WKC instability.
    // This function enables ms_tcpip via PowerShell before EnsureStaticIp
    // runs, so that GetAdaptersAddresses can find the adapter.
    //   ifname: Npcap device name (e.g. "rpcap://\\Device\\NPF_{GUID}")
    //   logOut: diagnostic output
    // Returns true if ms_tcpip is confirmed enabled (or was already).
    bool EnsureTcpIpv4BoundOnEthercatNic(const std::string &ifname, std::string &logOut);

    // Force-wake the EtherCAT NIC by opening a persistent Npcap handle.
    // When TCP/IP is unbound, Windows stops NDIS OID requests → NIC firmware
    // enters power-saving idle → WKC instability on Win11.
    // This opens a pcap handle that keeps NDIS OID processing active.
    // Call NpcapKeepAliveClose() after SOEM has opened its own handle.
    //   ifname: Npcap device name (e.g. "rpcap://\\Device\\NPF_{GUID}")
    //   logOut: diagnostic output
    // Returns true if handle was opened successfully.
    bool NpcapKeepAliveOpen(const std::string &ifname, std::string &logOut);

    // Close the keep-alive handle opened by NpcapKeepAliveOpen.
    // Safe to call even if no handle is open (no-op).
    //   logOut: diagnostic output
    void NpcapKeepAliveClose(std::string &logOut);

    // ── NVIDIA GPU power management for real-time stability ──
    // DisableDynamicPState prevents GPU clock transitions that trigger
    // long-running nvlddmkm.sys DPCs, the #1 DPC source in ETW traces.
    // Call with disable=true on EtherCAT connect, disable=false on disconnect
    // to restore the original setting.
    //   disable=true  →  set DisableDynamicPstate=1, save previous value
    //   disable=false →  restore previous value
    //   prevValueOut  →  receives the original value ("0" or "1") for later restore
    //   logOut        →  diagnostic output
    // Returns true if the registry was modified successfully (or already set).
    bool NvidiaSetDisableDynamicPState(bool disable, std::string &prevValueOut, std::string &logOut);

    // ── TwinCAT service control for DPC reduction ──
    // TcRouter.sys + TcIo.sys contribute ~50,000 μs DPC in ETW traces.
    // If the system uses SOEM (not TwinCAT) for EtherCAT, these services
    // are unnecessary and should be stopped during EtherCAT operation.
    //   stop=true  → save running service list, stop all Tc* services
    //   stop=false → restore previously running Tc* services
    //   savedListOut → semicolon-separated list of services to restore
    //   logOut       → diagnostic output
    // Returns true on success (even if no Tc* services found).
    bool ControlTwinCatServices(bool stop, std::string &savedListOut, std::string &logOut);

    // System-wide hardware IRQ affinity policy — all devices → CPU 0
    // Uses registry-based MSI/MSI-X interrupt affinity to pin GPU, NVMe SSD,
    // USB host controllers, gaming NIC, and the EtherCAT NIC interrupts to
    // CPU 0, keeping the RT core (CPU 4) free of hardware interrupt traffic.
    // ethercatAdapterName: friendly name / description of the EtherCAT NIC
    //   (used to exclude it from gaming-NIC targeting and to find it for pinning)
    // logOut: detailed per-device operation log
    // Returns: true (individual device failures are logged but non-fatal)
    // Requires Administrator privileges for registry writes; logs warnings and
    // skips without elevation.
    bool ApplySystemWideIrqAffinityToCpu0(const std::string &ethercatAdapterName, std::string &logOut);

    // Redirect USB host controller interrupts away from the RT core.
    // Called at startup before the cyclic thread to prevent USB ISR/DPC
    // from preempting the PDO cycle.  Uses the same registry-based MSI/MSI-X
    // interrupt affinity mechanism as ApplySystemWideIrqAffinityToCpu0 but
    // targets only USB controllers.  Non-fatal when Admin is unavailable.
    // logOut: detailed per-device operation log
    // Returns: true if the step completed (individual device failures are non-fatal)
    bool RedirectUsbIrqAffinityFromRtCore(std::string &logOut);

    // Redirect non-EtherCAT NIC interrupts away from the RT core.
    // Same pattern as USB redirect: writes registry, verifies, no device restart.
    bool RedirectNicIrqFromRtCore(std::string &logOut);

    // Set USB xHCI host controller interrupt frequency to 500 Hz.
    // Writes InterruptModerationInterval registry value (125 µs units × 16 = 2 ms
    // = 500 Hz) under each xHCI controller's Device Parameters key, and also
    // under the global USBXHCI driver Parameters key.  Reduces USB ISR/DPC rate
    // to avoid preempting the PDO cycle.  Requires Admin; non-fatal otherwise.
    // Returns true if at least one controller was successfully written.
    bool SetUsbInterruptRate500Hz(std::string &logOut);

    // Set USB HID device polling rate to 125 Hz (8 ms) globally and per-device.
    // Writes PollDeviceInterval (global, HidUsb\Parameters) and
    // PollIntervalOverride (per HID device, Device Parameters) to the registry.
    // Lowers HID interrupt-transfer polling from typical 1000–250 Hz down to
    // 125 Hz, reducing USB interrupt load on the system.  Requires Admin;
    // non-fatal otherwise.  Returns true if at least one key was written.
    bool SetHidPollingRate(std::string &logOut);

    // Query USB IRQ redirect status for main-window display.
    // Returns false if RedirectUsbIrqAffinityFromRtCore has not yet run.
    // outTargetCpu: the target CPU we attempted to set
    // outActualCpu: the verified CPU after write+restart (-1 if unverified)
    // outNeedsAdmin: true if registry writes failed (likely no Admin)
    // outMsiXRestartOk: true if MSI-X controller was restarted successfully
    // outLineBasedCount: number of line-based (INTx) controllers
    bool GetUsbIrqRedirectStatus(int &outTargetCpu, int &outActualCpu,
                                 bool &outNeedsAdmin, bool &outMsiXRestartOk,
                                 int &outLineBasedCount);

    // Query the effective IRQ CPU for a given adapter (reads registry Affinity Policy)
    // adapterName: friendly name or description used to resolve the device instance ID
    // outCpu: 0-based CPU index when found
    // logOut: diagnostic log
    // Returns true if an affinity mask was found and outCpu is valid
    bool GetAdapterEffectiveIrqCpu(const std::string &adapterName, int &outCpu, std::string &logOut);
    bool GetSystemIrqAffinitySummary(const std::string &ethercatAdapterName, std::string &summary, std::string &logOut);

    // Interrupt type returned by GetAdapterInterruptType
    enum class AdapterInterruptType {
        Unknown,
        LegacyIntx,   // IRQ 0-511 range, cannot be CPU-pinned via registry
        MsiX,         // MSI or MSI-X, affinity can be controlled via registry
    };

    // Detect whether the adapter uses MSI-X or Legacy INTx interrupts.
    // Checks the registry MessageSignaledInterruptProperties\MSISupported key.
    // adapterName: friendly name / description used to resolve device instance ID
    // outType: detected interrupt type
    // outDescription: human-readable string (e.g. "MSI-X" or "Legacy INTx")
    // logOut: diagnostic log
    // Returns true if detection succeeded (even if result is Unknown)
    bool GetAdapterInterruptType(const std::string &adapterName,
                                 AdapterInterruptType &outType,
                                 std::string &outDescription,
                                 std::string &logOut);

    // Query the actual (runtime) IRQ number for the selected adapter via WMI.
    // This is the same value shown in msinfo32 — small numbers (<512) indicate
    // Legacy INTx, large numbers (>0xFFFFFF00) indicate MSI-X.
    // adapterName: friendly name / description used to match the device
    // outIrqNumber: the raw IRQ number (e.g. 31 for Legacy INTx, 4294967249 for MSI-X)
    // outDeviceName: device name as reported by Win32_IRQResource
    // logOut: diagnostic log
    // Returns true if an IRQ was found for the matching device
    bool GetAdapterActualIrqNumber(const std::string &adapterName,
                                   unsigned long long &outIrqNumber,
                                   std::string &outDeviceName,
                                   std::string &logOut);

    // IRQ affinity readiness for EtherCAT operation.
    // Compares the actual (registry) IRQ CPU assignment against the expected
    // target (CPU 6 for EtherCAT NIC).  If they differ, a reboot is required
    // for the registry-based affinity to take effect.
    //
    // adapterName: friendly name used to resolve the device instance
    // outReady: true if IRQ is correctly assigned to the target CPU
    // outMessage: human-readable message describing the status
    // outLog: diagnostic log
    // Returns true if the check completed (outReady is valid)
    bool CheckEthercatIrqAffinityReady(const std::string &adapterName,
                                       bool &outReady,
                                       std::string &outMessage,
                                       std::string &outLog);

    // Apply real-time-friendly Windows power policy (once at startup)
    // Activates Ultimate Performance or High Performance power scheme,
    // disables core parking, USB selective suspend, and disk timeout.
    // logOut: detailed operation log
    // Returns: true (individual failures are logged but non-fatal)
    // Requires Administrator privileges for power-scheme writes.
    bool ApplyRealtimePowerPolicy(std::string &logOut);

    // Set NetworkThrottlingIndex to 0xFFFFFFFF to disable Windows network
    // throttling for real-time packet I/O. This prevents the OS from
    // rate-limiting pcap_sendpacket / pcap_next_ex calls.
    // logOut: detailed operation log
    // Returns: true if set successfully (Admin required for registry write)
    bool SetNetworkThrottlingForRealtime(std::string &logOut);

    // ── Per-processor idle disable for RT core ──
    // Writes PROCESSOR_IDLE_DISABLE=1 under HKLM\...\Control\Processor\{LP}
    // for the RT physical core's primary LP, preventing C-State transitions
    // that add wake latency.  Requires reboot to take effect.
    // Returns true if the registry write succeeded (or key already present).
    bool SetProcessorIdleDisableForRtCore(std::string &logOut);

    // ── Windows Defender exclusion for real-time stability ──
    // Automatically adds the BigCat executable directory to Windows Defender
    // exclusion list.  Prevents MsMpEng.exe DPCs from interfering with the
    // EtherCAT PDO cycle.  Uses Add-MpPreference via PowerShell.
    //   logOut: diagnostic output
    // Returns true if exclusion was confirmed (newly added or already present).
    bool AddDefenderExclusionForSelf(std::string &logOut);

    // Other small helpers can be added here as needed during migration.

}
