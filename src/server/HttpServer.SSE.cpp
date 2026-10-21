// HttpServer.SSE.cpp - Server-Sent Events 廣播。

#include <winsock2.h>

#include <array>
#include <chrono>
#include <cctype>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "HttpServer.h"
#include "EtherCATMaster.h"
#include "MainWindow.h"
#include "../app/AppController.h"
#include "HttpServer.Internal.h"

using namespace HttpServerInternal;

namespace {

CoreHelpers::EventSubscription g_sseStatusSubscription;
constexpr int kSseNoClientWaitMs = 200;
constexpr int kSseConnectedIdleWaitMs = 50;
constexpr int kSseIdleHeartbeatMs = 250;
constexpr uint16_t kErrorCode603FReadFailureSentinel = 0xFFFF;

const char* GetActiveMmf1InputModeJsonValue() {
    return AppController::GetInstance().GetMmf1InputMode() == Mmf1InputMode::Pose6D
               ? "Pose6D"
               : "ServoAngle";
}

CoreHelpers::AutoActionButtonInputs BuildAutoActionButtonInputs(MainWindow* win) {
    CoreHelpers::AutoActionButtonInputs inputs;
    if (win) {
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

void AppendAutoActionButtonStateJson(std::string& json,
                                     const CoreHelpers::AutoActionButtonState& state) {
    json += ",\"auto_connect\":";
    json += (state.connect_enabled ? "false" : "true");
    json += ",\"fault_reset\":";
    json += (state.fault_reset_enabled ? "false" : "true");
    json += ",\"down_to_origin\":";
    json += (state.down_to_origin_enabled ? "false" : "true");
    json += ",\"immediate_stop\":";
    json += (state.immediate_stop_enabled ? "false" : "true");
    json += ",\"emergency_stop\":";
    json += (state.emergency_stop_enabled ? "false" : "true");
    json += ",\"auto_connect_lamp\":";
    json += (state.connect_lit ? "true" : "false");
    json += ",\"down_to_origin_lamp\":";
    json += (state.down_to_origin_lit ? "true" : "false");
    json += ",\"fault_reset_lamp\":";
    json += (state.fault_reset_lit ? "true" : "false");
    json += ",\"immediate_stop_lamp\":";
    json += (state.immediate_stop_lit ? "true" : "false");
    json += ",\"emergency_stop_lamp\":";
    json += (state.emergency_stop_lit ? "true" : "false");
}

void WakeGeneralSseBroadcaster() {
    g_sseWakeCv.notify_one();
}

bool HasGeneralSseClients() {
    std::lock_guard<std::mutex> lk(g_sseClientsMutex);
    return !g_sseClients.empty();
}

bool IsSameSystemSnapshot(const CoreHelpers::SystemStateSnapshot& lhs,
                          const CoreHelpers::SystemStateSnapshot& rhs) {
    return lhs.ethercat_ok == rhs.ethercat_ok &&
           lhs.busy == rhs.busy &&
           lhs.error == rhs.error &&
           lhs.manual_reset_required == rhs.manual_reset_required &&
           lhs.ready == rhs.ready &&
           lhs.home_done == rhs.home_done &&
           lhs.online == rhs.online &&
           lhs.master_initialized == rhs.master_initialized &&
           lhs.worker_running == rhs.worker_running;
}

void WakeStewartSseBroadcaster() {
    g_stewartSseWakeCv.notify_one();
}

bool HasPendingStewartSseRequest(uint64_t lastProcessedInverseSeq,
                                 uint64_t lastProcessedForwardSeq) {
    bool hasClients = false;
    {
        std::lock_guard<std::mutex> lk(g_stewartSseClientsMutex);
        hasClients = !g_stewartSseClients.empty();
    }
    if (!hasClients) {
        return false;
    }

    StewartInverseStreamRequest inverseRequest;
    {
        std::lock_guard<std::mutex> lk(g_stewartInverseStreamMutex);
        inverseRequest = g_stewartInverseStreamRequest;
    }

    if (inverseRequest.valid && inverseRequest.seq != 0 &&
        inverseRequest.seq != lastProcessedInverseSeq) {
        return true;
    }

    StewartForwardStreamRequest forwardRequest;
    {
        std::lock_guard<std::mutex> lk(g_stewartForwardStreamMutex);
        forwardRequest = g_stewartForwardStreamRequest;
    }

    return forwardRequest.valid && forwardRequest.seq != 0 &&
           forwardRequest.seq != lastProcessedForwardSeq;
}

std::vector<int> ExtractStewartErrorArms(const std::vector<std::string>& errors) {
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

void AppendPoint3(std::ostringstream& response, const std::array<long double, 3>& point) {
    response << "[" << static_cast<double>(point[0]) << "," << static_cast<double>(point[1])
             << "," << static_cast<double>(point[2]) << "]";
}

void AppendArray6(std::ostringstream& response, const std::array<long double, 6>& values) {
    response << "[";
    for (size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            response << ",";
        }
        response << static_cast<double>(values[index]);
    }
    response << "]";
}

void AppendPointArray6(
    std::ostringstream& response,
    const std::array<std::array<long double, 3>, 6>& points) {
    response << "[";
    for (size_t index = 0; index < points.size(); ++index) {
        if (index != 0) {
            response << ",";
        }
        AppendPoint3(response, points[index]);
    }
    response << "]";
}

void AppendPose(std::ostringstream& response, const WuSOEM::Pose6D& pose) {
    response << "{";
    response << "\"x\":" << static_cast<double>(pose.x);
    response << ",\"y\":" << static_cast<double>(pose.y);
    response << ",\"z\":" << static_cast<double>(pose.z);
    response << ",\"roll\":" << static_cast<double>(pose.roll);
    response << ",\"pitch\":" << static_cast<double>(pose.pitch);
    response << ",\"yaw\":" << static_cast<double>(pose.yaw);
    response << "}";
}

void AppendErrorList(std::ostringstream& response, const std::vector<std::string>& errors) {
    const auto escapeJsonString = [](const std::string& in) {
        std::string out;
        out.reserve(in.size());
        char buf[8];
        for (unsigned char c : in) {
            switch (c) {
                case '"':
                    out += "\\\"";
                    break;
                case '\\':
                    out += "\\\\";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                default:
                    if (c < 0x20) {
                        sprintf_s(buf, "\\u%04x", c);
                        out += buf;
                    } else {
                        out.push_back(static_cast<char>(c));
                    }
            }
        }
        return out;
    };

    response << "[";
    for (size_t index = 0; index < errors.size(); ++index) {
        if (index != 0) {
            response << ",";
        }
        response << "\"" << escapeJsonString(errors[index]) << "\"";
    }
    response << "]";
}

void AppendIntList(std::ostringstream& response, const std::vector<int>& values) {
    response << "[";
    for (size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            response << ",";
        }
        response << values[index];
    }
    response << "]";
}

void AddUniqueArmIndex(std::vector<int>& indices, int index) {
    if (index < 0 || index >= 6) {
        return;
    }
    if (std::find(indices.begin(), indices.end(), index) == indices.end()) {
        indices.push_back(index);
    }
}

std::array<long double, 6> ComputeRodLengths(const WuSOEM::IKResult& result) {
    std::array<long double, 6> rodLengths{};
    if (result.armEndPoints.size() != 6 || result.platformPoints.size() != 6) {
        return rodLengths;
    }

    for (size_t index = 0; index < result.armEndPoints.size(); ++index) {
        const long double dx = result.platformPoints[index][0] - result.armEndPoints[index][0];
        const long double dy = result.platformPoints[index][1] - result.armEndPoints[index][1];
        const long double dz = result.platformPoints[index][2] - result.armEndPoints[index][2];
        rodLengths[index] = std::sqrtl(dx * dx + dy * dy + dz * dz);
    }

    return rodLengths;
}

void ComputeFisheyeDiagnostics(
    const WuSOEM::StewartParams& params,
    const std::array<std::array<long double, 3>, 6>& basePoints,
    const WuSOEM::IKResult& result,
    std::array<long double, 6>& fisheyeAngles,
    std::vector<int>& fisheyeWarningArms,
    std::vector<std::string>& warnings) {
    if (result.armEndPoints.size() != 6 || result.platformPoints.size() != 6) {
        return;
    }

    for (size_t index = 0; index < result.armEndPoints.size(); ++index) {
        const auto& basePoint = basePoints[index];
        const auto& armEndPoint = result.armEndPoints[index];
        const auto& platformPoint = result.platformPoints[index];
        const long double armVecX = armEndPoint[0] - basePoint[0];
        const long double armVecY = armEndPoint[1] - basePoint[1];
        const long double rodVecX = platformPoint[0] - armEndPoint[0];
        const long double rodVecY = platformPoint[1] - armEndPoint[1];
        const long double armLenXY = std::sqrtl(armVecX * armVecX + armVecY * armVecY);
        const long double rodLenXY = std::sqrtl(rodVecX * rodVecX + rodVecY * rodVecY);

        if (armLenXY <= 1e-9L || rodLenXY <= 1e-9L) {
            continue;
        }

        const long double dot = armVecX * rodVecX + armVecY * rodVecY;
        long double fisheyeAngle =
            acosl((std::max)(-1.0L, (std::min)(1.0L, dot / (armLenXY * rodLenXY)))) * 180.0L / acosl(-1.0L);
        if (fisheyeAngle > 90.0L) {
            fisheyeAngle = 180.0L - fisheyeAngle;
        }

        fisheyeAngles[index] = fisheyeAngle;
        if (fisheyeAngle > params.fisheyeAngleLimit) {
            AddUniqueArmIndex(fisheyeWarningArms, static_cast<int>(index));
            warnings.push_back(
                std::string(u8"力臂") + std::to_string(index + 1) +
                u8": 魚眼軸承角度 " + std::to_string(static_cast<double>(fisheyeAngle)) +
                u8"° 超過限制");
        }
    }
}

std::string BuildStewartInverseStreamPayload(
    const StewartInverseStreamRequest& request,
    const WuSOEM::IKResult& result) {
    const std::vector<int> errorArms = ExtractStewartErrorArms(result.errors);
    const auto basePoints = WuSOEM::StewartIK::CalculateSymmetricHexagonPoints(
        request.params.baseEdgeA,
        request.params.baseEdgeB,
        request.params.baseHeight);
    const auto rodLengths = ComputeRodLengths(result);
    std::array<long double, 6> fisheyeAngles{};
    std::vector<int> fisheyeWarningArms;
    std::vector<std::string> warnings;
    ComputeFisheyeDiagnostics(
        request.params,
        basePoints,
        result,
        fisheyeAngles,
        fisheyeWarningArms,
        warnings);

    std::ostringstream response;
    response << "{";
    response << "\"type\":\"stewartInverse\"";
    response << ",\"seq\":" << request.seq;
    response << ",\"valid\":" << (result.valid ? "true" : "false");
    response << ",\"worldPose\":";
    AppendPose(response, request.worldPose);
    response << ",\"platformPose\":";
    AppendPose(response, request.platformPose);
    response << ",\"initialHeight\":" << static_cast<double>(request.initialHeight);
    response << ",\"servoAngles\":";
    AppendArray6(response, result.servoAngles);
    response << ",\"armEndPoints\":";
    AppendPointArray6(response, result.armEndPoints);
    response << ",\"platformPoints\":";
    AppendPointArray6(response, result.platformPoints);
    response << ",\"basePoints\":";
    AppendPointArray6(response, basePoints);
    response << ",\"rodLengths\":";
    AppendArray6(response, rodLengths);
    response << ",\"fisheyeAngles\":";
    AppendArray6(response, fisheyeAngles);
    response << ",\"errors\":";
    AppendErrorList(response, result.errors);
    response << ",\"warnings\":";
    AppendErrorList(response, warnings);
    response << ",\"errorArms\":";
    AppendIntList(response, errorArms);
    response << ",\"warningArms\":";
    AppendIntList(response, fisheyeWarningArms);
    response << ",\"singularArms\":[]";
    response << ",\"fisheyeWarningArms\":";
    AppendIntList(response, fisheyeWarningArms);
    response << ",\"singularityInfo\":{}";
    response << ",\"platformSingularity\":false";
    response << "}";
    return response.str();
}

std::string BuildStewartForwardStreamPayload(const StewartForwardStreamRequest& request) {
    const WuSOEM::ForwardResult forwardResult =
        WuSOEM::StewartIK::CalculateForward(request.params, request.servoAngles, request.initialPose);
    const auto armEndPoints = ComputeArmEndPoints(request.params, request.servoAngles);

    std::array<std::array<long double, 3>, 6> platformPoints{};
    if (forwardResult.valid) {
        const WuSOEM::IKResult geometryResult =
            WuSOEM::StewartIK::CalculateInverse(request.params, forwardResult.pose);
        platformPoints = geometryResult.platformPoints;
    }

    StewartResolvedPoseInput resolvedPose;
    resolvedPose.initialHeight =
        WuSOEM::StewartIK::CalculateInitialHeight(request.params, request.settings.servoInitialAngle);
    resolvedPose.worldPose = forwardResult.pose;
    resolvedPose.platformPose =
        BuildPlatformPoseFromWorldPose(forwardResult.pose, resolvedPose.initialHeight);

    StewartDiagnostics diagnostics;
    diagnostics.initialHeight = resolvedPose.initialHeight;
    diagnostics.basePoints = WuSOEM::StewartIK::CalculateSymmetricHexagonPoints(
        request.params.baseEdgeA,
        request.params.baseEdgeB,
        request.params.baseHeight);
    if (forwardResult.valid) {
        WuSOEM::IKResult diagnosticsResult;
        diagnosticsResult.valid = true;
        diagnosticsResult.servoAngles = request.servoAngles;
        diagnosticsResult.armEndPoints = armEndPoints;
        diagnosticsResult.platformPoints = platformPoints;
        diagnosticsResult.errors = forwardResult.errors;
        diagnostics = AnalyzeStewartResult(request.params, request.settings, diagnosticsResult);
    }

    const std::vector<int> errorArms = ExtractErrorArmsFromErrors(forwardResult.errors);
    std::string json = BuildForwardKinematicsJson(
        forwardResult,
        resolvedPose,
        request.servoAngles,
        armEndPoints,
        platformPoints,
        diagnostics,
        errorArms);

    if (json.empty() || json.front() != '{') {
        return std::string("{\"type\":\"stewartForward\",\"seq\":") +
               std::to_string(request.seq) + "}";
    }

    std::ostringstream response;
    response << "{\"type\":\"stewartForward\",\"seq\":" << request.seq;
    if (json.size() > 1) {
        response << "," << json.substr(1);
    } else {
        response << "}";
    }
    return response.str();
}

long double ClampSignedDelta(long double current, long double target, long double step) {
    const long double delta = target - current;
    if (std::fabsl(delta) <= step) {
        return target;
    }
    return current + ((delta > 0.0L) ? step : -step);
}

long double StepAngleToward(long double current, long double target, long double step) {
    const long double delta = WuSOEM::StewartIK::NormalizeAngleDeg(target - current);
    if (std::fabsl(delta) <= step) {
        return WuSOEM::StewartIK::NormalizeAngleDeg(target);
    }
    return WuSOEM::StewartIK::NormalizeAngleDeg(current + ((delta > 0.0L) ? step : -step));
}

bool PoseNearEqual(const WuSOEM::Pose6D& lhs, const WuSOEM::Pose6D& rhs) {
    return std::fabsl(lhs.x - rhs.x) <= 1e-6L &&
           std::fabsl(lhs.y - rhs.y) <= 1e-6L &&
           std::fabsl(lhs.z - rhs.z) <= 1e-6L &&
           std::fabsl(WuSOEM::StewartIK::NormalizeAngleDeg(lhs.roll - rhs.roll)) <= 1e-6L &&
           std::fabsl(WuSOEM::StewartIK::NormalizeAngleDeg(lhs.pitch - rhs.pitch)) <= 1e-6L &&
           std::fabsl(WuSOEM::StewartIK::NormalizeAngleDeg(lhs.yaw - rhs.yaw)) <= 1e-6L;
}

WuSOEM::Pose6D AdvancePoseToward(
    const WuSOEM::Pose6D& current,
    const WuSOEM::Pose6D& target,
    long double linearStep,
    long double angularStep) {
    WuSOEM::Pose6D next = current;
    next.x = ClampSignedDelta(current.x, target.x, linearStep);
    next.y = ClampSignedDelta(current.y, target.y, linearStep);
    next.z = ClampSignedDelta(current.z, target.z, linearStep);
    next.roll = StepAngleToward(current.roll, target.roll, angularStep);
    next.pitch = StepAngleToward(current.pitch, target.pitch, angularStep);
    next.yaw = StepAngleToward(current.yaw, target.yaw, angularStep);
    return next;
}

std::string BuildStewartNativePlannerStatePayload(
    const StewartNativePlannerState& state,
    bool includeEventType) {
    const std::vector<int> errorArms = ExtractStewartErrorArms(state.lastResult.errors);
    std::ostringstream response;
    response << "{";
    if (includeEventType) {
        response << "\"type\":\"stewartNative\",";
    }
    response << "\"seq\":" << state.activeSeq;
    response << ",\"initialized\":" << (state.initialized ? "true" : "false");
    response << ",\"moving\":" << (state.moving ? "true" : "false");
    response << ",\"cycleCounter\":" << state.cycleCounter;
    response << ",\"updatePeriodUs\":" << state.updatePeriodUs;
    response << ",\"linearSpeedMmPerSec\":" << static_cast<double>(state.linearSpeedMmPerSec);
    response << ",\"angularSpeedDegPerSec\":" << static_cast<double>(state.angularSpeedDegPerSec);
    response << ",\"currentPose\":{";
    response << "\"x\":" << static_cast<double>(state.currentPose.x);
    response << ",\"y\":" << static_cast<double>(state.currentPose.y);
    response << ",\"z\":" << static_cast<double>(state.currentPose.z);
    response << ",\"roll\":" << static_cast<double>(state.currentPose.roll);
    response << ",\"pitch\":" << static_cast<double>(state.currentPose.pitch);
    response << ",\"yaw\":" << static_cast<double>(state.currentPose.yaw);
    response << "}";
    response << ",\"targetPose\":{";
    response << "\"x\":" << static_cast<double>(state.targetPose.x);
    response << ",\"y\":" << static_cast<double>(state.targetPose.y);
    response << ",\"z\":" << static_cast<double>(state.targetPose.z);
    response << ",\"roll\":" << static_cast<double>(state.targetPose.roll);
    response << ",\"pitch\":" << static_cast<double>(state.targetPose.pitch);
    response << ",\"yaw\":" << static_cast<double>(state.targetPose.yaw);
    response << "}";
    response << ",\"valid\":" << (state.lastResult.valid ? "true" : "false");
    response << ",\"servoAngles\":";
    AppendArray6(response, state.lastResult.servoAngles);
    response << ",\"armEndPoints\":";
    AppendPointArray6(response, state.lastResult.armEndPoints);
    response << ",\"platformPoints\":";
    AppendPointArray6(response, state.lastResult.platformPoints);
    response << ",\"errors\":";
    AppendErrorList(response, state.lastResult.errors);
    response << ",\"errorArms\":";
    AppendIntList(response, errorArms);
    response << "}";
    return response.str();
}

void BroadcastStewartSSE(const std::string& json) {
    try {
        std::lock_guard<std::mutex> lk(g_stewartSseClientsMutex);
        if (g_stewartSseClients.empty()) {
            return;
        }

        std::string msg = std::string("data: ") + json + "\r\n\r\n";
        for (auto it = g_stewartSseClients.begin(); it != g_stewartSseClients.end();) {
            try {
                SOCKET s = it->socket;
                const std::string& ip = it->ip;

                bool sessionValid = false;
                {
                    std::lock_guard<std::mutex> sessLk(g_sessionMutex);
                    const auto now = std::chrono::system_clock::now();
                    sessionValid = TryRefreshSessionByClientIP(ip, now);
                }

                if (!sessionValid) {
                    try {
                        closesocket(s);
                    } catch (...) {
                    }
                    try {
                        CloseClientAndDecrement(s);
                        PostHttpConnCountIfChanged();
                    } catch (...) {
                    }
                    it = g_stewartSseClients.erase(it);
                    continue;
                }

                if (!SendAll(s, msg)) {
                    try {
                        closesocket(s);
                    } catch (...) {
                    }
                    try {
                        CloseClientAndDecrement(s);
                        PostHttpConnCountIfChanged();
                    } catch (...) {
                    }
                    it = g_stewartSseClients.erase(it);
                } else {
                    ++it;
                }
            } catch (...) {
                try {
                    it = g_stewartSseClients.erase(it);
                } catch (...) {
                    ++it;
                }
            }
        }
    } catch (...) {
    }
}

}  // namespace

void BroadcastSSE(const std::string& json) {
    try {
        std::lock_guard<std::mutex> lk(g_sseClientsMutex);
        if (g_sseClients.empty()) {
            return;
        }

        std::string msg = std::string("data: ") + json + "\r\n\r\n";
        for (auto it = g_sseClients.begin(); it != g_sseClients.end();) {
            try {
                SOCKET s = it->socket;
                const std::string& ip = it->ip;

                bool sessionValid = false;
                {
                    std::lock_guard<std::mutex> sessLk(g_sessionMutex);
                    const auto now = std::chrono::system_clock::now();
                    sessionValid = TryRefreshSessionByClientIP(ip, now);
                }

                if (!sessionValid) {
                    try {
                        closesocket(s);
                    } catch (...) {
                    }
                    try {
                        CloseClientAndDecrement(s);
                        PostHttpConnCountIfChanged();
                    } catch (...) {
                    }
                    it = g_sseClients.erase(it);
                    continue;
                }

                if (!SendAll(s, msg)) {
                    try {
                        closesocket(s);
                    } catch (...) {
                    }
                    try {
                        CloseClientAndDecrement(s);
                        PostHttpConnCountIfChanged();
                    } catch (...) {
                    }
                    it = g_sseClients.erase(it);
                } else {
                    ++it;
                }
            } catch (const std::exception&) {
                try {
                    it = g_sseClients.erase(it);
                } catch (...) {
                    ++it;
                }
            } catch (...) {
                try {
                    it = g_sseClients.erase(it);
                } catch (...) {
                    ++it;
                }
            }
        }
    } catch (const std::exception&) {
    } catch (...) {
    }
}

void StartSseBroadcaster() {
    if (g_sseBroadcastRun.load()) {
        return;
    }

    g_sseBroadcastRun.store(true);
    g_sseStatusSubscription = AppController::GetInstance().SubscribeStatusEvent(
        [](const CoreHelpers::StatusEvent&) {
            WakeGeneralSseBroadcaster();
        });
    g_sseBroadcaster = std::thread([]() {
        try {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

        {
            DWORD_PTR processMask = 0;
            DWORD_PTR systemMask = 0;
            if (GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask)) {
            }
        }

        using namespace std::chrono;
        uint64_t seqCounter = 0;
        bool haveLastBroadcastState = false;
        CoreHelpers::SystemStateSnapshot lastSnapshot;
        bool lastMmfReady = false;
        bool lastMmfInMapped = false;
        bool lastMmfInRunning = false;
        uint64_t lastTs = 0;
        uint64_t lastEpoch = 0;
        uint32_t lastSlaveCount = 0;
        std::string lastInputMode;
        auto lastBroadcastAt = steady_clock::now() - milliseconds(kSseIdleHeartbeatMs);
        while (g_sseBroadcastRun.load()) {
            if (!HasGeneralSseClients()) {
                haveLastBroadcastState = false;
                std::unique_lock<std::mutex> waitLock(g_sseWakeMutex);
                g_sseWakeCv.wait_for(waitLock, milliseconds(kSseNoClientWaitMs));
                continue;
            }

            CoreHelpers::SystemStateSnapshot snapshot;
            MainWindow* targetWindow = nullptr;
            if (g_hwndHttpTarget && IsWindow(g_hwndHttpTarget)) {
                auto* win = reinterpret_cast<MainWindow*>(GetWindowLongPtr(g_hwndHttpTarget, GWLP_USERDATA));
                if (win) {
                    targetWindow = win;
                    snapshot = win->GetSystemStateSnapshot();
                }
            }

            EnsurePDOMapped();
            bool mmfReady = (g_pMmView && g_pMmView->magic == MMFWriter::MAGIC);
            bool mmfInMapped = false;
            bool mmfInRunning = false;

            if (g_hwndHttpTarget && IsWindow(g_hwndHttpTarget)) {
                auto* win = reinterpret_cast<MainWindow*>(GetWindowLongPtr(g_hwndHttpTarget, GWLP_USERDATA));
                if (win) {
                    mmfInMapped = win->MasterMMF1IsMapped();
                    mmfInRunning = win->MasterIsMMF1InRunning();
                }
            }

            uint64_t ts = 0;
            uint64_t epoch = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
            uint32_t slaveCount = 0;
            const char* inputMode = GetActiveMmf1InputModeJsonValue();

            if (mmfReady) {
                ts = g_pMmView->timestamp;
                epoch = g_pMmView->epochMs;
                slaveCount = g_pMmView->slaveCount;
            }

            const bool snapshotChanged =
                !haveLastBroadcastState || !IsSameSystemSnapshot(snapshot, lastSnapshot);
            const bool mmfStateChanged =
                !haveLastBroadcastState ||
                lastMmfReady != mmfReady ||
                lastMmfInMapped != mmfInMapped ||
                lastMmfInRunning != mmfInRunning ||
                lastTs != ts ||
                lastEpoch != epoch ||
                lastSlaveCount != slaveCount ||
                lastInputMode != inputMode;
            const bool fastPolling = snapshot.busy || snapshot.online || mmfInRunning ||
                                     (mmfReady && ts != lastTs);
            const auto now = steady_clock::now();
            const int activeIntervalMs = (g_sseBroadcastIntervalMs > 0)
                ? g_sseBroadcastIntervalMs
                : 10;
            const auto heartbeatInterval = milliseconds(
                fastPolling ? activeIntervalMs : kSseIdleHeartbeatMs);
            const bool heartbeatExpired =
                !haveLastBroadcastState || (now - lastBroadcastAt) >= heartbeatInterval;
            const bool shouldBroadcast =
                snapshotChanged || mmfStateChanged || fastPolling || heartbeatExpired;

            if (shouldBroadcast) {
                std::string payload = "{}";
                std::vector<MechanicalUnitMetrics> metrics(6);
                std::array<float, 6> mmfInValues = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
                std::array<double, 6> mmfInVelocities = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
                std::array<uint16_t, 6> errorCodes603F = {0, 0, 0, 0, 0, 0};
                std::array<bool, 6> errorCodeValid603F = {false, false, false, false, false, false};

                if (g_hwndHttpTarget && IsWindow(g_hwndHttpTarget)) {
                    auto* win = reinterpret_cast<MainWindow*>(GetWindowLongPtr(g_hwndHttpTarget, GWLP_USERDATA));
                    if (win) {
                        win->GetLastReportedErrorCodes(errorCodes603F, errorCodeValid603F);
                        for (int i = 0; i < 6; ++i) {
                            MechanicalUnitMetrics m;
                            if (win->MasterGetMechanicalUnitMetrics(i + 1, m) && m.valid) {
                                metrics[i] = m;
                            }
                        }
                        if (mmfInMapped && mmfInRunning) {
                            win->MasterGetMMF1LatestWithVelocities(mmfInValues, mmfInVelocities);
                        }
                    }
                }

                uint32_t observedCycleUs = 0;
                std::array<double, 6> filterInputAngles = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
                std::array<double, 6> filterInputVels = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
                if (auto* master = AppController::GetInstance().GetMaster()) {
                    observedCycleUs = master->GetObservedCycleTimeUs();
                    filterInputAngles = master->GetPDOFilterInputAngles();
                    filterInputVels = master->GetPDOFilterInputVelocities();  // precise, cyclic-computed
                }

                const uint64_t seqOut = ++seqCounter;

                // Pre-allocate JSON buffer to avoid ostringstream heap churn
                std::string json;
                json.reserve(4096);
                char numbuf[768];

                snprintf(numbuf, sizeof(numbuf),
                         "{\"seq\":%" PRIu64 ",\"ts\":%" PRIu64 ",\"epoch\":%" PRIu64 ",\"count\":%u",
                         seqOut, ts, epoch, (unsigned)slaveCount);
                json += numbuf;
                json += ",\"mmfReady\":"; json += (mmfReady ? "true" : "false");
                json += ",\"mmfInRunning\":"; json += (mmfInRunning ? "true" : "false");
                json += ",\"oEtherCatOk\":"; json += (snapshot.ethercat_ok ? "true" : "false");
                json += ",\"oBusy\":"; json += (snapshot.busy ? "true" : "false");
                json += ",\"oError\":"; json += (snapshot.error ? "true" : "false");
                json += ",\"oReady\":"; json += (snapshot.ready ? "true" : "false");
                json += ",\"oHomeDone\":"; json += (snapshot.home_done ? "true" : "false");
                json += ",\"oOnline\":"; json += (snapshot.online ? "true" : "false");
                json += ",\"mmf1InputMode\":\"";
                json += inputMode;
                json += "\"";
                if (observedCycleUs > 0) {
                    json += ",\"ecatCycleUs\":";
                    json += std::to_string(observedCycleUs);
                }

                {
                    const auto buttonState = CoreHelpers::EvaluateAutoActionButtons(
                        snapshot,
                        BuildAutoActionButtonInputs(targetWindow));
                    std::lock_guard<std::mutex> lk(g_uiStateMutex);
                    for (const auto& kv : g_uiDisabled) {
                        if (IsAutoActionUiStateKey(kv.first)) {
                            continue;
                        }
                        json += ",\"";
                        json += kv.first;
                        json += "\":";
                        json += (kv.second ? "true" : "false");
                    }
                    AppendAutoActionButtonStateJson(json, buttonState);
                }

                json += ",\"slaves\":[";

                for (uint32_t i = 0; i < 6; ++i) {
                    if (i > 0) {
                        json += ',';
                    }

                    // Narrow wstring metrics to stack char buffers (ASCII numerics only)
                    auto narrowMetric = [](const std::wstring& w, char* dst, size_t dstSize) {
                        size_t len = w.size();
                        if (len >= dstSize) len = dstSize - 1;
                        for (size_t j = 0; j < len; ++j)
                            dst[j] = static_cast<char>(w[j] & 0x7F);
                        dst[len] = '\0';
                    };
                    char tPosText[64], aPosText[64], tVelText[64], aVelText[64];
                    narrowMetric(metrics[i].targetPosition, tPosText, sizeof(tPosText));
                    narrowMetric(metrics[i].actualPosition, aPosText, sizeof(aPosText));
                    narrowMetric(metrics[i].targetSpeed, tVelText, sizeof(tVelText));
                    narrowMetric(metrics[i].actualSpeed, aVelText, sizeof(aVelText));

                    double tPosNum = 0.0;
                    double tVelNum = 0.0;
                    double aPosNum = 0.0;
                    double aVelNum = 0.0;
                    double filterOut = 0.0;
                    double filterVel = 0.0;
                    double filterAcc = 0.0;
                    uint16_t cw = 0;
                    uint16_t sw = 0;
                    double mmfIn = static_cast<double>(mmfInValues[i]);
                    double mmfInVel = mmfInVelocities[i];
                    const bool errorCodeValid = errorCodeValid603F[i];
                    const uint16_t errorCode603F = errorCodes603F[i];

                    if (mmfReady && i < slaveCount) {
                        const auto& s = g_pMmView->slaves[i];
                        tPosNum = s.targetPosition;
                        tVelNum = s.targetVelocity;
                        aPosNum = s.actualPosition;
                        aVelNum = s.actualVelocity;
                        filterOut = s.pdoFilterOutput;
                        filterVel = s.pdoFilterVel;
                        filterAcc = s.pdoFilterAcc;
                        cw = s.controlWord;
                        sw = s.statusWord;
                    }

                    int n = snprintf(numbuf, sizeof(numbuf),
                        "{\"tPosText\":\"%s\",\"aPosText\":\"%s\""
                        ",\"tVelText\":\"%s\",\"aVelText\":\"%s\""
                        ",\"tPos\":%g,\"tVel\":%g,\"aPos\":%g,\"aVel\":%g"
                        ",\"filterInput\":%g,\"filter\":%g,\"filterVel\":%g,\"filterAcc\":%g"
                        ",\"filterInputVel\":%g"
                        ",\"mmfIn\":%g,\"mmfInVel\":%g"
                        ",\"errorCode603F\":%s,\"errorCode603FValid\":%s,\"errorCode603FReadFailed\":%s"
                        ",\"cw\":%u,\"sw\":%u}",
                        tPosText, aPosText, tVelText, aVelText,
                        tPosNum, tVelNum, aPosNum, aVelNum,
                        filterInputAngles[i], filterOut, filterVel, filterAcc,
                        filterInputVels[i],
                        mmfIn, mmfInVel,
                        errorCodeValid ? std::to_string(errorCode603F).c_str() : "null",
                        errorCodeValid ? "true" : "false",
                        (errorCodeValid && errorCode603F == kErrorCode603FReadFailureSentinel) ? "true" : "false",
                        (unsigned)cw, (unsigned)sw);
                    json.append(numbuf, (n > 0 && n < (int)sizeof(numbuf)) ? (size_t)n : strlen(numbuf));
                }

                json += "]";

                // ---- PDO actual-angle → 6-DOF forward kinematics ----
                // Emit at JSON top level so RT_scope.html can read data.fwdKin.
                // Roll/Pitch swapped to match MMF convention: pose.roll(前後)→"pitch", pose.pitch(左右)→"roll".
                // Z emitted as relative height (world Z − initialHeight).
                {
                    bool fwdValid = false;
                    WuSOEM::Pose6D fwdPose;
                    long double fwdInitH = 450.0L;
                    if (auto* master = AppController::GetInstance().GetMaster()) {
                        master->GetForwardKinematicsResult(fwdPose, fwdValid, fwdInitH);
                    }
                    char fwdBuf[512];
                    int fwdN = snprintf(fwdBuf, sizeof(fwdBuf),
                        ",\"fwdKin\":{"
                        "\"valid\":%s"
                        ",\"x\":%.4f,\"y\":%.4f,\"z\":%.4f"
                        ",\"roll\":%.4f,\"pitch\":%.4f,\"yaw\":%.4f}",
                        fwdValid ? "true" : "false",
                        static_cast<double>(fwdPose.x),
                        static_cast<double>(fwdPose.y),
                        static_cast<double>(fwdPose.z - fwdInitH),
                        static_cast<double>(fwdPose.pitch),
                        static_cast<double>(fwdPose.roll),
                        static_cast<double>(fwdPose.yaw));
                    if (fwdN > 0 && fwdN < (int)sizeof(fwdBuf)) {
                        json.append(fwdBuf, static_cast<size_t>(fwdN));
                    }
                }

                json += "}";
                payload = json;
                ::BroadcastSSE(payload);

                lastSnapshot = snapshot;
                lastMmfReady = mmfReady;
                lastMmfInMapped = mmfInMapped;
                lastMmfInRunning = mmfInRunning;
                lastTs = ts;
                lastEpoch = epoch;
                lastSlaveCount = slaveCount;
                lastInputMode = inputMode;
                lastBroadcastAt = now;
                haveLastBroadcastState = true;
            }

            const int waitMs = fastPolling ? activeIntervalMs : kSseConnectedIdleWaitMs;
            std::unique_lock<std::mutex> waitLock(g_sseWakeMutex);
            g_sseWakeCv.wait_for(waitLock, milliseconds(waitMs));
        }
        } catch (const std::exception& e) {
            OutputDebugStringA((std::string("[SSE Broadcaster] exception: ") + e.what() + "\n").c_str());
        } catch (...) {
            OutputDebugStringA("[SSE Broadcaster] unknown exception\n");
        }
    });
}

