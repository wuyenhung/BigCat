#include "core/SystemState.h"
#include "core/CoreHelpers.h"

#include <cstdlib>
#include <iostream>

namespace {

bool Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

}  // namespace

int main() {
    if (!Check(CoreHelpers::ResolveLinkLossMonitoringPhase(false, true, false, true) == CoreHelpers::LinkLossMonitoringPhase::ConnectInProgress,
         "worker-running without master init should be treated as connect-in-progress") ||
     !Check(CoreHelpers::ResolveLinkLossMonitoringPhase(false, true, true, false) == CoreHelpers::LinkLossMonitoringPhase::Connected,
         "master-initialized state should be treated as connected") ||
     !Check(std::string(CoreHelpers::DescribeLinkLossMonitoringPhase(CoreHelpers::LinkLossMonitoringPhase::ConnectInProgress)) == "connect-in-progress",
         "connect-in-progress phase label should be stable") ||
     !Check(std::string(CoreHelpers::DescribeLinkLossMonitoringPhase(CoreHelpers::LinkLossMonitoringPhase::Connected)) == "connected",
         "connected phase label should be stable")) {
     return EXIT_FAILURE;
    }

    if (!Check(CoreHelpers::EvaluateWatchdogRecoveryDecision({2, 6, true, true}) == CoreHelpers::WatchdogRecoveryDecision::DisconnectImmediately,
         "multi-slave communication loss with global error should skip recovery") ||
        !Check(CoreHelpers::EvaluateWatchdogRecoveryDecision({1, 6, true, true}) == CoreHelpers::WatchdogRecoveryDecision::AttemptRecovery,
         "single-slave communication loss should still allow recovery") ||
        !Check(CoreHelpers::EvaluateWatchdogRecoveryDecision({1, 1, true, true}) == CoreHelpers::WatchdogRecoveryDecision::DisconnectImmediately,
         "single-slave system with total communication loss should disconnect immediately") ||
           !Check(CoreHelpers::EvaluateWatchdogRecoveryDecision({0, 6, true, false, 3, true}) == CoreHelpers::WatchdogRecoveryDecision::DisconnectImmediately,
            "repeated 0x60FD failures should skip recovery before 0x603F matures") ||
        !Check(CoreHelpers::EvaluateWatchdogRecoveryDecision({0, 6, false, false}) == CoreHelpers::WatchdogRecoveryDecision::DisconnectImmediately,
         "physical link down should skip recovery immediately")) {
        return EXIT_FAILURE;
    }

    CoreHelpers::SystemStateSnapshot readySnapshot;
    readySnapshot.ethercat_ok = true;
    readySnapshot.ready = true;
    readySnapshot.home_done = true;
    readySnapshot.master_initialized = true;

    const auto autoConnectDecision = CoreHelpers::EvaluateAutoConnect(readySnapshot);
    if (!Check(autoConnectDecision.allowed, "auto connect should be allowed for a ready idle system")) {
        return EXIT_FAILURE;
    }

    auto notReadySnapshot = readySnapshot;
    notReadySnapshot.ready = false;
    const auto notReadyDecision = CoreHelpers::EvaluateAutoConnect(notReadySnapshot);
    if (!Check(!notReadyDecision.allowed, "auto connect should reject non-ready state") ||
        !Check(notReadyDecision.rejection == CoreHelpers::CommandPolicyRejection::kSystemNotReady, "auto connect should report kSystemNotReady")) {
        return EXIT_FAILURE;
    }

    auto busySnapshot = readySnapshot;
    busySnapshot.busy = true;
    const auto downToOriginDecision = CoreHelpers::EvaluateAutoDownToOrigin(busySnapshot);
    if (!Check(!downToOriginDecision.allowed, "down to origin should reject busy state") ||
        !Check(downToOriginDecision.rejection == CoreHelpers::CommandPolicyRejection::kSystemBusy, "down to origin should report kSystemBusy")) {
        return EXIT_FAILURE;
    }

    auto manualResetSnapshot = readySnapshot;
    manualResetSnapshot.manual_reset_required = true;
    const auto manualResetAutoConnectDecision = CoreHelpers::EvaluateAutoConnect(manualResetSnapshot);
    if (!Check(!manualResetAutoConnectDecision.allowed, "auto connect should reject while manual reset is required") ||
        !Check(manualResetAutoConnectDecision.rejection == CoreHelpers::CommandPolicyRejection::kSystemError, "manual reset lock should surface as system error for auto connect")) {
        return EXIT_FAILURE;
    }

    const auto manualResetDownToOriginDecision = CoreHelpers::EvaluateAutoDownToOrigin(manualResetSnapshot);
    if (!Check(!manualResetDownToOriginDecision.allowed, "down to origin should reject while manual reset is required") ||
        !Check(manualResetDownToOriginDecision.rejection == CoreHelpers::CommandPolicyRejection::kSystemError, "manual reset lock should surface as system error for down to origin")) {
        return EXIT_FAILURE;
    }

    const auto manualResetStartDecision = CoreHelpers::EvaluateStartAutoProcedure(manualResetSnapshot);
    if (!Check(!manualResetStartDecision.allowed, "start auto should reject while only manual reset is required") ||
        !Check(manualResetStartDecision.rejection == CoreHelpers::CommandPolicyRejection::kSystemError, "manual reset-only lock should surface as system error for start auto")) {
        return EXIT_FAILURE;
    }

    auto errorResetStartSnapshot = manualResetSnapshot;
    errorResetStartSnapshot.error = true;
    const auto errorResetStartDecision = CoreHelpers::EvaluateStartAutoProcedure(errorResetStartSnapshot);
    if (!Check(errorResetStartDecision.allowed, "start auto should stay allowed when an active error needs reset first")) {
        return EXIT_FAILURE;
    }

    auto resetSnapshot = readySnapshot;
    resetSnapshot.error = false;
    const auto faultResetDecision = CoreHelpers::EvaluateAutoFaultReset(resetSnapshot);
    if (!Check(!faultResetDecision.allowed, "fault reset should reject a healthy ready system") ||
        !Check(faultResetDecision.rejection == CoreHelpers::CommandPolicyRejection::kResetNotNeeded, "fault reset should report kResetNotNeeded")) {
        return EXIT_FAILURE;
    }

    auto requiredResetSnapshot = readySnapshot;
    requiredResetSnapshot.manual_reset_required = true;
    const auto requiredResetDecision = CoreHelpers::EvaluateAutoFaultReset(requiredResetSnapshot);
    if (!Check(requiredResetDecision.allowed, "fault reset should remain allowed while manual reset is required")) {
        return EXIT_FAILURE;
    }

    CoreHelpers::AutoActionButtonInputs disabledAutoWindowInputs;
    disabledAutoWindowInputs.action_gate_enabled = false;
    const auto disabledAutoWindowButtons =
        CoreHelpers::EvaluateAutoActionButtons(readySnapshot, disabledAutoWindowInputs);
    if (!Check(!disabledAutoWindowButtons.connect_enabled,
               "AutoWindow connect should stay disabled before the auto action gate is enabled") ||
        !Check(!disabledAutoWindowButtons.down_to_origin_enabled,
               "AutoWindow down-to-origin should stay disabled before the auto action gate is enabled") ||
        !Check(!disabledAutoWindowButtons.fault_reset_enabled,
               "AutoWindow fault reset should still follow reset-needed policy when action gate is disabled") ||
        !Check(disabledAutoWindowButtons.stop_enabled,
               "AutoWindow stop buttons should stay enabled when EtherCAT is OK")) {
        return EXIT_FAILURE;
    }

    CoreHelpers::AutoActionButtonInputs enabledAutoWindowInputs;
    enabledAutoWindowInputs.action_gate_enabled = true;
    const auto enabledAutoWindowButtons =
        CoreHelpers::EvaluateAutoActionButtons(readySnapshot, enabledAutoWindowInputs);
    if (!Check(enabledAutoWindowButtons.connect_enabled,
               "AutoWindow connect should enable when action gate and auto-connect policy allow it") ||
        !Check(enabledAutoWindowButtons.down_to_origin_enabled,
               "AutoWindow down-to-origin should enable when action gate and policy allow it")) {
        return EXIT_FAILURE;
    }

    enabledAutoWindowInputs.stop_active = true;
    const auto stoppedAutoWindowButtons =
        CoreHelpers::EvaluateAutoActionButtons(readySnapshot, enabledAutoWindowInputs);
    if (!Check(!stoppedAutoWindowButtons.connect_enabled,
               "AutoWindow connect should disable while a stop input is active") ||
        !Check(!stoppedAutoWindowButtons.down_to_origin_enabled,
               "AutoWindow down-to-origin should disable while a stop input is active")) {
        return EXIT_FAILURE;
    }

    CoreHelpers::AutoActionButtonInputs lampInputs;
    lampInputs.auto_connect_signal_active = true;
    lampInputs.down_to_origin_signal_active = true;
    lampInputs.fault_reset_signal_active = true;
    lampInputs.immediate_stop_signal_active = true;
    lampInputs.emergency_stop_latched = true;
    const auto lampButtons = CoreHelpers::EvaluateAutoActionButtons(readySnapshot, lampInputs);
    if (!Check(lampButtons.connect_lit,
               "connect lamp should follow 0x60FD bit1/override state") ||
        !Check(lampButtons.down_to_origin_lit,
               "down-to-origin lamp should follow 0x60FD bit3/override state") ||
        !Check(lampButtons.fault_reset_lit,
               "fault-reset lamp should follow 0x60FD bit6/override state") ||
        !Check(lampButtons.immediate_stop_lit,
               "immediate-stop lamp should follow bit0 or shared latch state") ||
        !Check(lampButtons.emergency_stop_lit,
               "emergency-stop lamp should follow bit5 or shared latch state")) {
        return EXIT_FAILURE;
    }

    CoreHelpers::LinkLossState linkLossState;
    const auto firstDown = CoreHelpers::EvaluateSustainedLinkLoss(true, false, 100, 1000, linkLossState);
    if (!Check(firstDown.decision == CoreHelpers::LinkLossDecision::StartedTiming, "first link-down sample should start timing") ||
        !Check(linkLossState.down_since_tick == 100, "link-down start tick should be captured") ||
        !Check(!linkLossState.disconnect_requested, "disconnect should not be requested immediately")) {
        return EXIT_FAILURE;
    }

    const auto beforeThreshold = CoreHelpers::EvaluateSustainedLinkLoss(true, false, 900, 1000, linkLossState);
    if (!Check(beforeThreshold.decision == CoreHelpers::LinkLossDecision::None, "link-down below threshold should keep waiting") ||
        !Check(!linkLossState.disconnect_requested, "disconnect should still be pending before threshold")) {
        return EXIT_FAILURE;
    }

    const auto reachedThreshold = CoreHelpers::EvaluateSustainedLinkLoss(true, false, 1100, 1000, linkLossState);
    if (!Check(reachedThreshold.decision == CoreHelpers::LinkLossDecision::TriggerDisconnect, "link-down at threshold should trigger disconnect") ||
        !Check(reachedThreshold.down_duration_ms == 1000, "trigger duration should reflect elapsed link-down time") ||
        !Check(linkLossState.disconnect_requested, "disconnect should latch after trigger")) {
        return EXIT_FAILURE;
    }

    const auto repeatedTrigger = CoreHelpers::EvaluateSustainedLinkLoss(true, false, 1500, 1000, linkLossState);
    if (!Check(repeatedTrigger.decision == CoreHelpers::LinkLossDecision::AlreadyTriggered, "subsequent link-down samples should not retrigger")) {
        return EXIT_FAILURE;
    }

    const auto recovered = CoreHelpers::EvaluateSustainedLinkLoss(true, true, 1700, 1000, linkLossState);
    if (!Check(recovered.decision == CoreHelpers::LinkLossDecision::Recovered, "link recovery should clear pending/latched loss state") ||
        !Check(recovered.down_duration_ms == 1600, "recovery should report total down duration") ||
        !Check(linkLossState.down_since_tick == 0, "recovery should clear down-since tick") ||
        !Check(!linkLossState.disconnect_requested, "recovery should clear disconnect latch")) {
        return EXIT_FAILURE;
    }

    CoreHelpers::SustainedFailureState sdoFailureState;
    const auto firstFailure = CoreHelpers::EvaluateSustainedFailure(true, 100, 1000, sdoFailureState);
    if (!Check(firstFailure.decision == CoreHelpers::SustainedFailureDecision::StartedTiming, "first 0x60FD failure should start timing") ||
        !Check(sdoFailureState.failure_since_tick == 100, "0x60FD failure start tick should be captured")) {
        return EXIT_FAILURE;
    }

    const auto beforeFailureThreshold = CoreHelpers::EvaluateSustainedFailure(true, 900, 1000, sdoFailureState);
    if (!Check(beforeFailureThreshold.decision == CoreHelpers::SustainedFailureDecision::None, "0x60FD failures below threshold should keep waiting")) {
        return EXIT_FAILURE;
    }

    const auto triggerFailureDisconnect = CoreHelpers::EvaluateSustainedFailure(true, 1100, 1000, sdoFailureState);
    if (!Check(triggerFailureDisconnect.decision == CoreHelpers::SustainedFailureDecision::TriggerDisconnect, "0x60FD failures at threshold should trigger disconnect") ||
        !Check(sdoFailureState.disconnect_requested, "0x60FD failure disconnect should latch after trigger")) {
        return EXIT_FAILURE;
    }

    const auto recoverFailure = CoreHelpers::EvaluateSustainedFailure(false, 1200, 1000, sdoFailureState);
    if (!Check(recoverFailure.decision == CoreHelpers::SustainedFailureDecision::Recovered, "0x60FD recovery should clear failure state") ||
        !Check(sdoFailureState.failure_since_tick == 0, "0x60FD recovery should clear failure start tick") ||
        !Check(!sdoFailureState.disconnect_requested, "0x60FD recovery should clear disconnect latch")) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}