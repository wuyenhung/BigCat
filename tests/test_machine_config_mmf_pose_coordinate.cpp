#include "MachineConfigIO.h"
#include "core/CoreHelpers.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

bool Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

bool CheckServoAxisAngles(const WuSOEM::StewartParams& params,
              const std::array<long double, 6>& expected,
              const char* message) {
  for (size_t index = 0; index < expected.size(); ++index) {
    if (params.servoAxisAngle[index] != expected[index]) {
      std::cerr << message << " at index " << index << '\n';
      return false;
    }
  }
  return true;
}

void WriteUtf8File(const std::filesystem::path& path, const std::string& contents) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << contents;
}

std::string MakeMachineXml(const char* coordinateSystemNode) {
    return std::string(R"(<?xml version="1.0" encoding="UTF-8"?>
<MachineParameters>
  <Platform>
    <EdgeA>620</EdgeA>
    <EdgeB>100</EdgeB>
  </Platform>
  <Base>
    <EdgeA>620</EdgeA>
    <EdgeB>100</EdgeB>
  </Base>
  <Actuators>
    <RodLength>495</RodLength>
    <ServoArmLength>300</ServoArmLength>
    <ServoInitialAngle>10</ServoInitialAngle>
    <ServoAngleLimit>75</ServoAngleLimit>
    <ServoAxisAngles>
      <Servo1>120</Servo1>
      <Servo2>300</Servo2>
      <Servo3>0</Servo3>
      <Servo4>180</Servo4>
      <Servo5>240</Servo5>
      <Servo6>60</Servo6>
    </ServoAxisAngles>
  </Actuators>
  <Limits>
    <FisheyeAngleLimit>30</FisheyeAngleLimit>
  </Limits>
)") + coordinateSystemNode + R"(
  <MMF1>
    <InputMode>Pose6D</InputMode>
  </MMF1>
</MachineParameters>
)";
}

}  // namespace

namespace Win32WindowCore {

std::string EscapeJsonString(const std::string& s) {
  return CoreHelpers::EscapeJsonString(s);
}

}  // namespace Win32WindowCore

int main() {
    const std::filesystem::path tempRoot = std::filesystem::temp_directory_path() / "bigcat_machine_config_mmf_pose_coordinate";
    std::error_code ec;
    std::filesystem::remove_all(tempRoot, ec);
    std::filesystem::create_directories(tempRoot, ec);
    if (!Check(!ec, "failed to create temporary test directory")) {
        return EXIT_FAILURE;
    }

    WuSOEM::StewartParams params;
    Mmf1InputMode mode = Mmf1InputMode::ServoAngle;
    Mmf1PoseCoordinateSystem coordinateSystem = Mmf1PoseCoordinateSystem::World;

    const std::filesystem::path platformXml = tempRoot / "machine_platform.xml";
    WriteUtf8File(platformXml, MakeMachineXml("  <InverseInput>\n    <CoordinateSystem>platform</CoordinateSystem>\n  </InverseInput>"));
    if (!MachineConfigIO::LoadMachineParams(platformXml.string(), params, mode, &coordinateSystem) ||
        !Check(mode == Mmf1InputMode::Pose6D, "machine.xml should load Pose6D MMF1 mode") ||
        !Check(coordinateSystem == Mmf1PoseCoordinateSystem::Platform, "machine.xml should load platform coordinate system") ||
      !Check(params.platformEdgeA == 620.0L, "machine.xml should still load Stewart parameters") ||
      !CheckServoAxisAngles(params,
                  std::array<long double, 6>{{120.0L, 300.0L, 0.0L, 180.0L, 240.0L, 60.0L}},
                  "machine.xml should preserve Servo1..6 in direct actuator order")) {
        std::filesystem::remove_all(tempRoot, ec);
        return EXIT_FAILURE;
    }

    mode = Mmf1InputMode::ServoAngle;
    coordinateSystem = Mmf1PoseCoordinateSystem::World;
    const std::filesystem::path trimmedXml = tempRoot / "machine_platform_trimmed.xml";
    WriteUtf8File(trimmedXml, MakeMachineXml("  <InverseInput>\n    <CoordinateSystem>  PLATFORM\r\n    </CoordinateSystem>\n  </InverseInput>\n  <MMF1>\n    <InputMode>  pose6d </InputMode>\n  </MMF1>"));
    if (!MachineConfigIO::LoadMachineParams(trimmedXml.string(), params, mode, &coordinateSystem) ||
      !Check(mode == Mmf1InputMode::Pose6D, "machine.xml should trim MMF1 input mode text") ||
      !Check(coordinateSystem == Mmf1PoseCoordinateSystem::Platform, "machine.xml should trim coordinate-system text")) {
      std::filesystem::remove_all(tempRoot, ec);
      return EXIT_FAILURE;
    }

    mode = Mmf1InputMode::ServoAngle;
    coordinateSystem = Mmf1PoseCoordinateSystem::Platform;
    const std::filesystem::path legacyXml = tempRoot / "machine_legacy.xml";
    WriteUtf8File(legacyXml, MakeMachineXml(""));
    if (!MachineConfigIO::LoadMachineParams(legacyXml.string(), params, mode, &coordinateSystem) ||
        !Check(mode == Mmf1InputMode::Pose6D, "legacy machine.xml should still load MMF1 mode") ||
        !Check(coordinateSystem == Mmf1PoseCoordinateSystem::World, "missing coordinate-system node should default to world")) {
        std::filesystem::remove_all(tempRoot, ec);
        return EXIT_FAILURE;
    }

    std::filesystem::remove_all(tempRoot, ec);
    return EXIT_SUCCESS;
}