void StopSseBroadcaster() {
    try {
        if (!g_sseBroadcastRun.load()) {
            return;
        }

        g_sseBroadcastRun.store(false);
        g_sseStatusSubscription.Reset();
        g_sseWakeCv.notify_all();
        if (g_sseBroadcaster.joinable()) {
            try {
                g_sseBroadcaster.join();
            } catch (...) {
            }
        }

        std::lock_guard<std::mutex> lk(g_sseClientsMutex);
        for (const auto& client : g_sseClients) {
            try {
                closesocket(client.socket);
            } catch (...) {
            }
            try {
                CloseClientAndDecrement(client.socket);
            } catch (...) {
            }
        }
        g_sseClients.clear();

        try {
            PostHttpConnCountIfChanged();
        } catch (...) {
        }
    } catch (...) {
    }
}

void QueueStewartInverseStreamRequest(
    uint64_t seq,
    const std::string& clientIP,
    const WuSOEM::StewartParams& params,
    const WuSOEM::Pose6D& worldPose,
    const WuSOEM::Pose6D& platformPose,
    long double initialHeight) {
    std::lock_guard<std::mutex> lk(g_stewartInverseStreamMutex);
    g_stewartInverseStreamRequest.valid = true;
    g_stewartInverseStreamRequest.seq = seq;
    g_stewartInverseStreamRequest.clientIP = clientIP;
    g_stewartInverseStreamRequest.params = params;
    g_stewartInverseStreamRequest.worldPose = worldPose;
    g_stewartInverseStreamRequest.platformPose = platformPose;
    g_stewartInverseStreamRequest.initialHeight = initialHeight;
    WakeStewartSseBroadcaster();
}

