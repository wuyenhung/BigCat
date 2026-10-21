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
    <Param><Index>P0</Index><Name>Device Name</Name><DataType>STRING</DataType><Value>DS_P100E-7</Value><Unit>Unit</Unit><Desc>裝置名稱</Desc></Param>
    <Param><Index>P1</Index><Name>Scaling Factor numerator</Name><DataType>LREAL</DataType><Value>0.0000274658203125</Value><Unit>Degree/INC</Unit><Desc>編碼器分子比例</Desc></Param>
    <Param><Index>P2</Index><Name>Motor rotation direction</Name><DataType>BOOL</DataType><Value>0</Value><Unit>Bit</Unit><Desc>電機轉動方向</Desc></Param>
    <Param><Index>P3</Index><Name>Maximum Velocity</Name><DataType>LREAL</DataType><Value>180</Value><Unit>Unit/s</Unit><Desc>最大速度</Desc></Param>
    <Param><Index>P4</Index><Name>Maximum Acceleration</Name><DataType>LREAL</DataType><Value>900</Value><Unit>Unit/s2</Unit><Desc>最大加速度</Desc></Param>
    <Param><Index>P5</Index><Name>Maximum Deceleration</Name><DataType>LREAL</DataType><Value>900</Value><Unit>Unit/s2</Unit><Desc>最大減速度</Desc></Param>
    <Param><Index>P6</Index><Name>Jerk</Name><DataType>LREAL</DataType><Value>1800</Value><Unit>Unit/s3</Unit><Desc>加加速度</Desc></Param>
    <Param><Index>P7</Index><Name>Manual Velocity(Fast)</Name><DataType>LREAL</DataType><Value>10</Value><Unit>Unit/s</Unit><Desc>手動快速</Desc></Param>
    <Param><Index>P8</Index><Name>Manual Velocity(Slow)</Name><DataType>LREAL</DataType><Value>5</Value><Unit>Unit/s</Unit><Desc>手動慢速</Desc></Param>
    <Param><Index>P9</Index><Name>Minimum Position Limit</Name><DataType>LREAL</DataType><Value>-65</Value><Unit>Unit</Unit><Desc>最小位置</Desc></Param>
    <Param><Index>P10</Index><Name>Maximum Position Limit</Name><DataType>LREAL</DataType><Value>65</Value><Unit>Unit</Unit><Desc>最大位置</Desc></Param>
    <Param><Index>P11</Index><Name>Positive Torque Limit</Name><DataType>UINT</DataType><Value>1000</Value><Unit>0.0%</Unit><Desc>正扭力限制</Desc></Param>
    <Param><Index>P12</Index><Name>Negative Torque Limit</Name><DataType>UINT</DataType><Value>700</Value><Unit>0.0%</Unit><Desc>負扭力限制</Desc></Param>
    <Param><Index>P13</Index><Name>Home Offset</Name><DataType>LREAL</DataType><Value>-68</Value><Unit>Unit</Unit><Desc>原點寫入值</Desc></Param>
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

    const std::filesystem::path targetFile = tempRoot / "motion.xml";

    // --- Test 1: motion.xml missing → rebuild from built-in defaults, restart required ---
    {
        MotionCache cache;
        std::string loadedFilenameUtf8;
        MotionConfigIO::MotionCacheLoadDiagnostics diagnostics;

        bool ok = MotionConfigIO::LoadMotionCacheFromFileAtPathWithDefaults(cache, targetFile.wstring(), loadedFilenameUtf8, &diagnostics);
        if (!Check(!ok, "missing motion.xml should return false (restart required)") ||
            !Check(diagnostics.usedDefaultTemplate, "missing motion.xml should use built-in defaults") ||
            !Check(diagnostics.restartRequired, "missing motion.xml should require restart") ||
            !Check(diagnostics.rewroteTargetFile, "missing motion.xml should be recreated from built-in defaults") ||
            !Check(std::filesystem::exists(targetFile), "fallback should recreate motion.xml")) {
            std::filesystem::remove_all(tempRoot, ec);
            return EXIT_FAILURE;
        }
    }

    // --- Test 2: corrupt motion.xml → rebuild from built-in defaults, restart required ---
    {
        WriteUtf8File(targetFile, "<MotionCache>");
        MotionCache cache;
        std::string loadedFilenameUtf8;
        MotionConfigIO::MotionCacheLoadDiagnostics diagnostics;

        bool ok = MotionConfigIO::LoadMotionCacheFromFileAtPathWithDefaults(cache, targetFile.wstring(), loadedFilenameUtf8, &diagnostics);
        if (!Check(!ok, "corrupt motion.xml should return false (restart required)") ||
            !Check(diagnostics.usedDefaultTemplate, "corrupt motion.xml should use built-in defaults") ||
            !Check(diagnostics.restartRequired, "corrupt motion.xml should require restart")) {
            std::filesystem::remove_all(tempRoot, ec);
            return EXIT_FAILURE;
        }
    }

    // --- Test 3: valid motion.xml with unreasonable values → repair + restart required ---
    {
        WriteUtf8File(targetFile, MakeInvalidMotionXml());
        MotionCache cache;
        std::string loadedFilenameUtf8;
        MotionConfigIO::MotionCacheLoadDiagnostics diagnostics;

        bool ok = MotionConfigIO::LoadMotionCacheFromFileAtPathWithDefaults(cache, targetFile.wstring(), loadedFilenameUtf8, &diagnostics);
        if (!Check(!ok, "invalid values should return false (restart required)") ||
            !Check(!diagnostics.usedDefaultTemplate, "invalid values should repair, not replace whole file") ||
            !Check(diagnostics.repairedInvalidValues, "invalid values should be repaired from built-in defaults") ||
            !Check(diagnostics.restartRequired, "invalid values should require restart") ||
            !Check(diagnostics.rewroteTargetFile, "repaired motion.xml should be written back to disk")) {
            std::filesystem::remove_all(tempRoot, ec);
            return EXIT_FAILURE;
        }
    }

    // --- Test 4: valid motion.xml loads normally (no restart) ---
    {
        WriteUtf8File(targetFile, MakeDefaultMotionXml());
        MotionCache cache;
        std::string loadedFilenameUtf8;
        MotionConfigIO::MotionCacheLoadDiagnostics diagnostics;

        bool ok = MotionConfigIO::LoadMotionCacheFromFileAtPathWithDefaults(cache, targetFile.wstring(), loadedFilenameUtf8, &diagnostics);
        if (!Check(ok, "valid motion.xml should return true") ||
            !Check(!diagnostics.usedDefaultTemplate, "valid motion.xml should not use defaults") ||
            !Check(!diagnostics.restartRequired, "valid motion.xml should not require restart") ||
            !Check(GetParamValue(cache, 1, L"P3") == L"180", "valid velocity should be loaded") ||
            !Check(GetParamValue(cache, 1, L"P9") == L"-65", "valid min limit should be loaded")) {
            std::filesystem::remove_all(tempRoot, ec);
            return EXIT_FAILURE;
        }
    }

    std::filesystem::remove_all(tempRoot, ec);
    return EXIT_SUCCESS;
}