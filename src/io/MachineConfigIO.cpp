#include "../include/MachineConfigIO.h"
#include "../include/core/CoreHelpers.h"
#include <cctype>
#include <fstream>
#include <sstream>
#include <tinyxml2.h>
#include <windows.h>
#include <filesystem>

namespace {

std::string TrimAsciiWhitespace(std::string value) {
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return std::string();
    }

    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool ValidateMachineConfigXmlContent(const std::string& xmlContent,
                                     std::string* errorMessage) {
    tinyxml2::XMLDocument doc;
    if (doc.Parse(xmlContent.c_str(), xmlContent.size()) != tinyxml2::XML_SUCCESS) {
        if (errorMessage) {
            const char* parseError = doc.ErrorStr();
            *errorMessage = std::string("invalid machine.xml: ") +
                            (parseError ? parseError : "XML parse failed");
        }
        return false;
    }

    if (!doc.FirstChildElement("MachineParameters")) {
        if (errorMessage) {
            *errorMessage = "invalid machine.xml: missing MachineParameters root";
        }
        return false;
    }

    if (errorMessage) {
        errorMessage->clear();
    }
    return true;
}

}  // namespace

namespace MachineConfigIO {

std::string GetDefaultMachineConfigPath() {
    wchar_t modulePath[MAX_PATH] = {0};
    if (GetModuleFileNameW(NULL, modulePath, MAX_PATH) == 0) {
        return "Par/machine.xml";
    }

    const std::filesystem::path configPath = std::filesystem::path(modulePath).parent_path() / L"Par" / L"machine.xml";
    return CoreHelpers::WideToUtf8(configPath.wstring());
}

bool LoadMachineConfigXml(const std::string& xmlPath,
                          std::string& xmlContent,
                          std::string* errorMessage) {
    std::ifstream input(xmlPath, std::ios::binary);
    if (!input.is_open()) {
        if (errorMessage) {
            *errorMessage = "failed to open machine.xml";
        }
        return false;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (!input.good() && !input.eof()) {
        if (errorMessage) {
            *errorMessage = "failed to read machine.xml";
        }
        return false;
    }

    xmlContent = buffer.str();
    if (!ValidateMachineConfigXmlContent(xmlContent, errorMessage)) {
        return false;
    }

    return true;
}

// Built-in default machine.xml content (factory defaults for DS_P100E-7 platform).
// Used when the file is missing or corrupt — the rebuilt file will match this template.
std::string BuildHardcodedDefaultMachineXml() {
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
    <RodLength>490</RodLength>
    <ServoArmLength>300</ServoArmLength>
    <ServoInitialAngle>10</ServoInitialAngle>
    <ServoAngleLimit>65</ServoAngleLimit>
    <ServoAxisAngles>
      <Servo1>120</Servo1>
      <Servo2>300</Servo2>
      <Servo3>0</Servo3>
      <Servo4>180</Servo4>
      <Servo5>240</Servo5>
      <Servo6>60</Servo6>
    </ServoAxisAngles>
    <ServoPointMap>
      <Servo1>2</Servo1>
      <Servo2>1</Servo2>
      <Servo3>0</Servo3>
      <Servo4>5</Servo4>
      <Servo5>4</Servo5>
      <Servo6>3</Servo6>
    </ServoPointMap>
  </Actuators>
  <Limits>
    <FisheyeAngleLimit>35</FisheyeAngleLimit>
    <Singularity>
      <VerticalAngle>18</VerticalAngle>
      <HorizontalAngle>87</HorizontalAngle>
      <ExtendRatio>95</ExtendRatio>
      <ContractRatio>105</ContractRatio>
    </Singularity>
    <MaxPoseLimits>
      <X>150</X>
      <Y>150</Y>
      <Z>150</Z>
      <Roll>25</Roll>
      <Pitch>25</Pitch>
      <Yaw>10</Yaw>
      <DynamicLimitStartRatio>0.50</DynamicLimitStartRatio>
      <DynamicLimitMinScale>0.50</DynamicLimitMinScale>
    </MaxPoseLimits>
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

bool LoadMachineConfigXmlWithDefaults(const std::string& xmlPath,
                                      std::string& xmlContent,
                                      bool& restartRequired,
                                      std::string* errorMessage) {
    restartRequired = false;

    // Try loading the existing file
    std::string loadError;
    if (LoadMachineConfigXml(xmlPath, xmlContent, &loadError)) {
        return true;  // File exists and is valid — nothing to do
    }

    // File is missing or corrupt — rebuild from built-in defaults
    const std::string defaultXml = BuildHardcodedDefaultMachineXml();
    xmlContent = defaultXml;

    // Ensure parent directory exists
    std::error_code ec;
    const auto parentPath = std::filesystem::path(xmlPath).parent_path();
    if (!parentPath.empty()) {
        std::filesystem::create_directories(parentPath, ec);
        if (ec) {
            if (errorMessage) {
                *errorMessage = "failed to create machine.xml directory for rebuild";
            }
            return false;
        }
    }

    // Write the rebuilt file
    std::ofstream output(xmlPath, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        if (errorMessage) {
            *errorMessage = "failed to write rebuilt machine.xml";
        }
        return false;
    }
    output << defaultXml;
    output.flush();
    if (!output.good()) {
        if (errorMessage) {
            *errorMessage = "failed to flush rebuilt machine.xml";
        }
        return false;
    }

    restartRequired = true;
    if (errorMessage) {
        errorMessage->clear();
    }
    return false;  // Caller must prompt restart
}

bool SaveMachineConfigXml(const std::string& xmlPath,
                          const std::string& xmlContent,
                          std::string* errorMessage) {
    if (!ValidateMachineConfigXmlContent(xmlContent, errorMessage)) {
        return false;
    }

    std::error_code ec;
    const auto parentPath = std::filesystem::path(xmlPath).parent_path();
    if (!parentPath.empty()) {
        std::filesystem::create_directories(parentPath, ec);
        if (ec) {
            if (errorMessage) {
                *errorMessage = "failed to create machine.xml directory";
            }
            return false;
        }
    }

    std::ofstream output(xmlPath, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        if (errorMessage) {
            *errorMessage = "failed to open machine.xml for writing";
        }
        return false;
    }

    output << xmlContent;
    output.flush();
    if (!output.good()) {
        if (errorMessage) {
            *errorMessage = "failed to write machine.xml";
        }
        return false;
    }

    if (errorMessage) {
        errorMessage->clear();
    }
    return true;
}

static bool TryDouble(const tinyxml2::XMLElement* parent, const char* name, long double& out) {
    if (!parent) return false;
    const auto* el = parent->FirstChildElement(name);
    if (!el || !el->GetText()) return false;
    try {
        out = std::stold(el->GetText());
        return true;
    } catch (...) {
        return false;
    }
}

bool LoadMachineParams(const std::string& xmlPath,
                       WuSOEM::StewartParams& out,
                       Mmf1InputMode& mode,
                       Mmf1PoseCoordinateSystem* coordinateSystem,
                       WuSOEM::Pose6DLimits* pose6dLimits) {
    tinyxml2::XMLDocument doc;
    if (doc.LoadFile(xmlPath.c_str()) != tinyxml2::XML_SUCCESS)
        return false;

    const auto* root = doc.FirstChildElement("MachineParameters");
    if (!root) return false;

    // Platform
    if (const auto* platform = root->FirstChildElement("Platform")) {
        TryDouble(platform, "EdgeA", out.platformEdgeA);
        TryDouble(platform, "EdgeB", out.platformEdgeB);
    }

    // Base
    if (const auto* base = root->FirstChildElement("Base")) {
        TryDouble(base, "EdgeA", out.baseEdgeA);
        TryDouble(base, "EdgeB", out.baseEdgeB);
    }

    // Actuators
    if (const auto* act = root->FirstChildElement("Actuators")) {
        TryDouble(act, "RodLength", out.rodLength);
        TryDouble(act, "ServoArmLength", out.servoArmLength);
        TryDouble(act, "ServoInitialAngle", out.servoInitialAngle);
        TryDouble(act, "ServoAngleLimit", out.servoAngleLimit);

        if (const auto* angles = act->FirstChildElement("ServoAxisAngles")) {
            const char* names[] = {"Servo1","Servo2","Servo3","Servo4","Servo5","Servo6"};
            for (int i = 0; i < 6; ++i) {
                TryDouble(angles, names[i], out.servoAxisAngle[i]);
            }
        }

        if (const auto* pmap = act->FirstChildElement("ServoPointMap")) {
            const char* names[] = {"Servo1","Servo2","Servo3","Servo4","Servo5","Servo6"};
            for (int i = 0; i < 6; ++i) {
                if (const auto* el = pmap->FirstChildElement(names[i])) {
                    int val = 0;
                    if (el->QueryIntText(&val) == tinyxml2::XML_SUCCESS && val >= 0 && val <= 5) {
                        out.servoPointMap[i] = val;
                    }
                }
            }
        }
    }

    // Limits
    if (const auto* limits = root->FirstChildElement("Limits")) {
        TryDouble(limits, "FisheyeAngleLimit", out.fisheyeAngleLimit);
        // Parse MaxPoseLimits (硬限制值)
        if (const auto* maxPose = limits->FirstChildElement("MaxPoseLimits")) {
            if (pose6dLimits) {
                TryDouble(maxPose, "X", pose6dLimits->maxX);
                TryDouble(maxPose, "Y", pose6dLimits->maxY);
                TryDouble(maxPose, "Z", pose6dLimits->maxZ);
                TryDouble(maxPose, "Roll", pose6dLimits->maxRoll);
                TryDouble(maxPose, "Pitch", pose6dLimits->maxPitch);
                TryDouble(maxPose, "Yaw", pose6dLimits->maxYaw);
                TryDouble(maxPose, "DynamicLimitStartRatio", pose6dLimits->dynamicLimitStartRatio);
                TryDouble(maxPose, "DynamicLimitMinScale", pose6dLimits->dynamicLimitMinScale);
            }
        }
    }

    if (coordinateSystem) {
        *coordinateSystem = Mmf1PoseCoordinateSystem::World;
        if (const auto* inverseInput = root->FirstChildElement("InverseInput")) {
            if (const auto* cs = inverseInput->FirstChildElement("CoordinateSystem")) {
                if (const char* text = cs->GetText()) {
                    std::string value = TrimAsciiWhitespace(text);
                    for (char& ch : value) {
                        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    }
                    if (value == "platform") {
                        *coordinateSystem = Mmf1PoseCoordinateSystem::Platform;
                    }
                }
            }
        }
    }

    // MMF1 input mode (optional, defaults to ServoAngle)
    if (const auto* mmf1 = root->FirstChildElement("MMF1")) {
        if (const auto* im = mmf1->FirstChildElement("InputMode")) {
            if (im->GetText()) {
                std::string val = TrimAsciiWhitespace(im->GetText());
                if (val == "Pose6D" || val == "pose6d" || val == "POSE6D") {
                    mode = Mmf1InputMode::Pose6D;
                } else {
                    mode = Mmf1InputMode::ServoAngle;
                }
            }
        }
    }

    return true;
}

bool SaveMmf1InputMode(const std::string& xmlPath, Mmf1InputMode mode) {
    tinyxml2::XMLDocument doc;
    if (doc.LoadFile(xmlPath.c_str()) != tinyxml2::XML_SUCCESS)
        return false;

    auto* root = doc.FirstChildElement("MachineParameters");
    if (!root) return false;

    // Find or create <MMF1><InputMode>
    auto* mmf1 = root->FirstChildElement("MMF1");
    if (!mmf1) {
        mmf1 = doc.NewElement("MMF1");
        root->InsertEndChild(mmf1);
    }
    auto* im = mmf1->FirstChildElement("InputMode");
    if (!im) {
        im = doc.NewElement("InputMode");
        mmf1->InsertEndChild(im);
    }

    const char* val = (mode == Mmf1InputMode::Pose6D) ? "Pose6D" : "ServoAngle";
    im->SetText(val);

    return doc.SaveFile(xmlPath.c_str()) == tinyxml2::XML_SUCCESS;
}

} // namespace MachineConfigIO
