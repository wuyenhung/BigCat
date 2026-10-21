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

void WriteUtf8File(const std::filesystem::path& path, const std::string& contents) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << contents;
}

std::string MakeMachineXml() {
    return R"(<?xml version="1.0" encoding="UTF-8"?>
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
  <InverseInput>
    <CoordinateSystem>platform</CoordinateSystem>
  </InverseInput>
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
    const std::filesystem::path tempRoot =
        std::filesystem::temp_directory_path() / "bigcat_machine_config_xml_io";
    std::error_code ec;
    std::filesystem::remove_all(tempRoot, ec);
    std::filesystem::create_directories(tempRoot, ec);
    if (!Check(!ec, "failed to create temporary test directory")) {
        return EXIT_FAILURE;
    }

    const std::filesystem::path targetFile = tempRoot / "Par" / "machine.xml";
    const std::string expectedXml = MakeMachineXml();

    std::string errorMessage;
    if (!MachineConfigIO::SaveMachineConfigXml(targetFile.string(), expectedXml, &errorMessage) ||
        !Check(errorMessage.empty(), "valid machine.xml save should not report an error") ||
        !Check(std::filesystem::exists(targetFile), "valid machine.xml save should create the target file")) {
        std::filesystem::remove_all(tempRoot, ec);
        return EXIT_FAILURE;
    }

    std::string loadedXml;
    if (!MachineConfigIO::LoadMachineConfigXml(targetFile.string(), loadedXml, &errorMessage) ||
        !Check(errorMessage.empty(), "valid machine.xml load should not report an error") ||
        !Check(loadedXml == expectedXml, "loaded machine.xml should match the saved XML content")) {
        std::filesystem::remove_all(tempRoot, ec);
        return EXIT_FAILURE;
    }

    if (!Check(!MachineConfigIO::SaveMachineConfigXml(targetFile.string(), "<MachineParameters>", &errorMessage),
               "invalid machine.xml content should be rejected") ||
        !Check(errorMessage.find("invalid machine.xml:") == 0,
               "invalid machine.xml save should report a validation error")) {
        std::filesystem::remove_all(tempRoot, ec);
        return EXIT_FAILURE;
    }

    loadedXml.clear();
    if (!MachineConfigIO::LoadMachineConfigXml(targetFile.string(), loadedXml, &errorMessage) ||
        !Check(loadedXml == expectedXml, "failed machine.xml save must not clobber the existing file")) {
        std::filesystem::remove_all(tempRoot, ec);
        return EXIT_FAILURE;
    }

    WriteUtf8File(targetFile, "<MachineParameters>");
    loadedXml.clear();
    if (!Check(!MachineConfigIO::LoadMachineConfigXml(targetFile.string(), loadedXml, &errorMessage),
               "invalid machine.xml on disk should fail load") ||
        !Check(errorMessage.find("invalid machine.xml:") == 0,
               "invalid machine.xml load should report a validation error")) {
        std::filesystem::remove_all(tempRoot, ec);
        return EXIT_FAILURE;
    }

    // --- Test LoadMachineConfigXmlWithDefaults: missing file → rebuild ---
    std::filesystem::remove(targetFile, ec);
    {
        std::string xmlContent;
        bool restartRequired = false;
        std::string rebuildError;
        bool ok = MachineConfigIO::LoadMachineConfigXmlWithDefaults(targetFile.string(), xmlContent, restartRequired, &rebuildError);
        if (!Check(!ok, "missing machine.xml should return false for LoadMachineConfigXmlWithDefaults") ||
            !Check(restartRequired, "missing machine.xml should set restartRequired") ||
            !Check(std::filesystem::exists(targetFile), "missing machine.xml should be rebuilt on disk") ||
            !Check(xmlContent.find("<MachineParameters>") != std::string::npos, "rebuilt XML should have MachineParameters root") ||
            !Check(xmlContent.find("<RodLength>490</RodLength>") != std::string::npos, "rebuilt XML should contain default RodLength")) {
            std::filesystem::remove_all(tempRoot, ec);
            return EXIT_FAILURE;
        }
    }

    // --- Test LoadMachineConfigXmlWithDefaults: corrupt file → rebuild ---
    WriteUtf8File(targetFile, "<MachineParameters>");
    {
        std::string xmlContent;
        bool restartRequired = false;
        std::string rebuildError;
        bool ok = MachineConfigIO::LoadMachineConfigXmlWithDefaults(targetFile.string(), xmlContent, restartRequired, &rebuildError);
        if (!Check(!ok, "corrupt machine.xml should return false for LoadMachineConfigXmlWithDefaults") ||
            !Check(restartRequired, "corrupt machine.xml should set restartRequired") ||
            !Check(xmlContent.find("<RodLength>490</RodLength>") != std::string::npos, "rebuilt from corrupt should contain default values")) {
            std::filesystem::remove_all(tempRoot, ec);
            return EXIT_FAILURE;
        }
    }

    // --- Test LoadMachineConfigXmlWithDefaults: valid file → normal load ---
    WriteUtf8File(targetFile, expectedXml);
    {
        std::string xmlContent;
        bool restartRequired = true;  // Start true to verify it's cleared
        std::string rebuildError;
        bool ok = MachineConfigIO::LoadMachineConfigXmlWithDefaults(targetFile.string(), xmlContent, restartRequired, &rebuildError);
        if (!Check(ok, "valid machine.xml should return true for LoadMachineConfigXmlWithDefaults") ||
            !Check(!restartRequired, "valid machine.xml should clear restartRequired") ||
            !Check(xmlContent == expectedXml, "valid machine.xml should return original content unchanged")) {
            std::filesystem::remove_all(tempRoot, ec);
            return EXIT_FAILURE;
        }
    }

    std::filesystem::remove_all(tempRoot, ec);
    return EXIT_SUCCESS;
}