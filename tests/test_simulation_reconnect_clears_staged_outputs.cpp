#include "EtherCATMaster.h"

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

}  // namespace

int main() {
    EtherCATMaster master;

    if (!Check(master.Initialize(EtherCATMaster::GetSimulationInterfaceName()),
               "simulation initialize should succeed on first connect") ||
        !Check(master.ConfigureSlaves(),
               "simulation slave configuration should succeed on first connect")) {
        return EXIT_FAILURE;
    }

    int outBase = -1;
    int inBase = -1;
    if (!Check(master.GetSlaveIOBaseBytes(1, outBase, inBase),
               "simulation slave 1 should expose IO base offsets")) {
        return EXIT_FAILURE;
    }

    const int targetPositionOffset = outBase + EtherCATMaster::kPdoOutOff_TargetPos;
    const int cyclicCommandOffset = outBase + EtherCATMaster::kPdoOutOff_TargetVel;
    const int32_t stagedTargetPosition = 112233;
    const int32_t stagedCsvVelocity = 445566;

    if (!Check(master.StageOutputBytes(targetPositionOffset,
                                       reinterpret_cast<const uint8_t*>(&stagedTargetPosition),
                                       static_cast<int>(sizeof(stagedTargetPosition))),
               "staging logical target position should succeed before reconnect") ||
        !Check(master.StageOutputBytes(cyclicCommandOffset,
                                       reinterpret_cast<const uint8_t*>(&stagedCsvVelocity),
                                       static_cast<int>(sizeof(stagedCsvVelocity))),
               "staging CSV command should succeed before reconnect")) {
        return EXIT_FAILURE;
    }

    master.Shutdown();

    if (!Check(master.Initialize(EtherCATMaster::GetSimulationInterfaceName()),
               "simulation initialize should succeed after reconnect") ||
        !Check(master.ConfigureSlaves(),
               "simulation slave configuration should succeed after reconnect")) {
        return EXIT_FAILURE;
    }

    if (!Check(master.GetSlaveIOBaseBytes(1, outBase, inBase),
               "simulation slave 1 should expose IO base offsets after reconnect")) {
        return EXIT_FAILURE;
    }

    int32_t targetPositionSnapshot = 1;
    int32_t csvVelocitySnapshot = 1;
    if (!Check(SnapshotI32(master, outBase + EtherCATMaster::kPdoOutOff_TargetPos, targetPositionSnapshot),
               "should snapshot logical target position after reconnect") ||
        !Check(SnapshotI32(master, outBase + EtherCATMaster::kPdoOutOff_TargetVel, csvVelocitySnapshot),
               "should snapshot CSV command velocity after reconnect") ||
        !Check(targetPositionSnapshot == 0,
               "simulation reconnect must not replay stale staged 0x607A values") ||
        !Check(csvVelocitySnapshot == 0,
               "simulation reconnect must not replay stale staged 0x60FF values")) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}