void QueueStewartForwardStreamRequest(
    uint64_t seq,
    const std::string& clientIP,
    const WuSOEM::StewartParams& params,
    const StewartWebSettings& settings,
    const WuSOEM::Pose6D& initialPose,
    const std::array<long double, 6>& servoAngles) {
    std::lock_guard<std::mutex> lk(g_stewartForwardStreamMutex);
    g_stewartForwardStreamRequest.valid = true;
    g_stewartForwardStreamRequest.seq = seq;
    g_stewartForwardStreamRequest.clientIP = clientIP;
    g_stewartForwardStreamRequest.params = params;
    g_stewartForwardStreamRequest.settings = settings;
    g_stewartForwardStreamRequest.initialPose = initialPose;
    g_stewartForwardStreamRequest.servoAngles = servoAngles;
    WakeStewartSseBroadcaster();
}

void SubmitStewartNativeTarget(
    uint64_t seq,
    const WuSOEM::StewartParams& params,
    const WuSOEM::Pose6D& targetPose,
    long double linearSpeedMmPerSec,
    long double angularSpeedDegPerSec,
    bool snapToTarget) {
    std::lock_guard<std::mutex> lk(g_stewartNativeMutex);
    g_stewartNativeTargetRequest.valid = true;
    g_stewartNativeTargetRequest.seq = seq;
    g_stewartNativeTargetRequest.params = params;
    g_stewartNativeTargetRequest.targetPose = targetPose;
    g_stewartNativeTargetRequest.linearSpeedMmPerSec =
        (linearSpeedMmPerSec > 1e-6L) ? linearSpeedMmPerSec : 600.0L;
    g_stewartNativeTargetRequest.angularSpeedDegPerSec =
        (angularSpeedDegPerSec > 1e-6L) ? angularSpeedDegPerSec : 120.0L;
    g_stewartNativeTargetRequest.snapToTarget = snapToTarget;

    if (!g_stewartNativePlannerState.initialized) {
        g_stewartNativePlannerState.initialized = true;
        g_stewartNativePlannerState.activeSeq = seq;
        g_stewartNativePlannerState.params = params;
        g_stewartNativePlannerState.currentPose = targetPose;
        g_stewartNativePlannerState.targetPose = targetPose;
        g_stewartNativePlannerState.moving = false;
        g_stewartNativePlannerState.linearSpeedMmPerSec =
            g_stewartNativeTargetRequest.linearSpeedMmPerSec;
        g_stewartNativePlannerState.angularSpeedDegPerSec =
            g_stewartNativeTargetRequest.angularSpeedDegPerSec;
        g_stewartNativePlannerState.lastResult =
            WuSOEM::StewartIK::CalculateInverse(params, targetPose);
    }
}

