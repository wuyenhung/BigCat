#include "core/AppConfig.h"
#include "core/Cia402Mode.h"

#include <windows.h>

#include <cmath>
#include <cwchar>
#include <cwctype>

namespace AppConfig {
namespace {

constexpr wchar_t kMissingValue[] = L"\x1f__BIGCAT_INI_MISSING__";

constexpr wchar_t kSectionLogging[] = L"Logging";
constexpr wchar_t kSectionNetwork[] = L"Network";
constexpr wchar_t kSectionStartup[] = L"Startup";
constexpr wchar_t kSectionServer[] = L"Server";
constexpr wchar_t kSectionMotion[] = L"Motion";
constexpr wchar_t kSectionPdoFilter[] = L"PDOFilter";
constexpr wchar_t kSectionSafety[] = L"Safety";
constexpr wchar_t kSectionCompat[] = L"Compat";

constexpr wchar_t kKeyEnableStatusLog[] = L"EnableStatusLog";
constexpr wchar_t kKeyAutoSelectedAdapter[] = L"AutoSelectedAdapter";
constexpr wchar_t kKeyShowMainWindow[] = L"ShowMainWindow";
constexpr wchar_t kKeyAutoTrigger[] = L"AutoTrigger";
constexpr wchar_t kKeyEnableLocalHttpServer[] = L"EnableLocalHttpServer";
constexpr wchar_t kKeyServerPassword[] = L"Password";
constexpr wchar_t kKeyOperationMode[] = L"OperationMode";
constexpr wchar_t kKeyExternalEstopEnabled[] = L"ExternalEstopEnabled";
constexpr wchar_t kKeyReverseJogSpeedThreshold[] = L"ReverseJogSpeedThreshold";
constexpr wchar_t kKeyReverseJogTorqueThreshold[] = L"ReverseJogTorqueThreshold";
constexpr wchar_t kKeyVelocityThresholdScale[] = L"DynamicVelocityThresholdScale";
constexpr wchar_t kKeyAccelerationThresholdScale[] = L"DynamicAccelerationThresholdScale";
constexpr wchar_t kKeyVelocityViolationDelaySec[] = L"DynamicVelocityViolationDelaySec";
constexpr wchar_t kKeyAccelerationViolationDelaySec[] = L"DynamicAccelerationViolationDelaySec";
constexpr wchar_t kKeyBlockSystemShutdown[] = L"BlockSystemShutdown";
constexpr wchar_t kKeyKeepSystemAwake[] = L"KeepSystemAwake";
constexpr wchar_t kKeyBlockWindowsUpdateAutoReboot[] = L"BlockWindowsUpdateAutoReboot";
constexpr wchar_t kKeyForceWin10Mode[] = L"ForceWin10Mode";
constexpr wchar_t kSectionMmf[] = L"MMF";
constexpr wchar_t kKeyMmf1InputName[] = L"InputName";
constexpr wchar_t kBuiltinMmfInputPose6D[] = L"OUTPUT_MMF_6DOF";
constexpr wchar_t kBuiltinMmfInputDeg[] = L"OUTPUT_MMF_DEG";

bool IsMissingKey(const std::wstring& iniPath, const wchar_t* section, const wchar_t* key) {
    wchar_t buffer[64] = {0};
    GetPrivateProfileStringW(section, key, kMissingValue, buffer, static_cast<DWORD>(std::size(buffer)), iniPath.c_str());
    return std::wcscmp(buffer, kMissingValue) == 0;
}

bool WriteDefaultIfMissing(const std::wstring& iniPath, const wchar_t* section, const wchar_t* key, const wchar_t* value) {
    if (!IsMissingKey(iniPath, section, key)) {
        return true;
    }
    return WritePrivateProfileStringW(section, key, value, iniPath.c_str()) != FALSE;
}

double SanitizeNonNegative(double value, double fallback) {
    return (std::isfinite(value) && value >= 0.0) ? value : fallback;
}

double ParseDoubleOrDefault(const std::wstring& text, double defaultValue) {
    wchar_t* end = nullptr;
    const double parsed = std::wcstod(text.c_str(), &end);
    if (end == text.c_str()) {
        return defaultValue;
    }
    while (end && *end != L'\0' && std::iswspace(static_cast<unsigned short>(*end))) {
        ++end;
    }
    if (!end || *end != L'\0') {
        return defaultValue;
    }
    return parsed;
}

std::wstring ReadString(const wchar_t* section, const wchar_t* key, const wchar_t* defaultValue) {
    const std::wstring iniPath = GetAppIniPath();
    EnsureDefaultIniFile();
    wchar_t buffer[512] = {0};
    GetPrivateProfileStringW(section, key, defaultValue, buffer, static_cast<DWORD>(std::size(buffer)), iniPath.c_str());
    return std::wstring(buffer);
}

} // namespace

std::wstring GetAppIniPath() {
    wchar_t path[MAX_PATH] = {0};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return L".\\BigCat.ini";
    }

