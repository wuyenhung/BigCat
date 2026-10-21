#pragma once

#include <string>
#include "StewartIK.h"

// MMF1 input interpretation mode (set at startup from machine.xml, requires restart)
enum class Mmf1InputMode : uint8_t {
    ServoAngle = 0,  // Default: 6 floats = 6 individual servo angles (degrees)
    Pose6D     = 1   // 6 floats = (x, y, z_offset, roll, pitch, yaw) for Stewart IK
};

enum class Mmf1PoseCoordinateSystem : uint8_t {
    World = 0,
    Platform = 1,
};

namespace MachineConfigIO {

// Resolve the runtime machine.xml path next to the executable so MMF1 mode
// persistence does not depend on the current working directory.
std::string GetDefaultMachineConfigPath();

// Load the raw machine.xml text from the fixed runtime path.
// Returns true when the file exists and contains a valid MachineParameters root.
bool LoadMachineConfigXml(const std::string& xmlPath,
                          std::string& xmlContent,
                          std::string* errorMessage = nullptr);

// Load machine.xml with recovery. Missing/corrupt files trigger a rebuild
// from built-in defaults. When rebuild occurs, the file is rewritten,
// restartRequired is set to true, and the function returns false.
// On success (file exists and is valid), returns true with xmlContent populated.
bool LoadMachineConfigXmlWithDefaults(const std::string& xmlPath,
                                      std::string& xmlContent,
                                      bool& restartRequired,
                                      std::string* errorMessage = nullptr);

// Save a full machine.xml document to the fixed runtime path.
// Validates the XML before writing and creates parent directories when needed.
bool SaveMachineConfigXml(const std::string& xmlPath,
                          const std::string& xmlContent,
                          std::string* errorMessage = nullptr);

// Load Stewart machine parameters and MMF1 input mode from an XML file.
// Servo1..6 in machine.xml use the runtime Stewart actuator order directly;
// any alternate arm numbering remains a UI display concern only.
// Returns true on success; on failure, out/mode are left at their default values.
// pose6dLimits: if not nullptr, populated from <Limits><MaxPoseLimits> in the XML.
bool LoadMachineParams(const std::string& xmlPath,
                       WuSOEM::StewartParams& out,
                       Mmf1InputMode& mode,
                       Mmf1PoseCoordinateSystem* coordinateSystem = nullptr,
                       WuSOEM::Pose6DLimits* pose6dLimits = nullptr);

// Save only the MMF1 InputMode element in an existing machine.xml.
// Reads-modifies-writes to preserve all other content.
bool SaveMmf1InputMode(const std::string& xmlPath, Mmf1InputMode mode);

} // namespace MachineConfigIO
