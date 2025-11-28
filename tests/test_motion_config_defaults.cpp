#include "MotionConfigIO.h"
#include "core/CoreHelpers.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

using MotionCache = std::map<int, std::vector<std::vector<std::wstring>>>;

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

std::wstring GetParamValue(const MotionCache& cache, int slaveIndex, const wchar_t* paramIndex) {
    const auto slaveIt = cache.find(slaveIndex);
    if (slaveIt == cache.end()) return std::wstring();

    for (const auto& row : slaveIt->second) {
        if (!row.empty() && row[0] == paramIndex) {
            return row.size() > 3 ? row[3] : std::wstring();
        }
    }

    return std::wstring();
}

std::string MakeDefaultMotionXml() {
    return R"(<?xml version="1.0" encoding="utf-8"?>
<MotionCache>
  <Slave index="1">
    <Param><Index>P0</Index><Name>Device Name</Name><DataType>STRING</DataType><Value>Axis</Value><Unit>Unit</Unit><Desc>Device</Desc></Param>
    <Param><Index>P1</Index><Name>Scaling</Name><DataType>LREAL</DataType><Value>0.0000274658203125</Value><Unit>Degree/INC</Unit><Desc>Scale</Desc></Param>
    <Param><Index>P2</Index><Name>Direction</Name><DataType>BOOL</DataType><Value>0</Value><Unit>Bit</Unit><Desc>Direction</Desc></Param>
    <Param><Index>P3</Index><Name>Maximum Velocity</Name><DataType>LREAL</DataType><Value>180</Value><Unit>Unit/s</Unit><Desc>Max velocity</Desc></Param>
    <Param><Index>P4</Index><Name>Maximum Acceleration</Name><DataType>LREAL</DataType><Value>600</Value><Unit>Unit/s2</Unit><Desc>Max acceleration</Desc></Param>
    <Param><Index>P5</Index><Name>Maximum Deceleration</Name><DataType>LREAL</DataType><Value>600</Value><Unit>Unit/s2</Unit><Desc>Max deceleration</Desc></Param>
    <Param><Index>P6</Index><Name>Jerk</Name><DataType>LREAL</DataType><Value>2500</Value><Unit>Unit/s3</Unit><Desc>Jerk</Desc></Param>
    <Param><Index>P7</Index><Name>Manual Fast</Name><DataType>LREAL</DataType><Value>10</Value><Unit>Unit/s</Unit><Desc>Fast jog</Desc></Param>
    <Param><Index>P8</Index><Name>Manual Slow</Name><DataType>LREAL</DataType><Value>5</Value><Unit>Unit/s</Unit><Desc>Slow jog</Desc></Param>
    <Param><Index>P9</Index><Name>Minimum Position Limit</Name><DataType>LREAL</DataType><Value>-77</Value><Unit>Unit</Unit><Desc>Min limit</Desc></Param>
    <Param><Index>P10</Index><Name>Maximum Position Limit</Name><DataType>LREAL</DataType><Value>65</Value><Unit>Unit</Unit><Desc>Max limit</Desc></Param>
    <Param><Index>P11</Index><Name>Positive Torque Limit</Name><DataType>UINT</DataType><Value>1000</Value><Unit>0.0%</Unit><Desc>Positive torque</Desc></Param>
    <Param><Index>P12</Index><Name>Negative Torque Limit</Name><DataType>UINT</DataType><Value>700</Value><Unit>0.0%</Unit><Desc>Negative torque</Desc></Param>
    <Param><Index>P13</Index><Name>Home Offset</Name><DataType>LREAL</DataType><Value>-80</Value><Unit>Unit</Unit><Desc>Home offset</Desc></Param>
  </Slave>
</MotionCache>
)";
}

std::string MakeInvalidMotionXml() {
    return R"(<?xml version="1.0" encoding="utf-8"?>
<MotionCache>
  <Slave index="1">
    <Param><Index>P0</Index><Name>Device Name</Name><DataType>STRING</DataType><Value>Axis</Value><Unit>Unit</Unit><Desc>Device</Desc></Param>
    <Param><Index>P1</Index><Name>Scaling</Name><DataType>LREAL</DataType><Value>0.0000274658203125</Value><Unit>Degree/INC</Unit><Desc>Scale</Desc></Param>
    <Param><Index>P2</Index><Name>Direction</Name><DataType>BOOL</DataType><Value>9</Value><Unit>Bit</Unit><Desc>Direction</Desc></Param>
    <Param><Index>P3</Index><Name>Maximum Velocity</Name><DataType>LREAL</DataType><Value>-5</Value><Unit>Unit/s</Unit><Desc>Max velocity</Desc></Param>
    <Param><Index>P4</Index><Name>Maximum Acceleration</Name><DataType>LREAL</DataType><Value>600</Value><Unit>Unit/s2</Unit><Desc>Max acceleration</Desc></Param>
    <Param><Index>P5</Index><Name>Maximum Deceleration</Name><DataType>LREAL</DataType><Value>600</Value><Unit>Unit/s2</Unit><Desc>Max deceleration</Desc></Param>
    <Param><Index>P6</Index><Name>Jerk</Name><DataType>LREAL</DataType><Value>2500</Value><Unit>Unit/s3</Unit><Desc>Jerk</Desc></Param>
    <Param><Index>P7</Index><Name>Manual Fast</Name><DataType>LREAL</DataType><Value>10</Value><Unit>Unit/s</Unit><Desc>Fast jog</Desc></Param>
    <Param><Index>P8</Index><Name>Manual Slow</Name><DataType>LREAL</DataType><Value>5</Value><Unit>Unit/s</Unit><Desc>Slow jog</Desc></Param>
    <Param><Index>P9</Index><Name>Minimum Position Limit</Name><DataType>LREAL</DataType><Value>90</Value><Unit>Unit</Unit><Desc>Min limit</Desc></Param>
    <Param><Index>P10</Index><Name>Maximum Position Limit</Name><DataType>LREAL</DataType><Value>10</Value><Unit>Unit</Unit><Desc>Max limit</Desc></Param>
    <Param><Index>P11</Index><Name>Positive Torque Limit</Name><DataType>UINT</DataType><Value>1000</Value><Unit>0.0%</Unit><Desc>Positive torque</Desc></Param>
    <Param><Index>P12</Index><Name>Negative Torque Limit</Name><DataType>UINT</DataType><Value>700</Value><Unit>0.0%</Unit><Desc>Negative torque</Desc></Param>
    <Param><Index>P13</Index><Name>Home Offset</Name><DataType>LREAL</DataType><Value>-80</Value><Unit>Unit</Unit><Desc>Home offset</Desc></Param>
  </Slave>
</MotionCache>
)";
}

}  // namespace