void HoldStewartNativeTarget() {
    std::lock_guard<std::mutex> lk(g_stewartNativeMutex);
    if (!g_stewartNativePlannerState.initialized) {
        return;
    }
    g_stewartNativeTargetRequest.valid = false;
    g_stewartNativeTargetRequest.seq = g_stewartNativePlannerState.activeSeq;
    g_stewartNativeTargetRequest.params = g_stewartNativePlannerState.params;
    g_stewartNativeTargetRequest.targetPose = g_stewartNativePlannerState.currentPose;
    g_stewartNativeTargetRequest.linearSpeedMmPerSec = g_stewartNativePlannerState.linearSpeedMmPerSec;
    g_stewartNativeTargetRequest.angularSpeedDegPerSec = g_stewartNativePlannerState.angularSpeedDegPerSec;
    g_stewartNativeTargetRequest.snapToTarget = true;
    g_stewartNativePlannerState.targetPose = g_stewartNativePlannerState.currentPose;
    g_stewartNativePlannerState.moving = false;
}

std::string GetStewartNativePlannerStateJson(bool includeEventType) {
    std::lock_guard<std::mutex> lk(g_stewartNativeMutex);
    return BuildStewartNativePlannerStatePayload(g_stewartNativePlannerState, includeEventType);
}

