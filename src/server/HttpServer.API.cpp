// HttpServer.API.cpp - REST API 路由與 UI 狀態同步。

#include <winsock2.h>

#include <array>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "HttpServer.h"
#include "EtherCATMaster.h"
#include "MainWindow.h"
#include "StewartIK.h"
#include "../app/AppController.h"
#include "core/CoreHelpers.h"
#include "HttpServer.Internal.h"

using namespace HttpServerInternal;

namespace {

using WuSOEM::ForwardResult;
using WuSOEM::IKResult;
using WuSOEM::Pose6D;
using WuSOEM::StewartIK;
using WuSOEM::StewartParams;

constexpr int kJsonFloatPrecision = std::numeric_limits<double>::max_digits10;
constexpr uint16_t kErrorCode603FReadFailureSentinel = 0xFFFF;

bool TryParseJsonString(const std::string& body, const std::string& key, std::string& value);

const char* GetMmf1InputModeJsonValue(Mmf1InputMode mode) {
    return mode == Mmf1InputMode::Pose6D ? "Pose6D" : "ServoAngle";
}

const char* GetActiveMmf1InputModeJsonValue() {
    return GetMmf1InputModeJsonValue(AppController::GetInstance().GetMmf1InputMode());
}

bool TryParseMmf1InputModeValue(const std::string& rawValue, Mmf1InputMode& mode) {
    std::string normalizedValue;
    normalizedValue.reserve(rawValue.size());
    for (char ch : rawValue) {
        normalizedValue.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }

    if (normalizedValue == "pose6d") {
        mode = Mmf1InputMode::Pose6D;
        return true;
    }

    if (normalizedValue == "servoangle") {
        mode = Mmf1InputMode::ServoAngle;
        return true;
    }

    return false;
}

bool TryParseMmf1InputModeFromBody(const std::string& body, Mmf1InputMode& mode) {
    std::string value;
    if (!TryParseJsonString(body, "mode", value)) {
        return false;
    }

    return TryParseMmf1InputModeValue(value, mode);
}

std::string BuildMmf1InputModeConfigJson(
    Mmf1InputMode activeMode,
    Mmf1InputMode savedMode,
    bool configLoaded) {
    std::ostringstream oss;
    oss << "{\"activeMode\":\"" << GetMmf1InputModeJsonValue(activeMode)
        << "\",\"savedMode\":\"" << GetMmf1InputModeJsonValue(savedMode)
        << "\",\"configLoaded\":" << (configLoaded ? "true" : "false")
        << ",\"restartRequired\":" << ((activeMode != savedMode) ? "true" : "false")
        << "}";
    return oss.str();
}

std::string BuildErrorJson(const std::string& message) {
    return std::string("{\"error\":\"") + CoreHelpers::EscapeJsonString(message) + "\"}";
}

bool IsMachineConfigValidationError(const std::string& errorMessage) {
    return errorMessage.rfind("invalid machine.xml:", 0) == 0;
}

MainWindow* GetHttpTargetMainWindow() {
    if (!g_hwndHttpTarget || !IsWindow(g_hwndHttpTarget)) {
        return nullptr;
    }
    return reinterpret_cast<MainWindow*>(GetWindowLongPtr(g_hwndHttpTarget, GWLP_USERDATA));
}

CoreHelpers::SystemStateSnapshot GetHttpTargetSystemStateSnapshot() {
    if (auto* win = GetHttpTargetMainWindow()) {
        return win->GetSystemStateSnapshot();
    }
    return {};
}

CoreHelpers::AutoActionButtonInputs GetHttpAutoActionButtonInputs() {
    CoreHelpers::AutoActionButtonInputs inputs;
    if (auto* win = GetHttpTargetMainWindow()) {
        inputs = win->GetAutoActionButtonInputsForWeb();
    }
    return inputs;
}

bool IsAutoActionUiStateKey(const std::string& key) {
    return key == "auto_connect" ||
           key == "fault_reset" ||
           key == "down_to_origin" ||
           key == "immediate_stop" ||
           key == "emergency_stop" ||
           key == "auto_connect_lamp" ||
           key == "down_to_origin_lamp" ||
           key == "fault_reset_lamp" ||
           key == "immediate_stop_lamp" ||
           key == "emergency_stop_lamp";
}

void AppendAutoActionButtonStateJson(std::ostringstream& oss,
                                     const CoreHelpers::AutoActionButtonState& state) {
    oss << "\"auto_connect\":" << (!state.connect_enabled ? "true" : "false");
    oss << ",\"fault_reset\":" << (!state.fault_reset_enabled ? "true" : "false");
    oss << ",\"down_to_origin\":" << (!state.down_to_origin_enabled ? "true" : "false");
    oss << ",\"immediate_stop\":" << (!state.immediate_stop_enabled ? "true" : "false");
    oss << ",\"emergency_stop\":" << (!state.emergency_stop_enabled ? "true" : "false");
    oss << ",\"auto_connect_lamp\":" << (state.connect_lit ? "true" : "false");
    oss << ",\"down_to_origin_lamp\":" << (state.down_to_origin_lit ? "true" : "false");
    oss << ",\"fault_reset_lamp\":" << (state.fault_reset_lit ? "true" : "false");
    oss << ",\"immediate_stop_lamp\":" << (state.immediate_stop_lit ? "true" : "false");
    oss << ",\"emergency_stop_lamp\":" << (state.emergency_stop_lit ? "true" : "false");
}

struct SelectedAdapterUiState {
    std::string value;
    std::string display;
};

std::string BuildAdapterDisplayLabel(const AdapterEntry& adapter) {
    std::string display = CoreHelpers::MakeAdapterDisplayString(adapter);
    if (!adapter.adapterName.empty()) {
        std::string shortId = adapter.adapterName.size() > 8
            ? adapter.adapterName.substr(adapter.adapterName.size() - 8)
            : adapter.adapterName;
        display += " [" + shortId + "]";
    }
    return display;
}

SelectedAdapterUiState GetSelectedAdapterUiState() {
    SelectedAdapterUiState state;
    if (auto* win = GetHttpTargetMainWindow()) {
        state.value = win->GetWebSelectedAdapterUtf8();
        if (state.value.empty()) {
            state.value = win->GetSelectedAdapterUtf8();
        }
        if (!state.value.empty()) {
            if (EtherCATMaster::IsSimulationAdapterSelection(state.value)) {
                state.value = EtherCATMaster::GetSimulationInterfaceName();
                state.display = EtherCATMaster::GetSimulationAdapterDisplayName();
                return state;
            }

            std::string enumerateError;
            std::vector<AdapterEntry> adapters = CoreHelpers::EnumerateAdapters(enumerateError);
            if (enumerateError.empty()) {
                std::string normalizedValue = state.value;
                if (normalizedValue.size() > 1 &&
                    normalizedValue.front() == '{' &&
                    normalizedValue.back() == '}') {
                    normalizedValue = normalizedValue.substr(1, normalizedValue.size() - 2);
                }

                auto matchesSelection = [&](const AdapterEntry& adapter) {
                    if (adapter.pcapName == state.value) {
                        return true;
                    }
                    if (!adapter.adapterName.empty() &&
                        (adapter.adapterName == state.value || adapter.adapterName == normalizedValue)) {
                        return true;
                    }
                    if (!adapter.friendlyName.empty() && adapter.friendlyName == state.value) {
                        return true;
                    }
                    return BuildAdapterDisplayLabel(adapter) == state.value ||
                           CoreHelpers::MakeAdapterDisplayString(adapter) == state.value;
                };

                for (const auto& adapter : adapters) {
                    if (matchesSelection(adapter)) {
                        state.value = adapter.pcapName;
                        state.display = BuildAdapterDisplayLabel(adapter);
                        break;
                    }
                }
            }

            if (state.display.empty()) {
                state.display = state.value;
            }
        }
    }
    return state;
}

bool TryFindJsonValueStart(const std::string& body, const std::string& key, size_t& valuePos) {
    const std::string token = "\"" + key + "\"";
    size_t keyPos = body.find(token);
    if (keyPos == std::string::npos) {
        return false;
    }

    size_t colonPos = body.find(':', keyPos + token.size());
    if (colonPos == std::string::npos) {
        return false;
    }

    valuePos = colonPos + 1;
    while (valuePos < body.size() &&
           std::isspace(static_cast<unsigned char>(body[valuePos])) != 0) {
        ++valuePos;
    }

    return valuePos < body.size();
}

bool TryParseJsonLongDouble(const std::string& body, const std::string& key, long double& value) {
    size_t valuePos = 0;
    if (!TryFindJsonValueStart(body, key, valuePos)) {
        return false;
    }

    const char* begin = body.c_str() + valuePos;
    char* end = nullptr;
    value = std::strtold(begin, &end);
    return end != begin;
}

bool TryParseJsonBool(const std::string& body, const std::string& key, bool& value) {
    size_t valuePos = 0;
    if (!TryFindJsonValueStart(body, key, valuePos)) {
        return false;
    }

    if (body.compare(valuePos, 4, "true") == 0) {
        value = true;
        return true;
    }
    if (body.compare(valuePos, 5, "false") == 0) {
        value = false;
        return true;
    }
    return false;
}

bool TryParseJsonString(const std::string& body, const std::string& key, std::string& value) {
    size_t valuePos = 0;
    if (!TryFindJsonValueStart(body, key, valuePos) || body[valuePos] != '"') {
        return false;
    }

    ++valuePos;
    size_t endPos = body.find('"', valuePos);
    if (endPos == std::string::npos) {
        return false;
    }

    value = body.substr(valuePos, endPos - valuePos);
    return true;
}

bool TryParseJsonIntArray6(
    const std::string& body,
    const std::string& key,
    std::array<int, 6>& values) {
    size_t valuePos = 0;
    if (!TryFindJsonValueStart(body, key, valuePos) || body[valuePos] != '[') {
        return false;
    }

    ++valuePos;
    for (size_t index = 0; index < values.size(); ++index) {
        while (valuePos < body.size() &&
               std::isspace(static_cast<unsigned char>(body[valuePos])) != 0) {
            ++valuePos;
        }

        const char* begin = body.c_str() + valuePos;
        char* end = nullptr;
        long val = std::strtol(begin, &end, 10);
        if (end == begin) {
            return false;
        }

        values[index] = static_cast<int>(val);
        valuePos = static_cast<size_t>(end - body.c_str());
        while (valuePos < body.size() &&
               std::isspace(static_cast<unsigned char>(body[valuePos])) != 0) {
            ++valuePos;
        }

        if (index + 1 < values.size()) {
            if (valuePos >= body.size() || body[valuePos] != ',') {
                return false;
            }
            ++valuePos;
        }
    }

    while (valuePos < body.size() &&
           std::isspace(static_cast<unsigned char>(body[valuePos])) != 0) {
        ++valuePos;
    }

    return valuePos < body.size() && body[valuePos] == ']';
}

bool TryParseJsonLongDoubleArray6(
    const std::string& body,
    const std::string& key,
    std::array<long double, 6>& values) {
    size_t valuePos = 0;
    if (!TryFindJsonValueStart(body, key, valuePos) || body[valuePos] != '[') {
        return false;
    }

    ++valuePos;
    for (size_t index = 0; index < values.size(); ++index) {
        while (valuePos < body.size() &&
               std::isspace(static_cast<unsigned char>(body[valuePos])) != 0) {
            ++valuePos;
        }

        const char* begin = body.c_str() + valuePos;
        char* end = nullptr;
        values[index] = std::strtold(begin, &end);
        if (end == begin) {
            return false;
        }

        valuePos = static_cast<size_t>(end - body.c_str());
        while (valuePos < body.size() &&
               std::isspace(static_cast<unsigned char>(body[valuePos])) != 0) {
            ++valuePos;
        }

        if (index + 1 < values.size()) {
            if (valuePos >= body.size() || body[valuePos] != ',') {
                return false;
            }
            ++valuePos;
        }
    }

    while (valuePos < body.size() &&
           std::isspace(static_cast<unsigned char>(body[valuePos])) != 0) {
        ++valuePos;
    }

    return valuePos < body.size() && body[valuePos] == ']';
}

bool TryParseStewartParamsFromBody(const std::string& body, StewartParams& params) {
    bool ok = TryParseJsonLongDouble(body, "platformEdgeA", params.platformEdgeA) &&
           TryParseJsonLongDouble(body, "platformEdgeB", params.platformEdgeB) &&
           TryParseJsonLongDouble(body, "baseEdgeA", params.baseEdgeA) &&
           TryParseJsonLongDouble(body, "baseEdgeB", params.baseEdgeB) &&
           TryParseJsonLongDouble(body, "rodLength", params.rodLength) &&
           TryParseJsonLongDouble(body, "servoArmLength", params.servoArmLength) &&
           TryParseJsonLongDouble(body, "servoAngleLimit", params.servoAngleLimit) &&
           TryParseJsonLongDouble(body, "fisheyeAngleLimit", params.fisheyeAngleLimit) &&
           TryParseJsonLongDouble(body, "baseHeight", params.baseHeight) &&
           TryParseJsonLongDoubleArray6(body, "servoAxisAngle", params.servoAxisAngle);
    if (!ok) {
        return false;
    }
    // servoPointMap is optional; keep default if absent
    TryParseJsonIntArray6(body, "servoPointMap", params.servoPointMap);
    return true;
}

enum class StewartInputCoordinateSystem {
    kLegacyWorldPose,
    kWorld,
    kPlatform,
};

bool TryParseStewartWebSettingsFromBody(const std::string& body, StewartWebSettings& settings) {
    long double value = 0.0L;
    if (TryParseJsonLongDouble(body, "servoInitialAngle", value)) {
        settings.servoInitialAngle = value;
    }
    if (TryParseJsonLongDouble(body, "singularityVerticalAngle", value)) {
        settings.singularityVerticalAngle = value;
    }
    if (TryParseJsonLongDouble(body, "singularityHorizontalAngle", value)) {
        settings.singularityHorizontalAngle = value;
    }
    if (TryParseJsonLongDouble(body, "singularityExtendRatio", value)) {
        settings.singularityExtendRatio = value;
    }
    if (TryParseJsonLongDouble(body, "singularityContractRatio", value)) {
        settings.singularityContractRatio = value;
    }
    return true;
}

StewartInputCoordinateSystem ParseStewartInputCoordinateSystem(const std::string& body) {
    std::string value;
    if (!TryParseJsonString(body, "inputCoordinateSystem", value)) {
        return StewartInputCoordinateSystem::kLegacyWorldPose;
    }

    if (value == "platform") {
        return StewartInputCoordinateSystem::kPlatform;
    }
    if (value == "world") {
        return StewartInputCoordinateSystem::kWorld;
    }
    return StewartInputCoordinateSystem::kLegacyWorldPose;
}

Pose6D BuildRelativeWorldPose(const Pose6D& worldPose, long double initialHeight) {
    Pose6D relativePose = worldPose;
    relativePose.z -= initialHeight;
    return relativePose;
}

Pose6D BuildPlatformPoseFromWorldPoseImpl(const Pose6D& worldPose, long double initialHeight) {
    const std::array<long double, 3> worldDelta{{
        worldPose.x,
        worldPose.y,
        worldPose.z - initialHeight,
    }};
    const auto platformDelta = StewartIK::TransformWorldToPlatformDelta(worldDelta, worldPose);

    Pose6D platformPose = worldPose;
    platformPose.x = platformDelta[0];
    platformPose.y = platformDelta[1];
    platformPose.z = platformDelta[2];
    return platformPose;
}

StewartResolvedPoseInput ResolveStewartPoseInput(
    const StewartParams& params,
    const StewartWebSettings& settings,
    StewartInputCoordinateSystem coordinateSystem,
    const Pose6D& inputPose) {
    StewartResolvedPoseInput resolved;
    resolved.initialHeight = StewartIK::CalculateInitialHeight(params, settings.servoInitialAngle);

    if (coordinateSystem == StewartInputCoordinateSystem::kLegacyWorldPose) {
        resolved.worldPose = inputPose;
        resolved.platformPose = BuildPlatformPoseFromWorldPoseImpl(inputPose, resolved.initialHeight);
        return resolved;
    }

    resolved.worldPose.roll = StewartIK::NormalizeAngleDeg(inputPose.roll);
    resolved.worldPose.pitch = StewartIK::NormalizeAngleDeg(inputPose.pitch);
    resolved.worldPose.yaw = StewartIK::NormalizeAngleDeg(inputPose.yaw);

    if (coordinateSystem == StewartInputCoordinateSystem::kWorld) {
        resolved.worldPose.x = inputPose.x;
        resolved.worldPose.y = inputPose.y;
        resolved.worldPose.z = resolved.initialHeight + inputPose.z;
        resolved.platformPose = BuildPlatformPoseFromWorldPoseImpl(resolved.worldPose, resolved.initialHeight);
        return resolved;
    }

    resolved.platformPose = inputPose;
    resolved.platformPose.roll = resolved.worldPose.roll;
    resolved.platformPose.pitch = resolved.worldPose.pitch;
    resolved.platformPose.yaw = resolved.worldPose.yaw;

    const std::array<long double, 3> localDelta{{inputPose.x, inputPose.y, inputPose.z}};
    const auto worldDelta = StewartIK::TransformPlatformToWorldDelta(localDelta, resolved.worldPose);
    resolved.worldPose.x = worldDelta[0];
    resolved.worldPose.y = worldDelta[1];
    resolved.worldPose.z = resolved.initialHeight + worldDelta[2];
    return resolved;
}

bool TryParsePoseFromBody(
    const std::string& body,
    const char* prefix,
    Pose6D& pose) {
    const std::string base(prefix);
    return TryParseJsonLongDouble(body, base + "X", pose.x) &&
           TryParseJsonLongDouble(body, base + "Y", pose.y) &&
           TryParseJsonLongDouble(body, base + "Z", pose.z) &&
           TryParseJsonLongDouble(body, base + "Roll", pose.roll) &&
           TryParseJsonLongDouble(body, base + "Pitch", pose.pitch) &&
           TryParseJsonLongDouble(body, base + "Yaw", pose.yaw);
}

std::array<std::array<long double, 3>, 6> ComputeArmEndPointsImpl(
    const StewartParams& params,
    const std::array<long double, 6>& servoAngles) {
    std::array<std::array<long double, 3>, 6> armEndPoints{};
    const auto basePoints = StewartIK::CalculateSymmetricHexagonPoints(
        params.baseEdgeA,
        params.baseEdgeB,
        params.baseHeight);

    for (size_t index = 0; index < servoAngles.size(); ++index) {
        const int pi = params.servoPointMap[index];
        const long double servoAngleRad = servoAngles[index] * acosl(-1.0L) / 180.0L;
        const long double servoRadial = params.servoAxisAngle[index] * acosl(-1.0L) / 180.0L;
        armEndPoints[index] = {
            basePoints[pi][0] +
                params.servoArmLength * std::cosl(servoRadial) * std::cosl(servoAngleRad),
            basePoints[pi][1] +
                params.servoArmLength * std::sinl(servoRadial) * std::cosl(servoAngleRad),
            basePoints[pi][2] + params.servoArmLength * std::sinl(servoAngleRad)};
    }

    return armEndPoints;
}

std::vector<int> ExtractErrorArmsFromErrorsImpl(const std::vector<std::string>& errors) {
    std::vector<int> indices;
    for (const std::string& error : errors) {
        size_t markerPos = error.find(u8"腿");
        size_t markerLen = std::strlen(u8"腿");
        if (markerPos == std::string::npos) {
            markerPos = error.find(u8"力臂");
            markerLen = std::strlen(u8"力臂");
        }
        if (markerPos == std::string::npos) {
            continue;
        }

        size_t digitPos = markerPos + markerLen;
        int legIndex = 0;
        while (digitPos < error.size() &&
               std::isdigit(static_cast<unsigned char>(error[digitPos])) != 0) {
            legIndex = legIndex * 10 + (error[digitPos] - '0');
            ++digitPos;
        }

        if (legIndex <= 0 || legIndex > 6) {
            continue;
        }

        const int zeroBasedIndex = legIndex - 1;
        if (std::find(indices.begin(), indices.end(), zeroBasedIndex) == indices.end()) {
            indices.push_back(zeroBasedIndex);
        }
    }

    return indices;
}

bool ArePointsCollinear(
    const std::array<long double, 3>& p1,
    const std::array<long double, 3>& p2,
    const std::array<long double, 3>& p3,
    long double tolerance = 0.1L) {
    const std::array<long double, 3> v1{{p2[0] - p1[0], p2[1] - p1[1], p2[2] - p1[2]}};
    const std::array<long double, 3> v2{{p3[0] - p1[0], p3[1] - p1[1], p3[2] - p1[2]}};
    const long double crossX = v1[1] * v2[2] - v1[2] * v2[1];
    const long double crossY = v1[2] * v2[0] - v1[0] * v2[2];
    const long double crossZ = v1[0] * v2[1] - v1[1] * v2[0];
    return std::sqrtl(crossX * crossX + crossY * crossY + crossZ * crossZ) < tolerance;
}

void AddUniqueArmIndex(std::vector<int>& indices, int index) {
    if (index < 0 || index >= 6) {
        return;
    }
    if (std::find(indices.begin(), indices.end(), index) == indices.end()) {
        indices.push_back(index);
    }
}

StewartDiagnostics AnalyzeStewartResultImpl(
    const StewartParams& params,
    const StewartWebSettings& settings,
    const IKResult& result) {
    StewartDiagnostics diagnostics;
    diagnostics.initialHeight = StewartIK::CalculateInitialHeight(params, settings.servoInitialAngle);
    diagnostics.basePoints = StewartIK::CalculateSymmetricHexagonPoints(
        params.baseEdgeA,
        params.baseEdgeB,
        params.baseHeight);
    diagnostics.servoPointMap = params.servoPointMap;

    if (result.armEndPoints.size() != 6 || result.platformPoints.size() != 6) {
        return diagnostics;
    }

    const long double extendRatio = settings.singularityExtendRatio / 100.0L;
    const long double contractRatio = settings.singularityContractRatio / 100.0L;
    const long double verticalCos = std::cosl(settings.singularityVerticalAngle * acosl(-1.0L) / 180.0L);
    const long double horizontalCos = std::cosl(settings.singularityHorizontalAngle * acosl(-1.0L) / 180.0L);
    const long double maxReach = params.servoArmLength + params.rodLength;
    const long double minReach = std::fabsl(params.rodLength - params.servoArmLength);

    for (size_t index = 0; index < result.armEndPoints.size(); ++index) {
        const int pi = params.servoPointMap[index];
        const auto& basePoint = diagnostics.basePoints[pi];
        const auto& armEndPoint = result.armEndPoints[index];
        const auto& platformPoint = result.platformPoints[pi];

        const long double rodDx = platformPoint[0] - armEndPoint[0];
        const long double rodDy = platformPoint[1] - armEndPoint[1];
        const long double rodDz = platformPoint[2] - armEndPoint[2];
        const long double rodLength = std::sqrtl(rodDx * rodDx + rodDy * rodDy + rodDz * rodDz);
        diagnostics.rodLengths[index] = rodLength;

        if (maxReach > 0.0L && rodLength > maxReach * extendRatio) {
            diagnostics.singularityInfo[index] = {
                true,
                "extend",
                maxReach > 0.0L ? (rodLength / maxReach) * 100.0L : 0.0L,
                settings.singularityExtendRatio,
            };
            AddUniqueArmIndex(diagnostics.singularArms, static_cast<int>(index));
            diagnostics.warnings.push_back(
                std::string(u8"力臂") + std::to_string(index + 1) +
                u8": 接近完全伸展邊界奇點");
        } else if (minReach > 0.0L && rodLength < minReach * contractRatio) {
            diagnostics.singularityInfo[index] = {
                true,
                "contract",
                (rodLength / minReach) * 100.0L,
                settings.singularityContractRatio,
            };
            AddUniqueArmIndex(diagnostics.singularArms, static_cast<int>(index));
            diagnostics.warnings.push_back(
                std::string(u8"力臂") + std::to_string(index + 1) +
                u8": 接近完全收縮邊界奇點");
        }

        if (rodLength > 1e-9L) {
            const long double cosAngleWithZ = std::fabsl(rodDz) / rodLength;
            const long double angleWithZ =
                acosl((std::min)(1.0L, (std::max)(-1.0L, cosAngleWithZ))) * 180.0L / acosl(-1.0L);

            if (cosAngleWithZ > verticalCos) {
                diagnostics.singularityInfo[index] = {
                    true,
                    "vertical",
                    angleWithZ,
                    settings.singularityVerticalAngle,
                };
                AddUniqueArmIndex(diagnostics.singularArms, static_cast<int>(index));
                diagnostics.warnings.push_back(
                    std::string(u8"力臂") + std::to_string(index + 1) +
                    u8": 連接桿接近垂直，存在幾何奇點風險");
            } else if (cosAngleWithZ < horizontalCos) {
                diagnostics.singularityInfo[index] = {
                    true,
                    "horizontal",
                    angleWithZ,
                    settings.singularityHorizontalAngle,
                };
                AddUniqueArmIndex(diagnostics.singularArms, static_cast<int>(index));
                diagnostics.warnings.push_back(
                    std::string(u8"力臂") + std::to_string(index + 1) +
                    u8": 連接桿接近水平，存在幾何奇點風險");
            }
        }

        const long double armVecX = armEndPoint[0] - basePoint[0];
        const long double armVecY = armEndPoint[1] - basePoint[1];
        const long double rodVecX = platformPoint[0] - armEndPoint[0];
        const long double rodVecY = platformPoint[1] - armEndPoint[1];
        const long double armLenXY = std::sqrtl(armVecX * armVecX + armVecY * armVecY);
        const long double rodLenXY = std::sqrtl(rodVecX * rodVecX + rodVecY * rodVecY);

        if (armLenXY > 1e-9L && rodLenXY > 1e-9L) {
            const long double dot = armVecX * rodVecX + armVecY * rodVecY;
            long double fisheyeAngle =
                acosl((std::max)(-1.0L, (std::min)(1.0L, dot / (armLenXY * rodLenXY)))) * 180.0L / acosl(-1.0L);
            if (fisheyeAngle > 90.0L) {
                fisheyeAngle = 180.0L - fisheyeAngle;
            }
            diagnostics.fisheyeAngles[index] = fisheyeAngle;

            if (fisheyeAngle > params.fisheyeAngleLimit) {
                AddUniqueArmIndex(diagnostics.fisheyeWarningArms, static_cast<int>(index));
                diagnostics.warnings.push_back(
                    std::string(u8"力臂") + std::to_string(index + 1) +
                    u8": 魚眼軸承角度 " + std::to_string(static_cast<double>(fisheyeAngle)) +
                    u8"° 超過限制");
            }
        }
    }

    for (size_t i = 0; i < result.platformPoints.size(); ++i) {
        for (size_t j = i + 1; j < result.platformPoints.size(); ++j) {
            for (size_t k = j + 1; k < result.platformPoints.size(); ++k) {
                if (ArePointsCollinear(result.platformPoints[i], result.platformPoints[j], result.platformPoints[k])) {
                    diagnostics.platformSingularity = true;
                    diagnostics.warnings.push_back(
                        std::string(u8"平台點") + std::to_string(i + 1) + "," +
                        std::to_string(j + 1) + "," + std::to_string(k + 1) +
                        u8": 三點共線，平台奇點");
                }
            }
        }
    }

    diagnostics.warningArms = diagnostics.singularArms;
    for (const int index : diagnostics.fisheyeWarningArms) {
        AddUniqueArmIndex(diagnostics.warningArms, index);
    }
    return diagnostics;
}

void AppendJsonPoint3(std::ostringstream& response, const std::array<long double, 3>& point) {
    response << std::setprecision(kJsonFloatPrecision);
    response << "[" << static_cast<double>(point[0]) << "," << static_cast<double>(point[1])
             << "," << static_cast<double>(point[2]) << "]";
}

void AppendJsonArray6(std::ostringstream& response, const std::array<long double, 6>& values) {
    response << std::setprecision(kJsonFloatPrecision);
    response << "[";
    for (size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            response << ",";
        }
        response << static_cast<double>(values[index]);
    }
    response << "]";
}

