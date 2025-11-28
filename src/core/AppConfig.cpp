#include "core/AppConfig.h"

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
constexpr wchar_t kSectionPdoFilter[] = L"PDOFilter";

constexpr wchar_t kKeyEnableStatusLog[] = L"EnableStatusLog";
constexpr wchar_t kKeyAutoSelectedAdapter[] = L"AutoSelectedAdapter";
constexpr wchar_t kKeyShowMainWindow[] = L"ShowMainWindow";
constexpr wchar_t kKeyAutoTrigger[] = L"AutoTrigger";
constexpr wchar_t kKeyEnableLocalHttpServer[] = L"EnableLocalHttpServer";
constexpr wchar_t kKeyServerPassword[] = L"Password";
constexpr wchar_t kKeyVelocityThresholdScale[] = L"DynamicVelocityThresholdScale";
constexpr wchar_t kKeyAccelerationThresholdScale[] = L"DynamicAccelerationThresholdScale";
constexpr wchar_t kKeyVelocityViolationDelaySec[] = L"DynamicVelocityViolationDelaySec";
constexpr wchar_t kKeyAccelerationViolationDelaySec[] = L"DynamicAccelerationViolationDelaySec";

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
    ok = WriteDefaultIfMissing(iniPath, kSectionPdoFilter, kKeyVelocityThresholdScale, L"0.60") && ok;
    ok = WriteDefaultIfMissing(iniPath, kSectionPdoFilter, kKeyAccelerationThresholdScale, L"1.20") && ok;
    ok = WriteDefaultIfMissing(iniPath, kSectionPdoFilter, kKeyVelocityViolationDelaySec, L"0.50") && ok;
    ok = WriteDefaultIfMissing(iniPath, kSectionPdoFilter, kKeyAccelerationViolationDelaySec, L"0.20") && ok;
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

} // namespace AppConfig