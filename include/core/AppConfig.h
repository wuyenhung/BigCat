#pragma once

#include <string>

namespace AppConfig {

struct PdoFilterDynamicViolationConfig {
    double velocityThresholdScale = 0.60;
    double accelerationThresholdScale = 1.20;
    double velocityViolationDelaySec = 0.50;
    double accelerationViolationDelaySec = 0.20;
};

std::wstring GetAppIniPath();
bool EnsureDefaultIniFile();

bool ReadBool(const wchar_t* section, const wchar_t* key, bool defaultValue);
double ReadDouble(const wchar_t* section, const wchar_t* key, double defaultValue);
std::wstring ReadServerPassword();
PdoFilterDynamicViolationConfig ReadPdoFilterDynamicViolationConfig();

} // namespace AppConfig