void AppendJsonIntArray6(std::ostringstream& response, const std::array<int, 6>& values) {
    response << "[";
    for (size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            response << ",";
        }
        response << values[index];
    }
    response << "]";
}

void AppendJsonPointArray6(
    std::ostringstream& response,
    const std::array<std::array<long double, 3>, 6>& points) {
    response << "[";
    for (size_t index = 0; index < points.size(); ++index) {
        if (index != 0) {
            response << ",";
        }
        AppendJsonPoint3(response, points[index]);
    }
    response << "]";
}

void AppendJsonErrorList(std::ostringstream& response, const std::vector<std::string>& errors) {
    response << "[";
    for (size_t index = 0; index < errors.size(); ++index) {
        if (index != 0) {
            response << ",";
        }
        response << "\"" << CoreHelpers::EscapeJsonString(errors[index]) << "\"";
    }
    response << "]";
}

void AppendJsonIntList(std::ostringstream& response, const std::vector<int>& values) {
    response << "[";
    for (size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            response << ",";
        }
        response << values[index];
    }
    response << "]";
}

void AppendJsonPose(std::ostringstream& response, const Pose6D& pose) {
    response << std::setprecision(kJsonFloatPrecision);
    response << "{";
    response << "\"x\":" << static_cast<double>(pose.x);
    response << ",\"y\":" << static_cast<double>(pose.y);
    response << ",\"z\":" << static_cast<double>(pose.z);
    response << ",\"roll\":" << static_cast<double>(pose.roll);
    response << ",\"pitch\":" << static_cast<double>(pose.pitch);
    response << ",\"yaw\":" << static_cast<double>(pose.yaw);
    response << "}";
}