    wchar_t* extension = std::wcsrchr(path, L'.');
    wchar_t* slash = std::wcsrchr(path, L'\\');
    if (extension && (!slash || extension > slash)) {
        *extension = L'\0';
    }
    return std::wstring(path) + L".ini";
}

bool EnsureDefaultIniFile() {
    const std::wstring iniPath = GetAppIniPath();
    bool ok = true;
    ok = WriteDefaultIfMissing(iniPath, kSectionLogging, kKeyEnableStatusLog, L"1") && ok;
    ok = WriteDefaultIfMissing(iniPath, kSectionNetwork, kKeyAutoSelectedAdapter, L"") && ok;
    ok = WriteDefaultIfMissing(iniPath, kSectionStartup, kKeyShowMainWindow, L"0") && ok;
    ok = WriteDefaultIfMissing(iniPath, kSectionStartup, kKeyAutoTrigger, L"1") && ok;
    ok = WriteDefaultIfMissing(iniPath, kSectionServer, kKeyEnableLocalHttpServer, L"0") && ok;
    ok = WriteDefaultIfMissing(iniPath, kSectionServer, kKeyServerPassword, L"1234") && ok;
    ok = WriteDefaultIfMissing(iniPath, kSectionMotion, kKeyOperationMode, L"CSP") && ok;
    ok = WriteDefaultIfMissing(iniPath, kSectionMotion, kKeyExternalEstopEnabled, L"1") && ok;
    ok = WriteDefaultIfMissing(iniPath, kSectionMotion, kKeyReverseJogSpeedThreshold, L"-2.5") && ok;
    ok = WriteDefaultIfMissing(iniPath, kSectionMotion, kKeyReverseJogTorqueThreshold, L"400") && ok;
    ok = WriteDefaultIfMissing(iniPath, kSectionPdoFilter, kKeyVelocityThresholdScale, L"0.60") && ok;
    ok = WriteDefaultIfMissing(iniPath, kSectionPdoFilter, kKeyAccelerationThresholdScale, L"1.20") && ok;
    ok = WriteDefaultIfMissing(iniPath, kSectionPdoFilter, kKeyVelocityViolationDelaySec, L"0.50") && ok;
    ok = WriteDefaultIfMissing(iniPath, kSectionPdoFilter, kKeyAccelerationViolationDelaySec, L"0.20") && ok;
    ok = WriteDefaultIfMissing(iniPath, kSectionMmf, kKeyMmf1InputName, kBuiltinMmfInputPose6D) && ok;
    ok = WriteDefaultIfMissing(iniPath, kSectionSafety, kKeyBlockSystemShutdown, L"1") && ok;
    ok = WriteDefaultIfMissing(iniPath, kSectionSafety, kKeyKeepSystemAwake, L"1") && ok;
    ok = WriteDefaultIfMissing(iniPath, kSectionSafety, kKeyBlockWindowsUpdateAutoReboot, L"0") && ok;
    ok = WriteDefaultIfMissing(iniPath, kSectionCompat, kKeyForceWin10Mode, L"0") && ok;
    return ok;
}

