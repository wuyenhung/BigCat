#include "EtherCATMaster.h"

#include <string>

int main()
{
    EtherCATMaster::SetPhysicalEthercatAllowed(false, "unit-test policy");
    if (EtherCATMaster::IsPhysicalEthercatAllowed()) {
        return 1;
    }
    if (EtherCATMaster::GetPhysicalEthercatRestrictionReason().find("unit-test policy") == std::string::npos) {
        return 2;
    }

    EtherCATMaster physicalMaster;
    if (physicalMaster.Initialize("unit-test-physical-interface")) {
        return 3;
    }
    if (physicalMaster.GetLastError().find("Physical EtherCAT communication disabled") == std::string::npos) {
        return 4;
    }

    EtherCATMaster simulationMaster;
    if (!simulationMaster.Initialize(EtherCATMaster::GetSimulationInterfaceName())) {
        return 5;
    }
    if (!simulationMaster.IsSimulationMode()) {
        simulationMaster.Shutdown();
        return 6;
    }

    simulationMaster.Shutdown();
    EtherCATMaster::SetPhysicalEthercatAllowed(true);
    return 0;
}