void AppendJsonArmDiagnostics(
    std::ostringstream& response,
    const std::array<StewartArmDiagnostic, 6>& diagnostics) {
    response << "{";
    bool first = true;
    for (size_t index = 0; index < diagnostics.size(); ++index) {
        const auto& diagnostic = diagnostics[index];
        if (!diagnostic.active) {
            continue;
        }
        if (!first) {
            response << ",";
        }
        first = false;
        response << "\"" << index << "\":{";
        response << "\"type\":\"" << CoreHelpers::EscapeJsonString(diagnostic.type) << "\"";
        response << ",\"value\":" << static_cast<double>(diagnostic.value);
        response << ",\"threshold\":" << static_cast<double>(diagnostic.threshold);
        response << "}";
    }
    response << "}";
}

std::string BuildStewartResultJson(
    const IKResult& result,
    const StewartResolvedPoseInput& resolvedPose,
    const StewartDiagnostics& diagnostics,
    bool validOverride,
    const WuSOEM::Pose6DValidationResult* poseValidation = nullptr) {
    const std::vector<int> errorArms = ExtractErrorArmsFromErrorsImpl(result.errors);
    std::ostringstream response;
    response << "{";
    response << "\"valid\":" << (validOverride ? "true" : "false");
    response << ",\"worldPose\":";
    AppendJsonPose(response, resolvedPose.worldPose);
    response << ",\"platformPose\":";
    AppendJsonPose(response, resolvedPose.platformPose);
    response << ",\"initialHeight\":" << static_cast<double>(resolvedPose.initialHeight);
    response << ",\"servoAngles\":";
    AppendJsonArray6(response, result.servoAngles);
    response << ",\"armEndPoints\":";
    AppendJsonPointArray6(response, result.armEndPoints);
    response << ",\"platformPoints\":";
    AppendJsonPointArray6(response, result.platformPoints);
    response << ",\"basePoints\":";
    AppendJsonPointArray6(response, diagnostics.basePoints);
    response << ",\"servoPointMap\":";
    AppendJsonIntArray6(response, diagnostics.servoPointMap);
    response << ",\"rodLengths\":";
    AppendJsonArray6(response, diagnostics.rodLengths);
    response << ",\"fisheyeAngles\":";
    AppendJsonArray6(response, diagnostics.fisheyeAngles);
    response << ",\"errors\":";
    AppendJsonErrorList(response, result.errors);
    response << ",\"warnings\":";
    AppendJsonErrorList(response, diagnostics.warnings);
    response << ",\"errorArms\":";
    AppendJsonIntList(response, errorArms);
    response << ",\"warningArms\":";
    AppendJsonIntList(response, diagnostics.warningArms);
    response << ",\"singularArms\":";
    AppendJsonIntList(response, diagnostics.singularArms);
    response << ",\"fisheyeWarningArms\":";
    AppendJsonIntList(response, diagnostics.fisheyeWarningArms);
    response << ",\"singularityInfo\":";
    AppendJsonArmDiagnostics(response, diagnostics.singularityInfo);
    response << ",\"platformSingularity\":" << (diagnostics.platformSingularity ? "true" : "false");
    // Pose6D 前置驗證層結果
    if (poseValidation) {
        response << ",\"poseValidation\":{";
        response << "\"valid\":" << (poseValidation->valid ? "true" : "false");
        response << ",\"violationKind\":\"" << static_cast<int>(poseValidation->violationKind) << "\"";
        response << ",\"limitValue\":" << static_cast<double>(poseValidation->limitValue);
        response << ",\"actualValue\":" << static_cast<double>(poseValidation->actualValue);
        response << ",\"message\":\"" << CoreHelpers::EscapeJsonString(poseValidation->message) << "\"";
        response << "}";
    }
    response << "}";
    return response.str();
}