void StartStewartNativePlanner() {
    if (g_stewartNativePlannerRun.load()) {
        return;
    }

    g_stewartNativePlannerRun.store(true);
    g_stewartNativePlannerThread = std::thread([]() {
        try {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

        using clock = std::chrono::steady_clock;
        auto nextWake = clock::now();
        constexpr uint64_t kBroadcastEveryCycles = 5;
        uint32_t prevPeriodUs = 0;

        while (g_stewartNativePlannerRun.load()) {
            // Read desired PDO cycle time dynamically (set via UI/API, 500-10000 us)
            uint32_t periodUs = g_desiredCycleTimeUs;
            if (periodUs == 0) periodUs = 2000;  // fallback default

            // Re-baseline nextWake when period changes to avoid accumulation error
            if (periodUs != prevPeriodUs) {
                nextWake = clock::now() + std::chrono::microseconds(periodUs);
                prevPeriodUs = periodUs;
            } else {
                nextWake += std::chrono::microseconds(periodUs);
            }

            StewartNativeTargetRequest target;
            StewartNativePlannerState state;
            {
                std::lock_guard<std::mutex> lk(g_stewartNativeMutex);
                target = g_stewartNativeTargetRequest;
                state = g_stewartNativePlannerState;
            }

            bool shouldBroadcast = false;
            if (target.valid) {
                state.updatePeriodUs = periodUs;
                state.lastCycleEpochMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                state.cycleCounter += 1;

                if (!state.initialized) {
                    state.initialized = true;
                    state.currentPose = target.targetPose;
                    state.targetPose = target.targetPose;
                    state.params = target.params;
                    state.activeSeq = target.seq;
                    state.linearSpeedMmPerSec = target.linearSpeedMmPerSec;
                    state.angularSpeedDegPerSec = target.angularSpeedDegPerSec;
                    state.lastResult = WuSOEM::StewartIK::CalculateInverse(state.params, state.currentPose);
                    state.moving = false;
                    shouldBroadcast = true;
                } else {
                    const bool targetChanged = target.seq != state.activeSeq ||
                                               !PoseNearEqual(target.targetPose, state.targetPose) ||
                                               target.snapToTarget;

                    state.params = target.params;
                    state.targetPose = target.targetPose;
                    state.activeSeq = target.seq;
                    state.linearSpeedMmPerSec = target.linearSpeedMmPerSec;
                    state.angularSpeedDegPerSec = target.angularSpeedDegPerSec;

                    if (target.snapToTarget) {
                        state.currentPose = target.targetPose;
                    } else {
                        const long double dtSec = static_cast<long double>(periodUs) / 1000000.0L;
                        const long double linearStep = state.linearSpeedMmPerSec * dtSec;
                        const long double angularStep = state.angularSpeedDegPerSec * dtSec;
                        state.currentPose = AdvancePoseToward(
                            state.currentPose,
                            state.targetPose,
                            linearStep,
                            angularStep);
                    }

                    state.moving = !PoseNearEqual(state.currentPose, state.targetPose);
                    if (targetChanged || state.moving || (state.cycleCounter % kBroadcastEveryCycles) == 0) {
                        state.lastResult =
                            WuSOEM::StewartIK::CalculateInverse(state.params, state.currentPose);
                        shouldBroadcast = true;
                    }
                }

                {
                    std::lock_guard<std::mutex> lk(g_stewartNativeMutex);
                    g_stewartNativePlannerState = state;
                }

                if (shouldBroadcast) {
                    ::BroadcastStewartSSE(BuildStewartNativePlannerStatePayload(state, true));
                }
            }

            std::this_thread::sleep_until(nextWake);
        }
        } catch (const std::exception& e) {
            OutputDebugStringA((std::string("[StewartNativePlanner] exception: ") + e.what() + "\n").c_str());
        } catch (...) {
            OutputDebugStringA("[StewartNativePlanner] unknown exception\n");
        }
    });
}

