#pragma once

#include "AppEvents.h"
#include "IAutoStatusProvider.h"

namespace CoreHelpers {

struct SystemStateSnapshot {
    bool ethercat_ok = false;
    bool busy = false;
    bool error = false;
    bool manual_reset_required = false;
    bool ready = false;
    bool home_done = false;
    bool online = false;
    bool master_initialized = false;
    bool worker_running = false;
};

enum class CommandPolicyRejection {
    kNone = 0,
    kMasterNotInitialized,
    kEtherCATNotOk,
    kSystemBusy,
    kSystemError,
    kSystemNotReady,
    kHomeNotDone,
    kAlreadyOnline,
    kResetNotNeeded,
};

struct CommandPolicyDecision {
    bool allowed = false;
    CommandPolicyRejection rejection = CommandPolicyRejection::kNone;
};

struct AutoActionButtonInputs {
    bool action_gate_enabled = false;
    bool stop_active = false;
    bool auto_connect_override_active = false;
    bool down_to_origin_override_active = false;
    bool fault_reset_override_active = false;
    bool auto_connect_signal_active = false;
    bool down_to_origin_signal_active = false;
    bool fault_reset_signal_active = false;
    bool immediate_stop_signal_active = false;
    bool emergency_stop_signal_active = false;
    bool immediate_stop_latched = false;
    bool emergency_stop_latched = false;
};

struct AutoActionButtonState {
    bool connect_enabled = false;
    bool down_to_origin_enabled = false;
    bool fault_reset_enabled = false;
    bool stop_enabled = false;
    bool immediate_stop_enabled = false;
    bool emergency_stop_enabled = false;
    bool connect_lit = false;
    bool down_to_origin_lit = false;
    bool fault_reset_lit = false;
    bool immediate_stop_lit = false;
    bool emergency_stop_lit = false;
};

inline SystemStateSnapshot BuildSystemStateSnapshot(const AutoStatusSnapshot& autoStatus,
                                                   bool masterInitialized,
                                                   bool workerRunning) {
    SystemStateSnapshot snapshot;
    snapshot.ethercat_ok = autoStatus.ethercat_ok;
    snapshot.busy = autoStatus.busy;
    snapshot.error = autoStatus.error;
    snapshot.manual_reset_required = autoStatus.manual_reset_required;
    snapshot.ready = autoStatus.ready;
    snapshot.home_done = autoStatus.home_done;
    snapshot.online = autoStatus.online;
    snapshot.master_initialized = masterInitialized;
    snapshot.worker_running = workerRunning;
    return snapshot;
}

inline AutoStatusSnapshot ToAutoStatusSnapshot(const SystemStateSnapshot& snapshot) {
    AutoStatusSnapshot autoStatus;
    autoStatus.ethercat_ok = snapshot.ethercat_ok;
    autoStatus.busy = snapshot.busy;
    autoStatus.error = snapshot.error;
    autoStatus.manual_reset_required = snapshot.manual_reset_required;
    autoStatus.ready = snapshot.ready;
    autoStatus.home_done = snapshot.home_done;
    autoStatus.online = snapshot.online;
    return autoStatus;
}

inline StatusEvent ToStatusEvent(const SystemStateSnapshot& snapshot) {
    StatusEvent evt;
    evt.ethercat_ok = snapshot.ethercat_ok;
    evt.busy = snapshot.busy;
    evt.error = snapshot.error;
    evt.ready = snapshot.ready;
    evt.home_done = snapshot.home_done;
    evt.online = snapshot.online;
    evt.master_initialized = snapshot.master_initialized;
    evt.worker_running = snapshot.worker_running;
    return evt;
}

inline CommandPolicyDecision AllowCommand() {
    return {true, CommandPolicyRejection::kNone};
}

inline CommandPolicyDecision RejectCommand(CommandPolicyRejection rejection) {
    return {false, rejection};
}

inline const char* DescribeCommandPolicyRejection(CommandPolicyRejection rejection) {
    switch (rejection) {
    case CommandPolicyRejection::kNone:
        return "none";
    case CommandPolicyRejection::kMasterNotInitialized:
        return "master not initialized";
    case CommandPolicyRejection::kEtherCATNotOk:
        return "EtherCAT not OK";
    case CommandPolicyRejection::kSystemBusy:
        return "system busy";
    case CommandPolicyRejection::kSystemError:
        return "system error";
    case CommandPolicyRejection::kSystemNotReady:
        return "system not ready";
    case CommandPolicyRejection::kHomeNotDone:
        return "home not done";
    case CommandPolicyRejection::kAlreadyOnline:
        return "already online";
    case CommandPolicyRejection::kResetNotNeeded:
        return "system ready and no error - reset not needed";
    }

    return "unknown";
}

inline CommandPolicyDecision EvaluateStartAutoProcedure(const SystemStateSnapshot& snapshot) {
    if (!snapshot.master_initialized) {
        return RejectCommand(CommandPolicyRejection::kMasterNotInitialized);
    }
    // Start Auto already begins with the shared fault-reset / power-on preamble.
    // Allow that path to run when a real error is still active, but keep the
    // manual-reset-only latch from stop flows locked behind explicit reset.
    if (snapshot.manual_reset_required && !snapshot.error) {
        return RejectCommand(CommandPolicyRejection::kSystemError);
    }
    if (snapshot.busy) {
        return RejectCommand(CommandPolicyRejection::kSystemBusy);
    }
    return AllowCommand();
}

inline CommandPolicyDecision EvaluateAutoConnect(const SystemStateSnapshot& snapshot) {
    if (!snapshot.ethercat_ok) {
        return RejectCommand(CommandPolicyRejection::kEtherCATNotOk);
    }
    if (!snapshot.ready) {
        return RejectCommand(CommandPolicyRejection::kSystemNotReady);
    }
    if (!snapshot.home_done) {
        return RejectCommand(CommandPolicyRejection::kHomeNotDone);
    }
    if (snapshot.manual_reset_required) {
        return RejectCommand(CommandPolicyRejection::kSystemError);
    }
    if (snapshot.error) {
        return RejectCommand(CommandPolicyRejection::kSystemError);
    }
    if (snapshot.busy) {
        return RejectCommand(CommandPolicyRejection::kSystemBusy);
    }
    if (snapshot.online) {
        return RejectCommand(CommandPolicyRejection::kAlreadyOnline);
    }
    return AllowCommand();
}

inline CommandPolicyDecision EvaluateAutoFaultReset(const SystemStateSnapshot& snapshot) {
    if (!snapshot.ethercat_ok) {
        return RejectCommand(CommandPolicyRejection::kEtherCATNotOk);
    }
    if (snapshot.busy) {
        return RejectCommand(CommandPolicyRejection::kSystemBusy);
    }
    if (snapshot.ready && !snapshot.error && !snapshot.manual_reset_required) {
        return RejectCommand(CommandPolicyRejection::kResetNotNeeded);
    }
    return AllowCommand();
}

inline CommandPolicyDecision EvaluateAutoDownToOrigin(const SystemStateSnapshot& snapshot) {
    if (snapshot.busy) {
        return RejectCommand(CommandPolicyRejection::kSystemBusy);
    }
    if (snapshot.manual_reset_required) {
        return RejectCommand(CommandPolicyRejection::kSystemError);
    }
    if (snapshot.error) {
        return RejectCommand(CommandPolicyRejection::kSystemError);
    }
    if (!snapshot.ethercat_ok) {
        return RejectCommand(CommandPolicyRejection::kEtherCATNotOk);
    }
    return AllowCommand();
}

inline AutoActionButtonState EvaluateAutoActionButtons(const SystemStateSnapshot& snapshot,
                                                       const AutoActionButtonInputs& inputs) {
    AutoActionButtonState state;
    const bool faultResetOnlyMode = snapshot.error || snapshot.manual_reset_required;

    const auto autoConnectDecision = EvaluateAutoConnect(snapshot);
    const auto downToOriginDecision = EvaluateAutoDownToOrigin(snapshot);
    const auto autoFaultResetDecision = EvaluateAutoFaultReset(snapshot);

    state.connect_enabled = !inputs.auto_connect_override_active &&
                            !faultResetOnlyMode &&
                            inputs.action_gate_enabled &&
                            !inputs.stop_active &&
                            autoConnectDecision.allowed;
    state.down_to_origin_enabled = !faultResetOnlyMode &&
                                   !inputs.down_to_origin_override_active &&
                                   inputs.action_gate_enabled &&
                                   !inputs.stop_active &&
                                   downToOriginDecision.allowed;
    state.fault_reset_enabled = !inputs.fault_reset_override_active &&
                                autoFaultResetDecision.allowed;
    state.stop_enabled = snapshot.ethercat_ok;
    state.immediate_stop_enabled = state.stop_enabled;
    state.emergency_stop_enabled = state.stop_enabled;
    state.connect_lit = inputs.auto_connect_override_active ||
                        inputs.auto_connect_signal_active;
    state.down_to_origin_lit = inputs.down_to_origin_override_active ||
                               inputs.down_to_origin_signal_active;
    state.fault_reset_lit = inputs.fault_reset_override_active ||
                            inputs.fault_reset_signal_active;
    state.immediate_stop_lit = inputs.immediate_stop_latched ||
                               inputs.immediate_stop_signal_active;
    state.emergency_stop_lit = inputs.emergency_stop_latched ||
                               inputs.emergency_stop_signal_active;
    return state;
}

}  // namespace CoreHelpers