std::string BuildInverseKinematicsJson(
    const IKResult& result,
    const StewartResolvedPoseInput& resolvedPose,
    const StewartDiagnostics& diagnostics) {
    return BuildStewartResultJson(
        result,
        resolvedPose,
        diagnostics,
        result.valid && !diagnostics.platformSingularity);
}

std::string BuildInverseKinematicsJson(
    const IKResult& result,
    const StewartResolvedPoseInput& resolvedPose,
    const StewartDiagnostics& diagnostics,
    const WuSOEM::Pose6DValidationResult& poseValidation) {
    return BuildStewartResultJson(
        result,
        resolvedPose,
        diagnostics,
        result.valid && !diagnostics.platformSingularity && poseValidation.valid,
        &poseValidation);
}

std::string BuildForwardKinematicsJsonImpl(
    const ForwardResult& forwardResult,
    const StewartResolvedPoseInput& resolvedPose,
    const std::array<long double, 6>& servoAngles,
    const std::array<std::array<long double, 3>, 6>& armEndPoints,
    const std::array<std::array<long double, 3>, 6>& platformPoints,
    const StewartDiagnostics& diagnostics,
    const std::vector<int>& errorArms) {
    (void)errorArms;

    IKResult result;
    result.valid = forwardResult.valid;
    result.servoAngles = servoAngles;
    result.armEndPoints = armEndPoints;
    result.platformPoints = platformPoints;
    result.errors = forwardResult.errors;

    std::string json = BuildStewartResultJson(
        result,
        resolvedPose,
        diagnostics,
        forwardResult.valid && !diagnostics.platformSingularity);

    if (!json.empty() && json.front() == '{') {
        std::ostringstream response;
        response << "{\"pose\":";
        AppendJsonPose(response, forwardResult.pose);
        if (json.size() > 1) {
            response << "," << json.substr(1);
        } else {
            response << "}";
        }
        json = response.str();
    }

    if (errorArms.empty()) {
        return json;
    }

    return json;
}

}  // namespace

namespace HttpServerInternal {

WuSOEM::Pose6D BuildPlatformPoseFromWorldPose(
    const WuSOEM::Pose6D& worldPose,
    long double initialHeight) {
    return BuildPlatformPoseFromWorldPoseImpl(worldPose, initialHeight);
}

std::array<std::array<long double, 3>, 6> ComputeArmEndPoints(
    const WuSOEM::StewartParams& params,
    const std::array<long double, 6>& servoAngles) {
    return ComputeArmEndPointsImpl(params, servoAngles);
}

std::vector<int> ExtractErrorArmsFromErrors(const std::vector<std::string>& errors) {
    return ExtractErrorArmsFromErrorsImpl(errors);
}

StewartDiagnostics AnalyzeStewartResult(
    const WuSOEM::StewartParams& params,
    const StewartWebSettings& settings,
    const WuSOEM::IKResult& result) {
    return AnalyzeStewartResultImpl(params, settings, result);
}

std::string BuildForwardKinematicsJson(
    const WuSOEM::ForwardResult& forwardResult,
    const StewartResolvedPoseInput& resolvedPose,
    const std::array<long double, 6>& servoAngles,
    const std::array<std::array<long double, 3>, 6>& armEndPoints,
    const std::array<std::array<long double, 3>, 6>& platformPoints,
    const StewartDiagnostics& diagnostics,
    const std::vector<int>& errorArms) {
    return BuildForwardKinematicsJsonImpl(
        forwardResult,
        resolvedPose,
        servoAngles,
        armEndPoints,
        platformPoints,
        diagnostics,
        errorArms);
}

}  // namespace HttpServerInternal

void SetStewartPose6DLimits(const WuSOEM::Pose6DLimits& limits) {
    HttpServerInternal::SetPose6DLimits(limits);
}

WuSOEM::Pose6DLimits GetStewartPose6DLimits() {
    return HttpServerInternal::GetPose6DLimits();
}

void SetAdapterStatus(const std::string& s) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(g_adapterStatusMutex);
        if (g_adapterStatus != s) {
            g_adapterStatus = s;
            changed = true;
        }
    }

    if (changed) {
        ::BroadcastSSE(GetUiStateJson());
    }
}

void SetUiDisabled(const std::string& name, bool disabled) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(g_uiStateMutex);
        auto it = g_uiDisabled.find(name);
        if (it == g_uiDisabled.end() || it->second != disabled) {
            g_uiDisabled[name] = disabled;
            changed = true;
        }
    }

    if (changed) {
        ::BroadcastSSE(GetUiStateJson());
    }
}

void SetMailboxFromHttp(const std::string& utf8Body) {
    std::lock_guard<std::mutex> lk(g_mailboxMutex);
    g_mailboxUtf8 = utf8Body;
}

std::string GetMailboxUtf8() {
    std::lock_guard<std::mutex> lk(g_mailboxMutex);
    return g_mailboxUtf8;
}

std::string GetLastStatusUtf8() {
    std::lock_guard<std::mutex> lk(g_statusMutex);
    return g_lastStatusUtf8;
}

void SetLastStatusUtf8(const std::string& utf8) {
    std::lock_guard<std::mutex> lk(g_statusMutex);
    g_lastStatusUtf8 = utf8;
    NormalizeStatusCacheUtf8(g_lastStatusUtf8);
}

void PostCommandToUIOrCache(const std::string& cmd) {
    if (cmd.rfind("HTTP_CMD:CONNECT", 0) == 0 || cmd.rfind("HTTP_CMD:CONNECT_WEB:", 0) == 0) {
        using namespace std::chrono;
        long long nowMs =
            duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
        long long prev = g_lastHttpConnPostTimeMs.load();
        if (nowMs - prev < kHttpConnThrottleMs) {
            if (g_hwndHttpTarget && IsWindow(g_hwndHttpTarget)) {
                try {
                    MainWindow::PostUTF8LogToHWND(
                        g_hwndHttpTarget,
                        std::string("HTTP_LOCAL:[HTTP_CMD] Throttled CONNECT request\n"));
                } catch (...) {
                }
            } else {
                std::lock_guard<std::mutex> lk(g_statusMutex);
                AppendStatusCacheLine(g_lastStatusUtf8, "[HTTP_CMD_THROTTLED] CONNECT");
            }
            return;
        }
        g_lastHttpConnPostTimeMs.store(nowMs);
    }

    if (g_hwndHttpTarget) {
        if (cmd.rfind("HTTP_CMD:CONNECT", 0) == 0 || cmd.rfind("HTTP_CMD:CONNECT_WEB:", 0) == 0) {
            auto snapshot = AppController::GetInstance().GetStatusSnapshot();
            if (snapshot.busy || snapshot.master_initialized || snapshot.worker_running) {
                std::lock_guard<std::mutex> lk(g_statusMutex);
                AppendStatusCacheLine(
                    g_lastStatusUtf8,
                    "[HTTP_CMD_REJECTED] CONNECT - already running or connected");
                return;
            }
        }

        if (cmd.rfind("HTTP_CMD:AUTO_CONNECT", 0) == 0) {
            auto snapshot = AppController::GetInstance().GetStatusSnapshot();
            const auto decision = CoreHelpers::EvaluateAutoConnect(snapshot);
            if (!decision.allowed) {
                std::lock_guard<std::mutex> lk(g_statusMutex);
                AppendStatusCacheLine(
                    g_lastStatusUtf8,
                    std::string("[HTTP_CMD_REJECTED] AUTO_CONNECT - ") +
                        CoreHelpers::DescribeCommandPolicyRejection(decision.rejection));
                return;
            }
        }

        MainWindow::PostUTF8LogToHWND(g_hwndHttpTarget, cmd);
        return;
    }

    HWND hwndFallback = FindWindowW(L"EtherCATWindowClass", NULL);
    if (hwndFallback && IsWindow(hwndFallback)) {
        MainWindow::PostUTF8LogToHWND(hwndFallback, cmd);
        return;
    }

    std::lock_guard<std::mutex> lk(g_statusMutex);
    AppendStatusCacheLine(g_lastStatusUtf8, std::string("[HTTP CMD FALLBACK] ") + cmd);
}

std::string GetUiStateJson() {
    const auto autoButtonState = CoreHelpers::EvaluateAutoActionButtons(
        GetHttpTargetSystemStateSnapshot(),
        GetHttpAutoActionButtonInputs());

    // Resolve the selected-adapter snapshot BEFORE taking g_uiStateMutex.
    // GetSelectedAdapterUiState() may call into MainWindow (GetSelectedAdapterUtf8)
    // which performs a cross-thread SendMessageW to the adapter combobox. Holding
    // g_uiStateMutex across that call deadlocks the UI thread during startup:
    // MainWindow::Create() -> SetUiDisabled("adapter_select", ...) wants this same
    // mutex while the HTTP worker waits for the (not yet pumping) UI thread.
    const SelectedAdapterUiState selectedAdapter = GetSelectedAdapterUiState();

    std::lock_guard<std::mutex> lk(g_uiStateMutex);
    std::ostringstream ss;
    ss << "{";

    bool first = true;
    for (const auto& kv : g_uiDisabled) {
        if (IsAutoActionUiStateKey(kv.first)) {
            continue;
        }
        if (!first) {
            ss << ",";
        }
        first = false;
        ss << "\"" << kv.first << "\":" << (kv.second ? "true" : "false");
    }

    if (!first) {
        ss << ",";
    }
    AppendAutoActionButtonStateJson(ss, autoButtonState);
    first = false;

    {
        std::lock_guard<std::mutex> clk(g_cycleMutex);
        if (!first) {
            ss << ",";
        }
        ss << "\"desiredCycleTime\":" << g_desiredCycleTimeUs;
        ss << ",\"commsTime\":" << g_commsTimeUs;
        ss << ",\"actualCycleTime\":" << g_actualCycleTimeUs;
        ss << ",\"maxObservedCycleTime\":" << g_maxObservedCycleTimeUs;
        ss << ",\"spikePhase\":" << g_spikePhase;
        ss << ",\"spikeCycleUs\":" << g_spikeCycleUs;
        ss << ",\"spikeCommsUs\":" << g_spikeCommsUs;
        ss << ",\"spikeCount\":" << g_spikeCount;
    }

    {
        std::lock_guard<std::mutex> lk2(g_adapterStatusMutex);
        if (!g_adapterStatus.empty()) {
            if (!first) {
                ss << ",";
            }
            ss << "\"adapter_status\":\"" << CoreHelpers::EscapeJsonString(g_adapterStatus)
               << "\"";
            first = false;
        }
    }

    {
        if (!selectedAdapter.value.empty()) {
            if (!first) {
                ss << ",";
            }
            ss << "\"selected_adapter\":\"" << CoreHelpers::EscapeJsonString(selectedAdapter.value) << "\"";
            if (!selectedAdapter.display.empty()) {
                ss << ",\"selected_adapter_display\":\"" << CoreHelpers::EscapeJsonString(selectedAdapter.display) << "\"";
            }
        }
    }

    ss << "}";
    return ss.str();
}

void SetCycleInfo(uint32_t desiredUs, uint32_t commsUs, uint32_t actualUs) {
    auto now = std::chrono::system_clock::now();
    std::lock_guard<std::mutex> clk(g_cycleMutex);
    if (now < g_ignoreWin32EchoUntil) {
        g_commsTimeUs = commsUs;
        g_actualCycleTimeUs = actualUs;
    } else {
        g_desiredCycleTimeUs = desiredUs;
        g_commsTimeUs = commsUs;
        g_actualCycleTimeUs = actualUs;
    }
}