namespace Win32WindowCore {

std::string EscapeJsonString(const std::string& s) {
    return CoreHelpers::EscapeJsonString(s);
}

}  // namespace Win32WindowCore

int main() {
    const std::filesystem::path tempRoot = std::filesystem::temp_directory_path() / "bigcat_motion_config_defaults";
    std::error_code ec;
    std::filesystem::remove_all(tempRoot, ec);
    std::filesystem::create_directories(tempRoot, ec);
    if (!Check(!ec, "failed to create temporary test directory")) {
        return EXIT_FAILURE;
    }

    const std::filesystem::path defaultTemplate = tempRoot / "motion.default.xml";
    const std::filesystem::path targetFile = tempRoot / "motion.xml";
    WriteUtf8File(defaultTemplate, MakeDefaultMotionXml());

    MotionCache cache;
    std::string loadedFilenameUtf8;
    MotionConfigIO::MotionCacheLoadDiagnostics diagnostics;

    if (!MotionConfigIO::LoadMotionCacheFromFileAtPathWithDefaults(cache, targetFile.wstring(), loadedFilenameUtf8, &diagnostics) ||
        !Check(diagnostics.usedDefaultTemplate, "missing motion.xml should fall back to motion.default.xml") ||
        !Check(diagnostics.rewroteTargetFile, "missing motion.xml should be recreated from defaults") ||
        !Check(GetParamValue(cache, 1, L"P3") == L"180", "default velocity should be restored for missing motion.xml") ||
        !Check(std::filesystem::exists(targetFile), "fallback load should recreate motion.xml")) {
        std::filesystem::remove_all(tempRoot, ec);
        return EXIT_FAILURE;
    }

    WriteUtf8File(targetFile, "<MotionCache>");
    cache.clear();
    diagnostics = MotionConfigIO::MotionCacheLoadDiagnostics{};
    if (!MotionConfigIO::LoadMotionCacheFromFileAtPathWithDefaults(cache, targetFile.wstring(), loadedFilenameUtf8, &diagnostics) ||
        !Check(diagnostics.usedDefaultTemplate, "corrupt motion.xml should fall back to motion.default.xml") ||
        !Check(GetParamValue(cache, 1, L"P3") == L"180", "corrupt motion.xml should restore default velocity")) {
        std::filesystem::remove_all(tempRoot, ec);
        return EXIT_FAILURE;
    }

    WriteUtf8File(targetFile, MakeInvalidMotionXml());
    cache.clear();
    diagnostics = MotionConfigIO::MotionCacheLoadDiagnostics{};
    if (!MotionConfigIO::LoadMotionCacheFromFileAtPathWithDefaults(cache, targetFile.wstring(), loadedFilenameUtf8, &diagnostics) ||
        !Check(!diagnostics.usedDefaultTemplate, "invalid values should repair the current file instead of replacing the whole file") ||
        !Check(diagnostics.repairedInvalidValues, "invalid values should be repaired from defaults") ||
        !Check(diagnostics.rewroteTargetFile, "repaired motion.xml should be written back to disk") ||
        !Check(GetParamValue(cache, 1, L"P2") == L"0", "invalid direction should be restored from default") ||
        !Check(GetParamValue(cache, 1, L"P3") == L"180", "invalid velocity should be restored from default") ||
        !Check(GetParamValue(cache, 1, L"P9") == L"-77", "invalid minimum position limit should be restored from default") ||
        !Check(GetParamValue(cache, 1, L"P10") == L"65", "invalid maximum position limit should be restored from default")) {
        std::filesystem::remove_all(tempRoot, ec);
        return EXIT_FAILURE;
    }

    std::filesystem::remove_all(tempRoot, ec);
    return EXIT_SUCCESS;
}