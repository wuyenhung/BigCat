#include "EtherCATMaster.h"

#include <array>
#include <cstdint>
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

bool SnapshotI32(EtherCATMaster& master, int offset, int32_t& outValue) {
    outValue = 0;
    return master.SnapshotIOMap(offset, reinterpret_cast<uint8_t*>(&outValue), static_cast<int>(sizeof(outValue))) == sizeof(outValue);
}

struct MasterShutdownGuard {
    EtherCATMaster& master;

    ~MasterShutdownGuard() {
        master.Shutdown();
    }
};

}  // namespace

int main() {
    if (!Check(CoreHelpers::kDefaultCyclicCommandPdoIndex == 0x60FF,
               "CSV regression test expects the default cyclic command PDO to remain 0x60FF")) {
        return EXIT_FAILURE;
    }

    EtherCATMaster master;
    MasterShutdownGuard shutdownGuard{master};

    if (!Check(master.Initialize(EtherCATMaster::GetSimulationInterfaceName()),
               "simulation initialize should succeed") ||
        !Check(master.ConfigureSlaves(),
               "simulation slave configuration should succeed")) {
        return EXIT_FAILURE;
    }

    int outBase = -1;
    int inBase = -1;
    if (!Check(master.GetSlaveIOBaseBytes(1, outBase, inBase),
               "simulation slave 1 should expose IO base offsets")) {
        return EXIT_FAILURE;
    }

    const int targetPositionOffset = outBase + 2;
    const int cyclicCommandOffset = outBase + 6;
    const int32_t stagedTargetPosition = 123456;
    const int32_t stagedCsvVelocity = 654321;

    if (!Check(master.StageOutputBytes(targetPositionOffset,
                                       reinterpret_cast<const uint8_t*>(&stagedTargetPosition),
                                       static_cast<int>(sizeof(stagedTargetPosition))),
               "staging logical target position should succeed") ||
        !Check(master.StageOutputBytes(cyclicCommandOffset,
                                       reinterpret_cast<const uint8_t*>(&stagedCsvVelocity),
                                       static_cast<int>(sizeof(stagedCsvVelocity))),
               "staging CSV command velocity should succeed")) {
        return EXIT_FAILURE;
    }

    int32_t stagedPositionSnapshot = 0;
    int32_t stagedVelocitySnapshot = 0;
    if (!Check(SnapshotI32(master, targetPositionOffset, stagedPositionSnapshot),
               "should snapshot staged logical target position") ||
        !Check(SnapshotI32(master, cyclicCommandOffset, stagedVelocitySnapshot),
               "should snapshot staged CSV command velocity") ||
        !Check(stagedPositionSnapshot == stagedTargetPosition,
               "logical target position snapshot should match staged value") ||
        !Check(stagedVelocitySnapshot == stagedCsvVelocity,
               "CSV command snapshot should match staged value before filtering is disabled")) {
        return EXIT_FAILURE;
    }

    master.EnablePDOFiltering(false);

    int32_t clearedPositionSnapshot = 1;
    int32_t clearedVelocitySnapshot = 1;
    if (!Check(SnapshotI32(master, targetPositionOffset, clearedPositionSnapshot),
               "should snapshot logical target position after filtering disable") ||
        !Check(SnapshotI32(master, cyclicCommandOffset, clearedVelocitySnapshot),
               "should snapshot CSV command velocity after filtering disable") ||
        !Check(clearedPositionSnapshot == 0,
               "disabling PDO filtering must clear the logical target position bytes from IOmap") ||
        !Check(clearedVelocitySnapshot == 0,
               "disabling PDO filtering must clear the active CSV command bytes from IOmap")) {
        return EXIT_FAILURE;
    }

    const int32_t staleSoftStartPosition = 111111;
    const int32_t staleSoftStartVelocity = 777777;
    if (!Check(master.StageOutputBytes(targetPositionOffset,
                                       reinterpret_cast<const uint8_t*>(&staleSoftStartPosition),
                                       static_cast<int>(sizeof(staleSoftStartPosition))),
               "staging stale logical target before soft-start should succeed") ||
        !Check(master.StageOutputBytes(cyclicCommandOffset,
                                       reinterpret_cast<const uint8_t*>(&staleSoftStartVelocity),
                                       static_cast<int>(sizeof(staleSoftStartVelocity))),
               "staging stale CSV command before soft-start should succeed")) {
        return EXIT_FAILURE;
    }

    master.TriggerAllPDOFiltersSoftStart();

    int32_t softStartPositionSnapshot = 1;
    int32_t softStartVelocitySnapshot = 1;
    if (!Check(SnapshotI32(master, targetPositionOffset, softStartPositionSnapshot),
               "should snapshot logical target position after soft-start trigger") ||
        !Check(SnapshotI32(master, cyclicCommandOffset, softStartVelocitySnapshot),
               "should snapshot CSV command velocity after soft-start trigger") ||
        !Check(softStartPositionSnapshot == 0,
               "triggering soft-start must clear stale logical target bytes before the first filter write") ||
        !Check(softStartVelocitySnapshot == 0,
               "triggering soft-start must clear stale CSV command bytes before the first filter write")) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