void SetMaxObservedCycleTime(uint32_t maxUs) {
    std::lock_guard<std::mutex> clk(g_cycleMutex);
    g_maxObservedCycleTimeUs = maxUs;
}

void SetSpikeDiagnostics(uint32_t phase, uint32_t cycleUs, uint32_t commsUs, uint32_t count) {
    std::lock_guard<std::mutex> clk(g_cycleMutex);
    g_spikePhase = phase;
    g_spikeCycleUs = cycleUs;
    g_spikeCommsUs = commsUs;
    g_spikeCount = count;
}

void SetSlaveCacheJson(const std::string& json) {
    std::lock_guard<std::mutex> lk(g_slaveCacheMutex);
    g_slaveInfoCacheJson = json;
}

void SetSlaveSdoCacheJson(int slave, const std::string& json) {
    std::lock_guard<std::mutex> lk(g_slaveSdoCacheMutex);
    if (json.empty()) {
        g_slaveSdoCache.erase(slave);
    } else {
        g_slaveSdoCache[slave] = json;
    }
}

std::string GetSlaveSdoCacheJson(int slave) {
    std::lock_guard<std::mutex> lk(g_slaveSdoCacheMutex);
    auto it = g_slaveSdoCache.find(slave);
    if (it == g_slaveSdoCache.end()) {
        return std::string();
    }
    return it->second;
}

std::string GetSlaveCacheJson() {
    std::lock_guard<std::mutex> lk(g_slaveCacheMutex);
    return g_slaveInfoCacheJson;
}

namespace {

void SendContentResponse(SOCKET client,
                         const std::string& statusLine,
                         const char* contentType,
                         const std::string& body) {
    std::ostringstream hdr;
    hdr << statusLine << "\r\n"
        << "Connection: close\r\n"
        << "Content-Type: " << contentType << "\r\n"
        << "Content-Length: " << body.size() << "\r\n\r\n";
    std::string hs = hdr.str();
    SendAll(client, hs);
    if (!body.empty()) {
        SendAll(client, body);
    }
}

void SendJsonResponse(SOCKET client, const std::string& statusLine, const std::string& body) {
    SendContentResponse(client, statusLine, "application/json; charset=utf-8", body);
}

void SendUnauthorized(SOCKET client) {
    const char* una = "HTTP/1.1 401 Unauthorized\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
    SendAll(client, una);
}

void SendNoContent(SOCKET client) {
    const char* ok = "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n";
    SendAll(client, ok);
}

}  // namespace

