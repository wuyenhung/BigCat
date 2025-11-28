#include "app/AppController.h"

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
    AutoStatusSnapshot autoStatus;
    autoStatus.ethercat_ok = true;
    autoStatus.busy = false;
    autoStatus.error = false;
    autoStatus.manual_reset_required = false;
    autoStatus.ready = true;
    autoStatus.home_done = true;
    autoStatus.online = false;

    const auto snapshot = AppController::BuildStatusSnapshot(autoStatus, true, false);
    if (!Check(snapshot.ethercat_ok, "snapshot should preserve ethercat_ok") ||
        !Check(snapshot.ready, "snapshot should preserve ready") ||
        !Check(snapshot.home_done, "snapshot should preserve home_done") ||
        !Check(!snapshot.manual_reset_required, "snapshot should preserve manual_reset_required") ||
        !Check(snapshot.master_initialized, "snapshot should preserve master_initialized") ||
        !Check(!snapshot.worker_running, "snapshot should preserve worker_running")) {
        return EXIT_FAILURE;
    }

    if (!Check(AppController::CanStartAutoProcedure(snapshot), "auto procedure should be allowed when initialized and not busy")) {
        return EXIT_FAILURE;
    }

    if (!Check(AppController::CanRequestAutoConnect(snapshot), "auto connect should be allowed when initialized and ready")) {
        return EXIT_FAILURE;
    }

    if (!Check(AppController::CanRequestAutoDownToOrigin(snapshot), "down to origin should be allowed when initialized and ready")) {
        return EXIT_FAILURE;
    }

    auto busySnapshot = snapshot;
    busySnapshot.busy = true;
    if (!Check(!AppController::CanStartAutoProcedure(busySnapshot), "auto procedure should be blocked while busy")) {
        return EXIT_FAILURE;
    }

    if (!Check(!AppController::CanRequestAutoDownToOrigin(busySnapshot), "down to origin should be blocked while busy")) {
        return EXIT_FAILURE;
    }

    auto uninitializedSnapshot = snapshot;
    uninitializedSnapshot.master_initialized = false;
    if (!Check(!AppController::CanStartAutoProcedure(uninitializedSnapshot), "auto procedure should be blocked before master init")) {
        return EXIT_FAILURE;
    }

    auto notReadySnapshot = snapshot;
    notReadySnapshot.ready = false;
    if (!Check(!AppController::CanRequestAutoConnect(notReadySnapshot), "auto connect should be blocked before ready")) {
        return EXIT_FAILURE;
    }

    auto healthySnapshot = snapshot;
    healthySnapshot.error = false;
    if (!Check(!AppController::CanRequestAutoFaultReset(healthySnapshot), "fault reset should be blocked when the system is already healthy")) {
        return EXIT_FAILURE;
    }

    auto resetRequiredSnapshot = snapshot;
    resetRequiredSnapshot.manual_reset_required = true;
    if (!Check(!AppController::CanStartAutoProcedure(resetRequiredSnapshot), "auto procedure should stay blocked for manual-reset-only latch")) {
        return EXIT_FAILURE;
    }

    auto errorResetSnapshot = resetRequiredSnapshot;
    errorResetSnapshot.error = true;
    if (!Check(AppController::CanStartAutoProcedure(errorResetSnapshot), "auto procedure should be allowed when start flow must reset an active error first")) {
        return EXIT_FAILURE;
    }

    if (!Check(AppController::CanRequestAutoFaultReset(resetRequiredSnapshot), "fault reset should remain allowed while manual reset is required")) {
        return EXIT_FAILURE;
    }

    const auto statusEvent = AppController::ToStatusEvent(snapshot);
    if (!Check(statusEvent.ethercat_ok == snapshot.ethercat_ok, "status event should preserve ethercat_ok") ||
        !Check(statusEvent.ready == snapshot.ready, "status event should preserve ready") ||
        !Check(statusEvent.master_initialized == snapshot.master_initialized, "status event should preserve master_initialized") ||
        !Check(statusEvent.worker_running == snapshot.worker_running, "status event should preserve worker_running")) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}