void StopStewartNativePlanner() {
    if (!g_stewartNativePlannerRun.load()) {
        return;
    }

    g_stewartNativePlannerRun.store(false);
    if (g_stewartNativePlannerThread.joinable()) {
        try {
            g_stewartNativePlannerThread.join();
        } catch (...) {
        }
    }
}

void StartStewartSseBroadcaster() {
    if (g_stewartSseBroadcastRun.load()) {
        return;
    }

    ::StartStewartNativePlanner();
    g_stewartSseBroadcastRun.store(true);
    g_stewartSseBroadcaster = std::thread([]() {
        try {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

        uint64_t lastProcessedInverseSeq = 0;
        uint64_t lastProcessedForwardSeq = 0;
        while (g_stewartSseBroadcastRun.load()) {
            {
                std::unique_lock<std::mutex> waitLock(g_stewartSseWakeMutex);
                g_stewartSseWakeCv.wait(waitLock, [&]() {
                    return !g_stewartSseBroadcastRun.load() ||
                           HasPendingStewartSseRequest(
                               lastProcessedInverseSeq,
                               lastProcessedForwardSeq);
                });
            }

            if (!g_stewartSseBroadcastRun.load()) {
                break;
            }

            StewartInverseStreamRequest inverseRequest;
            {
                std::lock_guard<std::mutex> lk(g_stewartInverseStreamMutex);
                inverseRequest = g_stewartInverseStreamRequest;
            }

            StewartForwardStreamRequest forwardRequest;
            {
                std::lock_guard<std::mutex> lk(g_stewartForwardStreamMutex);
                forwardRequest = g_stewartForwardStreamRequest;
            }

            if (inverseRequest.valid && inverseRequest.seq != 0 &&
                inverseRequest.seq != lastProcessedInverseSeq) {
                const WuSOEM::IKResult result =
                    WuSOEM::StewartIK::CalculateInverse(inverseRequest.params, inverseRequest.worldPose);
                ::BroadcastStewartSSE(BuildStewartInverseStreamPayload(inverseRequest, result));
                lastProcessedInverseSeq = inverseRequest.seq;
            }

            if (forwardRequest.valid && forwardRequest.seq != 0 &&
                forwardRequest.seq != lastProcessedForwardSeq) {
                ::BroadcastStewartSSE(BuildStewartForwardStreamPayload(forwardRequest));
                lastProcessedForwardSeq = forwardRequest.seq;
            }
        }
        } catch (const std::exception& e) {
            OutputDebugStringA((std::string("[StewartSSE] exception: ") + e.what() + "\n").c_str());
        } catch (...) {
            OutputDebugStringA("[StewartSSE] unknown exception\n");
        }
    });
}