namespace HttpServerInternal {

RequestHandleResult TryHandleApiRequest(const HttpRequestContext& ctx) {
    const SOCKET client = ctx.client;
    const std::string& method = ctx.method;
    const std::string& path = ctx.path;
    const std::string& req = ctx.requestRaw;
    const std::string& body = ctx.body;
    const std::string& clientIP = ctx.clientIP;

    if (method == "GET" && path == "/adapters") {
        if (!IsRequestAuthorized(req, clientIP)) {
            SendUnauthorized(client);
            return RequestHandleResult::kHandledClose;
        }

        std::string content = CoreHelpers::EnumerateAdaptersJson();
        if (content.empty()) {
            content = "[]";
        }
        SendJsonResponse(client, "HTTP/1.1 200 OK", content);
        return RequestHandleResult::kHandledClose;
    }

    if (method == "GET" && path == "/slaves") {
        if (!IsRequestAuthorized(req, clientIP)) {
            SendUnauthorized(client);
            return RequestHandleResult::kHandledClose;
        }

        std::string cache = GetSlaveCacheJson();
        if (cache.empty()) {
            cache = "[]";
        }
        SendJsonResponse(client, "HTTP/1.1 200 OK", cache);
        return RequestHandleResult::kHandledClose;
    }

    if (method == "GET" && path.rfind("/sdo", 0) == 0) {
        if (!IsRequestAuthorized(req, clientIP)) {
            SendUnauthorized(client);
            return RequestHandleResult::kHandledClose;
        }

        int slave = ParseSlaveFromPath(path);
        std::string out;
        if (slave > 0) {
            out = GetSlaveSdoCacheJson(slave);
        }

        if (out.empty()) {
            const char* notf =
                "HTTP/1.1 404 Not Found\r\nContent-Type: application/json; charset=utf-8\r\n\r\n{}";
            SendAll(client, notf);
            return RequestHandleResult::kHandledClose;
        }

        SendJsonResponse(client, "HTTP/1.1 200 OK", out);
        return RequestHandleResult::kHandledClose;
    }

    if (method == "GET" && path == "/ui_state") {
        if (!IsRequestAuthorized(req, clientIP)) {
            SendUnauthorized(client);
            return RequestHandleResult::kHandledClose;
        }

        std::string js = GetUiStateJson();
        SendJsonResponse(client, "HTTP/1.1 200 OK", js);
        return RequestHandleResult::kHandledClose;
    }

    if (method == "GET" && path == "/status") {
        if (!IsRequestAuthorized(req, clientIP)) {
            SendUnauthorized(client);
            return RequestHandleResult::kHandledClose;
        }

        std::string s = GetLastStatusUtf8();
        std::ostringstream hdr;
        hdr << "HTTP/1.1 200 OK\r\n"
            << "Content-Type: text/plain; charset=utf-8\r\n"
            << "Content-Length: " << s.size() << "\r\n\r\n";
        std::string hs = hdr.str();
        SendAll(client, hs);
        if (!s.empty()) {
            SendAll(client, s);
        }
        return RequestHandleResult::kHandledClose;
    }

    if (method == "GET" && path == "/features") {
        if (!IsRequestAuthorized(req, clientIP)) {
            SendUnauthorized(client);
            return RequestHandleResult::kHandledClose;
        }

        SendJsonResponse(client, "HTTP/1.1 200 OK", "{\"features\":[]}");
        return RequestHandleResult::kHandledClose;
    }

    if (method == "GET" && path == "/info") {
        if (!IsRequestAuthorized(req, clientIP)) {
            SendUnauthorized(client);
            return RequestHandleResult::kHandledClose;
        }

        std::ostringstream oss;
        oss << "{";
        oss << "\"http_port\": " << g_httpPort.load() << ", ";
        oss << "\"bind_addr\": \"" << g_httpBindAddr << "\", ";
        oss << "\"build\": \"\"";
        oss << "}";

        SendJsonResponse(client, "HTTP/1.1 200 OK", oss.str());
        return RequestHandleResult::kHandledClose;
    }

    if (method == "GET" && path == "/conn_status") {
        if (!IsRequestAuthorized(req, clientIP)) {
            SendUnauthorized(client);
            return RequestHandleResult::kHandledClose;
        }

        std::ostringstream stat;
        stat << "{" << "\"active\":" << g_activeConnections.load() << ",\"max\":"
             << g_maxConnections << "}";
        SendJsonResponse(client, "HTTP/1.1 200 OK", stat.str());
        return RequestHandleResult::kHandledClose;
    }

    if (method == "GET" && path == "/pdo_data") {
        if (!IsRequestAuthorized(req, clientIP)) {
            SendUnauthorized(client);
            return RequestHandleResult::kHandledClose;
        }

        std::string json = "{}";
        EnsurePDOMapped();
        if (g_pMmView && g_pMmView->magic == MMFWriter::MAGIC) {
            std::array<float, 6> mmfInValues = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
            std::array<double, 6> mmfInVelocities = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
            std::array<double, 6> filterInputAngles = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
            std::array<uint16_t, 6> errorCodes603F = {0, 0, 0, 0, 0, 0};
            std::array<bool, 6> errorCodeValid603F = {false, false, false, false, false, false};
            bool mmfInMapped = false;
            bool mmfInRunning = false;
            if (auto* win = GetHttpTargetMainWindow()) {
                mmfInMapped = win->MasterMMF1IsMapped();
                mmfInRunning = win->MasterIsMMF1InRunning();
                win->GetLastReportedErrorCodes(errorCodes603F, errorCodeValid603F);
                if (mmfInMapped && mmfInRunning) {
                    win->MasterGetMMF1LatestWithVelocities(mmfInValues, mmfInVelocities);
                }
            }
            if (auto* master = AppController::GetInstance().GetMaster()) {
                filterInputAngles = master->GetPDOFilterInputAngles();
            }

            std::ostringstream oss;
            oss << "{\"seq\":" << g_pMmView->sequenceNumber << ",\"ts\":" << g_pMmView->timestamp
                << ",\"epoch\":" << g_pMmView->epochMs << ",\"count\":" << g_pMmView->slaveCount;

            {
                const auto snapshot = GetHttpTargetSystemStateSnapshot();
                oss << ",\"oEtherCatOk\":" << (snapshot.ethercat_ok ? "true" : "false")
                    << ",\"oBusy\":" << (snapshot.busy ? "true" : "false")
                    << ",\"oError\":" << (snapshot.error ? "true" : "false")
                    << ",\"oReady\":" << (snapshot.ready ? "true" : "false")
                    << ",\"oHomeDone\":" << (snapshot.home_done ? "true" : "false")
                    << ",\"oOnline\":" << (snapshot.online ? "true" : "false")
                    << ",\"mmf1InputMode\":\"" << GetActiveMmf1InputModeJsonValue() << "\"";

                std::lock_guard<std::mutex> lk(g_uiStateMutex);
                std::ostringstream uiFrag;
                bool first = true;
                for (const auto& kv : g_uiDisabled) {
                    if (IsAutoActionUiStateKey(kv.first)) {
                        continue;
                    }
                    if (!first) {
                        uiFrag << ",";
                    }
                    first = false;
                    uiFrag << "\"" << kv.first << "\":" << (kv.second ? "true" : "false");
                }

                const auto buttonState = CoreHelpers::EvaluateAutoActionButtons(
                    snapshot,
                    GetHttpAutoActionButtonInputs());
                if (!first) {
                    uiFrag << ",";
                }
                                AppendAutoActionButtonStateJson(uiFrag, buttonState);

                if (!uiFrag.str().empty()) {
                    oss << "," << uiFrag.str();
                }
            }

            oss << ",\"slaves\":[";
            for (uint32_t i = 0; i < g_pMmView->slaveCount && i < 6; ++i) {
                if (i > 0) {
                    oss << ",";
                }
                const auto& s = g_pMmView->slaves[i];
                const double mmfIn = (mmfInMapped && mmfInRunning)
                                         ? static_cast<double>(mmfInValues[i])
                                         : s.mmfIn;
                oss << "{\"tPos\":" << s.targetPosition << ",\"tVel\":" << s.targetVelocity
                    << ",\"aPos\":" << s.actualPosition << ",\"aVel\":" << s.actualVelocity
                    << ",\"filterInput\":" << filterInputAngles[i]
                    << ",\"filter\":" << s.pdoFilterOutput << ",\"filterVel\":"
                    << s.pdoFilterVel << ",\"filterAcc\":" << s.pdoFilterAcc << ",\"mmfIn\":"
                    << mmfIn << ",\"mmfInVel\":" << mmfInVelocities[i]
                    << ",\"errorCode603F\":";
                if (errorCodeValid603F[i]) {
                    oss << errorCodes603F[i];
                } else {
                    oss << "null";
                }
                oss << ",\"errorCode603FValid\":" << (errorCodeValid603F[i] ? "true" : "false")
                    << ",\"errorCode603FReadFailed\":"
                    << ((errorCodeValid603F[i] && errorCodes603F[i] == kErrorCode603FReadFailureSentinel) ? "true" : "false")
                    << ",\"cw\":" << s.controlWord << ",\"sw\":" << s.statusWord
                    << ",\"tAcc\":" << s.targetAcceleration << ",\"aAcc\":" << s.actualAcceleration
                    << "}";
            }
            oss << "]}";
            json = oss.str();
        }

        SendJsonResponse(client, "HTTP/1.1 200 OK", json);
        return RequestHandleResult::kHandledClose;
    }

    if (method == "GET" && path == "/pdo_sse") {
        if (!IsRequestAuthorized(req, clientIP)) {
            SendUnauthorized(client);
            return RequestHandleResult::kHandledClose;
        }

        std::string hdr =
            "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\n"
            "Connection: keep-alive\r\n\r\n";
        SendAll(client, hdr);
        {
            std::lock_guard<std::mutex> lk(g_sseClientsMutex);
            g_sseClients.push_back({client, clientIP});
        }
        g_sseWakeCv.notify_one();
        UnregisterIPConnection(clientIP, client);
        StartSseBroadcaster();
        return RequestHandleResult::kHandledKeepOpen;
    }

    if (method == "GET" && path == "/api/stewart/sse") {
        if (!IsRequestAuthorized(req, clientIP)) {
            SendUnauthorized(client);
            return RequestHandleResult::kHandledClose;
        }

        std::string hdr =
            "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\n"
            "Connection: keep-alive\r\n\r\n";
        SendAll(client, hdr);
        {
            std::lock_guard<std::mutex> lk(g_stewartSseClientsMutex);
            g_stewartSseClients.push_back({client, clientIP});
        }
        g_stewartSseWakeCv.notify_one();
        UnregisterIPConnection(clientIP, client);
        StartStewartSseBroadcaster();
        return RequestHandleResult::kHandledKeepOpen;
    }

    if (method == "POST" && path == "/login") {
        if (IsLoginLockedOut(clientIP)) {
            const char* locked =
                "HTTP/1.1 429 Too Many Requests\r\nConnection: close\r\nContent-Type: text/plain; charset=utf-8\r\n"
                "Content-Length: 27\r\n\r\nToo many login attempts";
            SendAll(client, locked);
            return RequestHandleResult::kHandledClose;
        }

        if (!CheckRateLimit(clientIP)) {
            const char* rateLimit = "HTTP/1.1 429 Too Many Requests\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
            SendAll(client, rateLimit);
            return RequestHandleResult::kHandledClose;
        }

        std::string val = GetHeaderValue(req, "x-api-key");
        bool okLogin = false;
        if (!val.empty()) {
            std::string trimmed = TrimHttpValue(val);
            if (IsValidApiKey(trimmed)) {
                okLogin = true;
            }
        }

        if (!okLogin) {
            RecordFailedLoginAttempt(clientIP);
            SendUnauthorized(client);
            return RequestHandleResult::kHandledClose;
        }

        ClearLoginAttempts(clientIP);

        auto now = std::chrono::system_clock::now();
        {
            std::lock_guard<std::mutex> lk(g_sessionMutex);
            g_sessions[clientIP] = {now + std::chrono::seconds(g_sessionTtlSeconds), now};
        }

        const char* ok = "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
        SendAll(client, ok);

        try {
            PostCommandToUIOrCache(std::string("HTTP_CMD:REPORT_UI_STATE"));
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            SetUiDisabled("oscilloscope", false);
            SetUiDisabled("attitude", false);
            SetUiDisabled("mechanism", false);
            SetUiDisabled("auto_test", false);
            SetUiDisabled("toggle_status", false);
            BroadcastSSE(GetUiStateJson());
        } catch (...) {
        }

        return RequestHandleResult::kHandledClose;
    }

    if (method == "POST" && path == "/logout") {
        {
            std::lock_guard<std::mutex> lk(g_sessionMutex);
            g_sessions.erase(clientIP);
        }

        try {
            std::lock_guard<std::mutex> lk(g_sseClientsMutex);
            for (auto it = g_sseClients.begin(); it != g_sseClients.end();) {
                if (it->ip == clientIP) {
                    try {
                        SOCKET s = it->socket;
                        try {
                            closesocket(s);
                        } catch (...) {
                        }
                        try {
                            CloseClientAndDecrement(s);
                        } catch (...) {
                        }
                    } catch (...) {
                    }
                    try {
                        PostHttpConnCountIfChanged();
                    } catch (...) {
                    }
                    it = g_sseClients.erase(it);
                } else {
                    ++it;
                }
            }
        } catch (...) {
        }

        try {
            std::lock_guard<std::mutex> lk(g_stewartSseClientsMutex);
            for (auto it = g_stewartSseClients.begin(); it != g_stewartSseClients.end();) {
                if (it->ip == clientIP) {
                    try {
                        SOCKET s = it->socket;
                        try {
                            closesocket(s);
                        } catch (...) {
                        }
                        try {
                            CloseClientAndDecrement(s);
                        } catch (...) {
                        }
                    } catch (...) {
                    }
                    try {
                        PostHttpConnCountIfChanged();
                    } catch (...) {
                    }
                    it = g_stewartSseClients.erase(it);
                } else {
                    ++it;
                }
            }
        } catch (...) {
        }

        SendNoContent(client);

        try {
            PostCommandToUIOrCache(std::string("HTTP_CMD:REPORT_UI_STATE"));
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            SetUiDisabled("oscilloscope", true);
            SetUiDisabled("attitude", true);
            SetUiDisabled("mechanism", true);
            SetUiDisabled("auto_test", true);
            SetUiDisabled("toggle_status", true);
            BroadcastSSE(GetUiStateJson());
        } catch (...) {
        }

        return RequestHandleResult::kHandledClose;
    }

    if (method == "GET" && path == "/session_status") {
        bool loggedIn = false;
        {
            std::lock_guard<std::mutex> lk(g_sessionMutex);
            const auto now = std::chrono::system_clock::now();
            loggedIn = TryRefreshSessionByClientIP(clientIP, now);
        }

        std::string response = loggedIn ? "{\"loggedIn\":true}" : "{\"loggedIn\":false}";
        SendJsonResponse(client, "HTTP/1.1 200 OK", response);
        return RequestHandleResult::kHandledClose;
    }

    if (method == "GET" && path == "/check_force_logout") {
        bool shouldLogout = CheckAndClearForceLogoutFlag();
        std::string response = shouldLogout ? "{\"forceLogout\":true}" : "{\"forceLogout\":false}";
        SendJsonResponse(client, "HTTP/1.1 200 OK", response);
        return RequestHandleResult::kHandledClose;
    }

    if (method == "POST" && path == "/set_ui_state") {
        if (!IsRequestAuthorized(req, clientIP)) {
            SendUnauthorized(client);
            return RequestHandleResult::kHandledClose;
        }

        try {
            if (g_hwndHttpTarget) {
                MainWindow::PostUTF8LogToHWND(g_hwndHttpTarget, body);
            }
        } catch (...) {
        }

        SendNoContent(client);
        return RequestHandleResult::kHandledClose;
    }

    if (method == "POST" &&
        (path == "/connect" || path == "/clear_status" || path == "/toggle_status" ||
         path == "/dump_esi" || path == "/browse_esi" || path == "/upload_esi" ||
         path == "/send_mailbox" || path == "/disconnect" || path == "/set_adapter" ||
         path == "/auto_connect" || path == "/start_auto" || path == "/stop_auto" ||
         path == "/set_period")) {
        if (!IsRequestAuthorized(req, clientIP)) {
            SendUnauthorized(client);
            return RequestHandleResult::kHandledClose;
        }

        if (!CheckRateLimit(clientIP)) {
            const char* rateLimit = "HTTP/1.1 429 Too Many Requests\r\nContent-Length: 0\r\n\r\n";
            SendAll(client, rateLimit);
            return RequestHandleResult::kHandledClose;
        }

        if (path == "/connect") {
            const auto snapshot = GetHttpTargetSystemStateSnapshot();
            std::string webIf;
            if (g_hwndHttpTarget && IsWindow(g_hwndHttpTarget)) {
                auto* win = reinterpret_cast<MainWindow*>(GetWindowLongPtr(g_hwndHttpTarget, GWLP_USERDATA));
                if (win) {
                    webIf = win->GetWebSelectedAdapterUtf8();
                }
            }

            ApiCommandAction action =
                BuildConnectAction(snapshot.busy, snapshot.ethercat_ok, webIf);
            if (!action.command.empty()) {
                PostCommandToUIOrCache(action.command);
            }
            if (action.responseKind == ApiResponseKind::kJson) {
                SendJsonResponse(client, action.responseStatusLine, action.responseBody);
                return RequestHandleResult::kHandledClose;
            }
            SendNoContent(client);
            return RequestHandleResult::kHandledClose;
        } else if (path == "/set_adapter") {
            try {
                std::string ifname = TrimHttpValue(body);
                if (!ifname.empty()) {
                    if (g_hwndHttpTarget && IsWindow(g_hwndHttpTarget)) {
                        auto* win = reinterpret_cast<MainWindow*>(
                            GetWindowLongPtr(g_hwndHttpTarget, GWLP_USERDATA));
                        if (win) {
                            win->SetSelectedAdapter(ifname, true);
                            const bool simulationSelected = EtherCATMaster::IsSimulationAdapterSelection(ifname);
                            bool linkUp = simulationSelected ? true : CoreHelpers::CheckLinkStatus(ifname.c_str());
                            bool disabled = true;
                            const auto snapshot = win->GetSystemStateSnapshot();
                            if (linkUp && !snapshot.busy && !win->IsMasterInitialized()) {
                                disabled = false;
                            }
                            SetUiDisabled("connect", disabled);
                            try {
                                std::string ast = std::string(simulationSelected ? "simulation:" : (linkUp ? "link_up:" : "no_link:")) + ifname;
                                SetAdapterStatus(ast);
                            } catch (...) {
                            }
                        }
                    }
                } else {
                    SetUiDisabled("connect", true);
                }
            } catch (...) {
            }
            SendNoContent(client);
            return RequestHandleResult::kHandledClose;
        } else if (path == "/auto_connect") {
            const auto snapshot = GetHttpTargetSystemStateSnapshot();
            ApiCommandAction action = BuildAutoConnectAction(snapshot);
            if (!action.command.empty()) {
                PostCommandToUIOrCache(action.command);
            }
            if (action.responseKind == ApiResponseKind::kJson) {
                SendJsonResponse(client, action.responseStatusLine, action.responseBody);
                return RequestHandleResult::kHandledClose;
            }
            SendNoContent(client);
            return RequestHandleResult::kHandledClose;
        } else if (path == "/upload_esi") {
            std::string filename = GetHeaderValue(req, "x-filename");
            if (filename.empty()) {
                const char* bad = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
                SendAll(client, bad);
                return RequestHandleResult::kHandledClose;
            }

            std::string exeDir = GetExecutableDirA();
            std::string outpath;
            if (!exeDir.empty()) {
                std::string uploadDir = exeDir + "uploads";
                CreateDirectoryA(uploadDir.c_str(), NULL);
                size_t pos = filename.find_last_of("\\/");
                std::string base = (pos == std::string::npos) ? filename : filename.substr(pos + 1);
                if (base.find('.') == std::string::npos) {
                    base += ".xml";
                }
                std::string uniq = std::to_string(GetTickCount());
                outpath = uploadDir + "\\" + uniq + "_" + base;
            } else {
                outpath = filename;
            }

            FILE* f = nullptr;
            errno_t er = fopen_s(&f, outpath.c_str(), "wb");
            if (er != 0 || !f) {
                const char* errh =
                    "HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
                SendAll(client, errh);
                return RequestHandleResult::kHandledClose;
            }

            fwrite(body.data(), 1, body.size(), f);
            fclose(f);
            PostCommandToUIOrCache(std::string("HTTP_CMD:LOAD_ESI:") + outpath);
            SendNoContent(client);
            return RequestHandleResult::kHandledClose;
        } else {
            ApiCommandAction action = BuildApiCommandAction(path, body);
            if (action.handled) {
                if (action.shouldCacheMailbox) {
                    SetMailboxFromHttp(action.mailboxUtf8);
                }

                if (action.shouldUpdateDesiredCycle) {
                    {
                        std::lock_guard<std::mutex> clk(g_cycleMutex);
                        g_desiredCycleTimeUs = action.desiredCycleUs;
                    }

                    g_ignoreWin32EchoUntil =
                        std::chrono::system_clock::now() + std::chrono::milliseconds(3000);
                }

                if (!action.command.empty()) {
                    PostCommandToUIOrCache(action.command);
                }

                if (action.responseKind == ApiResponseKind::kNoContent) {
                    SendNoContent(client);
                    return RequestHandleResult::kHandledClose;
                }
            }
        }

        SendNoContent(client);
        return RequestHandleResult::kHandledClose;
    }

    if (method == "GET" && path == "/api/get-key") {
        if (!IsRequestAuthorized(req, clientIP)) {
            SendUnauthorized(client);
            return RequestHandleResult::kHandledClose;
        }

        std::string jsonResp = "{\"api_key\":\"" + g_httpApiKey + "\"}";
        SendJsonResponse(client, "HTTP/1.1 200 OK", jsonResp);
        return RequestHandleResult::kHandledClose;
    }

    if (method == "GET" && path == "/api/mmf/status") {
        // Polled by the web UI during page load. Read the master's atomic MMF-output
        // state instead of sending WM_USER+250 to the main window: a blocking
        // SendMessageW from this HTTP worker would stall (or deadlock) while the UI
        // thread is not pumping messages, e.g. while MainWindow::Create() is still
        // running (repo-facts 2026-10-03).
        bool mmfOutputEnabled = false;
        if (auto* master = AppController::GetInstance().GetMaster()) {
            mmfOutputEnabled = master->IsMMFOutputEnabled();
        }
        SendJsonResponse(client, "HTTP/1.1 200 OK",
                         mmfOutputEnabled ? "{\"enabled\":true}" : "{\"enabled\":false}");
        return RequestHandleResult::kHandledClose;
    }

    if (method == "GET" && path == "/api/mmf/input_mode") {
        if (!IsRequestAuthorized(req, clientIP)) {
            SendUnauthorized(client);
            return RequestHandleResult::kHandledClose;
        }

        const Mmf1InputMode activeMode = AppController::GetInstance().GetMmf1InputMode();
        Mmf1InputMode savedMode = activeMode;
        const bool configLoaded = AppController::GetInstance().TryLoadSavedMmf1InputMode(savedMode);
        SendJsonResponse(
            client,
            "HTTP/1.1 200 OK",
            BuildMmf1InputModeConfigJson(activeMode, savedMode, configLoaded));
        return RequestHandleResult::kHandledClose;
    }

    if (method == "POST" && path == "/api/mmf/input_mode") {
        if (!IsRequestAuthorized(req, clientIP)) {
            SendUnauthorized(client);
            return RequestHandleResult::kHandledClose;
        }

        Mmf1InputMode requestedMode = Mmf1InputMode::ServoAngle;
        if (!TryParseMmf1InputModeFromBody(body, requestedMode)) {
            SendJsonResponse(
                client,
                "HTTP/1.1 400 Bad Request",
                "{\"error\":\"invalid MMF1 input mode request\"}");
            return RequestHandleResult::kHandledClose;
        }

        if (!AppController::GetInstance().SaveMmf1InputModeConfig(requestedMode)) {
            SendJsonResponse(
                client,
                "HTTP/1.1 500 Internal Server Error",
                "{\"error\":\"failed to save MMF1 input mode to machine.xml\"}");
            return RequestHandleResult::kHandledClose;
        }

        const Mmf1InputMode activeMode = AppController::GetInstance().GetMmf1InputMode();
        SendJsonResponse(
            client,
            "HTTP/1.1 200 OK",
            BuildMmf1InputModeConfigJson(activeMode, requestedMode, true));
        return RequestHandleResult::kHandledClose;
    }

    if (method == "GET" && path == "/api/machine_config/xml") {
        if (!IsRequestAuthorized(req, clientIP)) {
            SendUnauthorized(client);
            return RequestHandleResult::kHandledClose;
        }

        std::string xmlContent;
        std::string errorMessage;
        if (!AppController::GetInstance().LoadMachineConfigXml(xmlContent, &errorMessage)) {
            SendJsonResponse(
                client,
                "HTTP/1.1 500 Internal Server Error",
                BuildErrorJson(errorMessage.empty() ? "failed to load machine.xml" : errorMessage));
            return RequestHandleResult::kHandledClose;
        }

        SendContentResponse(client,
                            "HTTP/1.1 200 OK",
                            "application/xml; charset=utf-8",
                            xmlContent);
        return RequestHandleResult::kHandledClose;
    }

    if (method == "POST" && path == "/api/machine_config/xml") {
        if (!IsRequestAuthorized(req, clientIP)) {
            SendUnauthorized(client);
            return RequestHandleResult::kHandledClose;
        }

        if (body.empty()) {
            SendJsonResponse(client,
                             "HTTP/1.1 400 Bad Request",
                             BuildErrorJson("machine.xml content is empty"));
            return RequestHandleResult::kHandledClose;
        }

        std::string errorMessage;
        if (!AppController::GetInstance().SaveMachineConfigXml(body, &errorMessage)) {
            SendJsonResponse(
                client,
                IsMachineConfigValidationError(errorMessage)
                    ? "HTTP/1.1 400 Bad Request"
                    : "HTTP/1.1 500 Internal Server Error",
                BuildErrorJson(errorMessage.empty() ? "failed to save machine.xml" : errorMessage));
            return RequestHandleResult::kHandledClose;
        }

        SendJsonResponse(client, "HTTP/1.1 200 OK", "{\"saved\":true}");
        return RequestHandleResult::kHandledClose;
    }

    if (method == "GET" && path == "/api/stewart/native/status") {
        if (!IsRequestAuthorized(req, clientIP)) {
            SendUnauthorized(client);
            return RequestHandleResult::kHandledClose;
        }

        ::StartStewartNativePlanner();
        SendJsonResponse(client, "HTTP/1.1 200 OK", ::GetStewartNativePlannerStateJson(true));
        return RequestHandleResult::kHandledClose;
    }

    if (method == "POST" && path == "/api/stewart/native/target") {
        if (!IsRequestAuthorized(req, clientIP)) {
            SendUnauthorized(client);
            return RequestHandleResult::kHandledClose;
        }

        StewartParams params;
        Pose6D pose;
        if (!TryParseStewartParamsFromBody(body, params) ||
            !TryParsePoseFromBody(body, "pose", pose)) {
            SendJsonResponse(
                client,
                "HTTP/1.1 400 Bad Request",
                "{\"error\":\"invalid Stewart native target request\"}");
            return RequestHandleResult::kHandledClose;
        }

        long double seqValue = 0;
        long double linearSpeedMmPerSec = 600.0L;
        long double angularSpeedDegPerSec = 120.0L;
        bool snapToTarget = false;
        (void)TryParseJsonLongDouble(body, "seq", seqValue);
        (void)TryParseJsonLongDouble(body, "linearSpeedMmPerSec", linearSpeedMmPerSec);
        (void)TryParseJsonLongDouble(body, "angularSpeedDegPerSec", angularSpeedDegPerSec);
        (void)TryParseJsonBool(body, "snapToTarget", snapToTarget);

        ::StartStewartNativePlanner();
        ::SubmitStewartNativeTarget(
            seqValue > 0 ? static_cast<uint64_t>(seqValue) : 0,
            params,
            pose,
            linearSpeedMmPerSec,
            angularSpeedDegPerSec,
            snapToTarget);
        SendJsonResponse(client, "HTTP/1.1 200 OK", ::GetStewartNativePlannerStateJson(true));
        return RequestHandleResult::kHandledClose;
    }

    if (method == "POST" && path == "/api/stewart/native/stop") {
        if (!IsRequestAuthorized(req, clientIP)) {
            SendUnauthorized(client);
            return RequestHandleResult::kHandledClose;
        }

        ::HoldStewartNativeTarget();
        SendJsonResponse(client, "HTTP/1.1 200 OK", ::GetStewartNativePlannerStateJson(true));
        return RequestHandleResult::kHandledClose;
    }

    if (method == "POST" && path == "/api/stewart/inverse") {
        if (!IsRequestAuthorized(req, clientIP)) {
            SendUnauthorized(client);
            return RequestHandleResult::kHandledClose;
        }

        StewartParams params;
        StewartWebSettings settings;
        Pose6D inputPose;
        if (!TryParseStewartParamsFromBody(body, params) ||
            !TryParseStewartWebSettingsFromBody(body, settings) ||
            !TryParsePoseFromBody(body, "inputPose", inputPose)) {
            SendJsonResponse(
                client,
                "HTTP/1.1 400 Bad Request",
                "{\"error\":\"invalid Stewart inverse request\"}");
            return RequestHandleResult::kHandledClose;
        }

        const StewartResolvedPoseInput resolvedPose = ResolveStewartPoseInput(
            params,
            settings,
            ParseStewartInputCoordinateSystem(body),
            inputPose);
        // Pose6D 前置驗證層：使用從 machine.xml 載入的全域硬限制值
        const auto pose6dLimits = GetStewartPose6DLimits();
        const auto poseValidation = StewartIK::ValidatePose6D(
            resolvedPose.worldPose, resolvedPose.initialHeight, pose6dLimits);
        const IKResult result = StewartIK::CalculateInverse(params, poseValidation.clampedPose);
        const StewartDiagnostics diagnostics = AnalyzeStewartResult(params, settings, result);
        SendJsonResponse(
            client,
            "HTTP/1.1 200 OK",
            BuildInverseKinematicsJson(result, resolvedPose, diagnostics, poseValidation));
        return RequestHandleResult::kHandledClose;
    }

    if (method == "POST" && path == "/api/stewart/inverse/stream") {
        if (!IsRequestAuthorized(req, clientIP)) {
            SendUnauthorized(client);
            return RequestHandleResult::kHandledClose;
        }

        StewartParams params;
        StewartWebSettings settings;
        Pose6D inputPose;
        Pose6D worldPose;
        long double seqValue = 0;
        const bool hasInputPose = TryParsePoseFromBody(body, "inputPose", inputPose);
        const bool hasLegacyWorldPose = !hasInputPose && TryParsePoseFromBody(body, "pose", worldPose);
        if (!TryParseStewartParamsFromBody(body, params) ||
            !TryParseStewartWebSettingsFromBody(body, settings) ||
            !TryParseJsonLongDouble(body, "seq", seqValue) ||
            (!hasInputPose && !hasLegacyWorldPose)) {
            SendJsonResponse(
                client,
                "HTTP/1.1 400 Bad Request",
                "{\"error\":\"invalid Stewart inverse stream request\"}");
            return RequestHandleResult::kHandledClose;
        }

        StewartResolvedPoseInput resolvedPose;
        if (hasInputPose) {
            resolvedPose = ResolveStewartPoseInput(
                params,
                settings,
                ParseStewartInputCoordinateSystem(body),
                inputPose);
        } else {
            resolvedPose.initialHeight = StewartIK::CalculateInitialHeight(params, settings.servoInitialAngle);
            resolvedPose.worldPose = worldPose;
            resolvedPose.platformPose = BuildPlatformPoseFromWorldPose(worldPose, resolvedPose.initialHeight);
        }

        const uint64_t seq = seqValue > 0 ? static_cast<uint64_t>(seqValue) : 0;
        QueueStewartInverseStreamRequest(
            seq,
            clientIP,
            params,
            resolvedPose.worldPose,
            resolvedPose.platformPose,
            resolvedPose.initialHeight);
        SendJsonResponse(client, "HTTP/1.1 200 OK", "{\"queued\":true}");
        return RequestHandleResult::kHandledClose;
    }

    if (method == "POST" && path == "/api/stewart/forward/stream") {
        if (!IsRequestAuthorized(req, clientIP)) {
            SendUnauthorized(client);
            return RequestHandleResult::kHandledClose;
        }

        StewartParams params;
        StewartWebSettings settings;
        Pose6D initialPose;
        std::array<long double, 6> servoAngles{};
        long double seqValue = 0;
        if (!TryParseStewartParamsFromBody(body, params) ||
            !TryParseStewartWebSettingsFromBody(body, settings) ||
            !TryParsePoseFromBody(body, "initialPose", initialPose) ||
            !TryParseJsonLongDoubleArray6(body, "inputServoAngles", servoAngles) ||
            !TryParseJsonLongDouble(body, "seq", seqValue)) {
            SendJsonResponse(
                client,
                "HTTP/1.1 400 Bad Request",
                "{\"error\":\"invalid Stewart forward stream request\"}");
            return RequestHandleResult::kHandledClose;
        }

        const uint64_t seq = seqValue > 0 ? static_cast<uint64_t>(seqValue) : 0;
        QueueStewartForwardStreamRequest(
            seq,
            clientIP,
            params,
            settings,
            initialPose,
            servoAngles);
        SendJsonResponse(client, "HTTP/1.1 200 OK", "{\"queued\":true}");
        return RequestHandleResult::kHandledClose;
    }

    if (method == "POST" && path == "/api/stewart/forward") {
        if (!IsRequestAuthorized(req, clientIP)) {
            SendUnauthorized(client);
            return RequestHandleResult::kHandledClose;
        }

        StewartParams params;
        StewartWebSettings settings;
        Pose6D initialPose;
        std::array<long double, 6> servoAngles{};
        if (!TryParseStewartParamsFromBody(body, params) ||
            !TryParseStewartWebSettingsFromBody(body, settings) ||
            !TryParsePoseFromBody(body, "initialPose", initialPose) ||
            !TryParseJsonLongDoubleArray6(body, "inputServoAngles", servoAngles)) {
            SendJsonResponse(
                client,
                "HTTP/1.1 400 Bad Request",
                "{\"error\":\"invalid Stewart forward request\"}");
            return RequestHandleResult::kHandledClose;
        }

        const ForwardResult forwardResult =
            StewartIK::CalculateForward(params, servoAngles, initialPose);
        const auto armEndPoints = ComputeArmEndPoints(params, servoAngles);

        std::array<std::array<long double, 3>, 6> platformPoints{};
        if (forwardResult.valid) {
            const IKResult geometryResult = StewartIK::CalculateInverse(params, forwardResult.pose);
            platformPoints = geometryResult.platformPoints;
        }

        StewartResolvedPoseInput resolvedPose;
        resolvedPose.initialHeight = StewartIK::CalculateInitialHeight(params, settings.servoInitialAngle);
        resolvedPose.worldPose = forwardResult.pose;
        resolvedPose.platformPose = BuildPlatformPoseFromWorldPose(forwardResult.pose, resolvedPose.initialHeight);

        StewartDiagnostics diagnostics;
        diagnostics.initialHeight = resolvedPose.initialHeight;
        diagnostics.basePoints = StewartIK::CalculateSymmetricHexagonPoints(
            params.baseEdgeA,
            params.baseEdgeB,
            params.baseHeight);
        if (forwardResult.valid) {
            IKResult diagnosticsResult;
            diagnosticsResult.valid = true;
            diagnosticsResult.servoAngles = servoAngles;
            diagnosticsResult.armEndPoints = armEndPoints;
            diagnosticsResult.platformPoints = platformPoints;
            diagnosticsResult.errors = forwardResult.errors;
            diagnostics = AnalyzeStewartResult(params, settings, diagnosticsResult);
        }

        const std::vector<int> errorArms = ExtractErrorArmsFromErrors(forwardResult.errors);
        SendJsonResponse(
            client,
            "HTTP/1.1 200 OK",
            BuildForwardKinematicsJson(
                forwardResult,
                resolvedPose,
                servoAngles,
                armEndPoints,
                platformPoints,
                diagnostics,
                errorArms));
        return RequestHandleResult::kHandledClose;
    }

    if (method == "POST" && path == "/api/mmf/enable") {
        if (!IsRequestAuthorized(req, clientIP)) {
            SendUnauthorized(client);
            return RequestHandleResult::kHandledClose;
        }

        ApiCommandAction action = BuildApiCommandAction(path, body);
        if (!action.command.empty()) {
            PostCommandToUIOrCache(action.command);
        }
        SendJsonResponse(client, action.responseStatusLine, action.responseBody);
        return RequestHandleResult::kHandledClose;
    }

    if (method == "POST" && path == "/api/mmf/disable") {
        if (!IsRequestAuthorized(req, clientIP)) {
            SendUnauthorized(client);
            return RequestHandleResult::kHandledClose;
        }

        ApiCommandAction action = BuildApiCommandAction(path, body);
        if (!action.command.empty()) {
            PostCommandToUIOrCache(action.command);
        }
        SendJsonResponse(client, action.responseStatusLine, action.responseBody);
        return RequestHandleResult::kHandledClose;
    }

    if (method == "POST" && path.rfind("/auto/", 0) == 0) {
        if (!IsRequestAuthorized(req, clientIP)) {
            SendUnauthorized(client);
            return RequestHandleResult::kHandledClose;
        }

        if (!CheckRateLimit(clientIP)) {
            const char* rateLimit = "HTTP/1.1 429 Too Many Requests\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
            SendAll(client, rateLimit);
            return RequestHandleResult::kHandledClose;
        }

        if (!g_hwndHttpTarget || !IsWindow(g_hwndHttpTarget)) {
            const char* unavail =
                "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
            SendAll(client, unavail);
            return RequestHandleResult::kHandledClose;
        }

        const auto snapshot = GetHttpTargetSystemStateSnapshot();
        ApiCommandAction windowAction = BuildAutoWindowMessageAction(path, snapshot);
        if (windowAction.handled) {
            if (windowAction.responseKind == ApiResponseKind::kJson) {
                SendJsonResponse(client, windowAction.responseStatusLine, windowAction.responseBody);
                return RequestHandleResult::kHandledClose;
            }
            if (windowAction.postedWindowMessage != 0) {
                PostMessageW(g_hwndHttpTarget, windowAction.postedWindowMessage,
                             static_cast<WPARAM>(0), 0);
            }
            SendNoContent(client);
            return RequestHandleResult::kHandledClose;
        }

        if (path == "/auto/fault_reset") {
            const auto snapshot = GetHttpTargetSystemStateSnapshot();
            ApiCommandAction action = BuildAutoFaultResetAction(snapshot);
            if (!action.command.empty()) {
                PostCommandToUIOrCache(action.command);
            }
            if (action.responseKind == ApiResponseKind::kJson) {
                SendJsonResponse(client, action.responseStatusLine, action.responseBody);
                return RequestHandleResult::kHandledClose;
            }
        } else if (path == "/auto/down_to_origin") {
            const auto snapshot = GetHttpTargetSystemStateSnapshot();
            ApiCommandAction action = BuildAutoDownToOriginAction(snapshot);
            if (!action.command.empty()) {
                PostCommandToUIOrCache(action.command);
            }
            if (action.responseKind == ApiResponseKind::kJson) {
                SendJsonResponse(client, action.responseStatusLine, action.responseBody);
                return RequestHandleResult::kHandledClose;
            }
        }

        SendNoContent(client);
        return RequestHandleResult::kHandledClose;
    }

    if (method == "POST" && path == "/select_slave") {
        if (!IsRequestAuthorized(req, clientIP)) {
            SendUnauthorized(client);
            return RequestHandleResult::kHandledClose;
        }

        try {
            ApiCommandAction action = BuildApiCommandAction(path, body);
            if (!action.command.empty()) {
                PostCommandToUIOrCache(action.command);
            }
        } catch (...) {
        }

        SendNoContent(client);
        return RequestHandleResult::kHandledClose;
    }

    if (method == "POST" && path == "/read_sdo") {
        if (!IsRequestAuthorized(req, clientIP)) {
            SendUnauthorized(client);
            return RequestHandleResult::kHandledClose;
        }
        ApiCommandAction action = BuildApiCommandAction(path, body);
        if (!action.command.empty()) {
            PostCommandToUIOrCache(action.command);
        }
        SendNoContent(client);
        return RequestHandleResult::kHandledClose;
    }

    if (method == "POST" && path == "/write_sdo") {
        if (!IsRequestAuthorized(req, clientIP)) {
            SendUnauthorized(client);
            return RequestHandleResult::kHandledClose;
        }
        ApiCommandAction action = BuildApiCommandAction(path, body);
        if (!action.command.empty()) {
            PostCommandToUIOrCache(action.command);
        }
        SendNoContent(client);
        return RequestHandleResult::kHandledClose;
    }

    if (method == "POST" && path == "/read_all_sdo") {
        if (!IsRequestAuthorized(req, clientIP)) {
            SendUnauthorized(client);
            return RequestHandleResult::kHandledClose;
        }
        ApiCommandAction action = BuildApiCommandAction(path, body);
        if (!action.command.empty()) {
            PostCommandToUIOrCache(action.command);
        }
        SendNoContent(client);
        return RequestHandleResult::kHandledClose;
    }

    if (method == "POST" && path == "/write_all_sdo") {
        if (!IsRequestAuthorized(req, clientIP)) {
            SendUnauthorized(client);
            return RequestHandleResult::kHandledClose;
        }
        ApiCommandAction action = BuildApiCommandAction(path, body);
        if (!action.command.empty()) {
            PostCommandToUIOrCache(action.command);
        }
        SendNoContent(client);
        return RequestHandleResult::kHandledClose;
    }

    return RequestHandleResult::kNotHandled;
}

}  // namespace HttpServerInternal