bool ReadBool(const wchar_t* section, const wchar_t* key, bool defaultValue) {
    EnsureDefaultIniFile();
    const std::wstring iniPath = GetAppIniPath();
    const int value = GetPrivateProfileIntW(section, key, defaultValue ? 1 : 0, iniPath.c_str());
    return value != 0;
}

double ReadDouble(const wchar_t* section, const wchar_t* key, double defaultValue) {
    const std::wstring rawValue = ReadString(section, key, L"");
    return ParseDoubleOrDefault(rawValue, defaultValue);
}

std::wstring ReadServerPassword() {
    return ReadString(kSectionServer, kKeyServerPassword, L"1234");
}

std::wstring ReadMmf1InputName(const std::wstring& fallback) {
    const std::wstring value = ReadString(kSectionMmf, kKeyMmf1InputName, L"");
    // A built-in name stored in the file is treated as "not overridden" so the
    // mode-dependent default (OUTPUT_MMF_6DOF / OUTPUT_MMF_DEG) still applies.
    if (value.empty() || value == kBuiltinMmfInputPose6D || value == kBuiltinMmfInputDeg) {
        return fallback;
    }
    return value;
}

PdoFilterDynamicViolationConfig ReadPdoFilterDynamicViolationConfig() {
    EnsureDefaultIniFile();
    PdoFilterDynamicViolationConfig config;
    config.velocityThresholdScale = SanitizeNonNegative(
        ReadDouble(kSectionPdoFilter, kKeyVelocityThresholdScale, config.velocityThresholdScale),
        config.velocityThresholdScale);
    config.accelerationThresholdScale = SanitizeNonNegative(
        ReadDouble(kSectionPdoFilter, kKeyAccelerationThresholdScale, config.accelerationThresholdScale),
        config.accelerationThresholdScale);
    config.velocityViolationDelaySec = SanitizeNonNegative(
        ReadDouble(kSectionPdoFilter, kKeyVelocityViolationDelaySec, config.velocityViolationDelaySec),
        config.velocityViolationDelaySec);
    config.accelerationViolationDelaySec = SanitizeNonNegative(
        ReadDouble(kSectionPdoFilter, kKeyAccelerationViolationDelaySec, config.accelerationViolationDelaySec),
        config.accelerationViolationDelaySec);
    return config;
}

int8_t ReadCyclicOperationMode() {
    const std::wstring iniPath = GetAppIniPath();
    EnsureDefaultIniFile();
    wchar_t buffer[16] = {0};
    GetPrivateProfileStringW(kSectionMotion, kKeyOperationMode, L"CSP", buffer,
                             static_cast<DWORD>(std::size(buffer)), iniPath.c_str());
    std::wstring modeStr(buffer);
    // Case-insensitive comparison
    for (auto& ch : modeStr) {
        ch = static_cast<wchar_t>(std::towupper(static_cast<unsigned short>(ch)));
    }
    if (modeStr == L"CSV" || modeStr == L"9") {
        return CoreHelpers::kCia402OperationModeCyclicSyncVelocity;
    }
    // Default to CSP (including "CSP", "8", or any unrecognized value)
    return CoreHelpers::kCia402OperationModeCyclicSyncPosition;
}

bool ReadExternalEstopEnabled() {
    return ReadBool(kSectionMotion, kKeyExternalEstopEnabled, true);
}
double ReadReverseJogSpeedThreshold() {
    return ReadDouble(kSectionMotion, kKeyReverseJogSpeedThreshold, -2.5);
}

double ReadReverseJogTorqueThreshold() {
    return ReadDouble(kSectionMotion, kKeyReverseJogTorqueThreshold, 400.0);
}

bool ReadBlockSystemShutdown() {
    return ReadBool(kSectionSafety, kKeyBlockSystemShutdown, true);
}

bool ReadKeepSystemAwake() {
    return ReadBool(kSectionSafety, kKeyKeepSystemAwake, true);
}

bool ReadBlockWindowsUpdateAutoReboot() {
    return ReadBool(kSectionSafety, kKeyBlockWindowsUpdateAutoReboot, false);
}

bool ReadForceWin10Compat() {
    return ReadBool(kSectionCompat, kKeyForceWin10Mode, false);
}

} // namespace AppConfig