void StopStewartSseBroadcaster() {
    try {
        if (!g_stewartSseBroadcastRun.load()) {
            ::StopStewartNativePlanner();
            return;
        }

        g_stewartSseBroadcastRun.store(false);
        g_stewartSseWakeCv.notify_all();
        if (g_stewartSseBroadcaster.joinable()) {
            try {
                g_stewartSseBroadcaster.join();
            } catch (...) {
            }
        }

        std::lock_guard<std::mutex> lk(g_stewartSseClientsMutex);
        for (const auto& client : g_stewartSseClients) {
            try {
                closesocket(client.socket);
            } catch (...) {
            }
            try {
                CloseClientAndDecrement(client.socket);
            } catch (...) {
            }
        }
        g_stewartSseClients.clear();
        try {
            PostHttpConnCountIfChanged();
        } catch (...) {
        }
    } catch (...) {
    }

    ::StopStewartNativePlanner();
}

namespace HttpServerInternal {

void BroadcastSSE(const std::string& json) {
    ::BroadcastSSE(json);
}

void StartSseBroadcaster() {
    ::StartSseBroadcaster();
}

void StopSseBroadcaster() {
    ::StopSseBroadcaster();
}

void QueueStewartInverseStreamRequest(
    uint64_t seq,
    const std::string& clientIP,
    const WuSOEM::StewartParams& params,
    const WuSOEM::Pose6D& worldPose,
    const WuSOEM::Pose6D& platformPose,
    long double initialHeight) {
    ::QueueStewartInverseStreamRequest(
        seq,
        clientIP,
        params,
        worldPose,
        platformPose,
        initialHeight);
}

void QueueStewartForwardStreamRequest(
    uint64_t seq,
    const std::string& clientIP,
    const WuSOEM::StewartParams& params,
    const StewartWebSettings& settings,
    const WuSOEM::Pose6D& initialPose,
    const std::array<long double, 6>& servoAngles) {
    ::QueueStewartForwardStreamRequest(
        seq,
        clientIP,
        params,
        settings,
        initialPose,
        servoAngles);
}

void SubmitStewartNativeTarget(
    uint64_t seq,
    const WuSOEM::StewartParams& params,
    const WuSOEM::Pose6D& targetPose,
    long double linearSpeedMmPerSec,
    long double angularSpeedDegPerSec,
    bool snapToTarget) {
    ::SubmitStewartNativeTarget(
        seq,
        params,
        targetPose,
        linearSpeedMmPerSec,
        angularSpeedDegPerSec,
        snapToTarget);
}

void HoldStewartNativeTarget() {
    ::HoldStewartNativeTarget();
}

std::string GetStewartNativePlannerStateJson(bool includeEventType) {
    return ::GetStewartNativePlannerStateJson(includeEventType);
}

void StartStewartNativePlanner() {
    ::StartStewartNativePlanner();
}

void StopStewartNativePlanner() {
    ::StopStewartNativePlanner();
}

void StartStewartSseBroadcaster() {
    ::StartStewartSseBroadcaster();
}

void StopStewartSseBroadcaster() {
    ::StopStewartSseBroadcaster();
}

}  // namespace HttpServerInternal
