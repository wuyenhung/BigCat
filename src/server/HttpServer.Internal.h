#pragma once

#include <winsock2.h>
#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/SystemState.h"
#include "MMFWriter.h"
#include "StewartIK.h"

namespace HttpServerInternal {

inline constexpr size_t kStatusCacheMaxBytes = 512 * 1024;

struct SessionInfo {
    std::chrono::system_clock::time_point expiry;
    std::chrono::system_clock::time_point lastActivity;
};

struct LoginAttempt {
    int failedCount = 0;
    std::chrono::steady_clock::time_point lastAttemptTime;
};

struct RateLimitWindow {
    int requestCount = 0;
    std::chrono::steady_clock::time_point windowStart;
};

struct IPConnection {
    SOCKET socket = INVALID_SOCKET;
    std::chrono::steady_clock::time_point connectionTime;
};

struct SSEClient {
    SOCKET socket = INVALID_SOCKET;
    std::string ip;
};

struct StewartWebSettings {
    long double servoInitialAngle = 15.0L;
    long double singularityVerticalAngle = 18.0L;
    long double singularityHorizontalAngle = 87.0L;
    long double singularityExtendRatio = 95.0L;
    long double singularityContractRatio = 105.0L;
};

struct StewartArmDiagnostic {
    bool active = false;
    std::string type;
    long double value = 0.0L;
    long double threshold = 0.0L;
};

struct StewartResolvedPoseInput {
    long double initialHeight = 0.0L;
    WuSOEM::Pose6D worldPose{};
    WuSOEM::Pose6D platformPose{};
};

struct StewartDiagnostics {
    long double initialHeight = 0.0L;
    std::array<std::array<long double, 3>, 6> basePoints{};
    std::array<int, 6> servoPointMap{{0,1,2,3,4,5}};
    std::array<long double, 6> rodLengths{};
    std::array<long double, 6> fisheyeAngles{};
    std::array<StewartArmDiagnostic, 6> singularityInfo{};
    std::vector<int> singularArms;
    std::vector<int> fisheyeWarningArms;
    std::vector<int> warningArms;
    std::vector<std::string> warnings;
    bool platformSingularity = false;
};

struct StewartInverseStreamRequest {
    bool valid = false;
    uint64_t seq = 0;
    std::string clientIP;
    WuSOEM::StewartParams params{};
    WuSOEM::Pose6D worldPose{};
    WuSOEM::Pose6D platformPose{};
    long double initialHeight = 0.0L;
};

struct StewartForwardStreamRequest {
    bool valid = false;
    uint64_t seq = 0;
    std::string clientIP;
    WuSOEM::StewartParams params{};
    StewartWebSettings settings{};
    WuSOEM::Pose6D initialPose{};
    std::array<long double, 6> servoAngles{};
};

struct StewartNativeTargetRequest {
    bool valid = false;
    uint64_t seq = 0;
    WuSOEM::StewartParams params{};
    WuSOEM::Pose6D targetPose{};
    long double linearSpeedMmPerSec = 600.0L;
    long double angularSpeedDegPerSec = 120.0L;
    bool snapToTarget = false;
};

struct StewartNativePlannerState {
    bool initialized = false;
    bool moving = false;
    uint64_t activeSeq = 0;
    uint64_t cycleCounter = 0;
    uint32_t updatePeriodUs = 2000;
    long long lastCycleEpochMs = 0;
    long double linearSpeedMmPerSec = 600.0L;
    long double angularSpeedDegPerSec = 120.0L;
    WuSOEM::StewartParams params{};
    WuSOEM::Pose6D currentPose{};
    WuSOEM::Pose6D targetPose{};
    WuSOEM::IKResult lastResult{};
};

extern std::thread g_httpThread;
extern std::atomic_bool g_httpServerRun;
extern SOCKET g_listenSocket;
extern HWND g_hwndHttpTarget;

extern std::mutex g_slaveCacheMutex;
extern std::string g_slaveInfoCacheJson;
extern std::mutex g_slaveSdoCacheMutex;
extern std::unordered_map<int, std::string> g_slaveSdoCache;

extern std::atomic<int> g_httpPort;  // 實際監聽埠（bind 被占用時自動跳號）
extern std::string g_httpApiKey;
extern std::string g_httpBindAddr;

extern std::mutex g_sessionMutex;
extern std::unordered_map<std::string, SessionInfo> g_sessions;
extern int g_sessionTtlSeconds;
extern int g_sessionInactivityTimeoutSeconds;
extern std::atomic<bool> g_forceLogoutOnNextRequest;

extern std::mutex g_loginAttemptMutex;
extern std::unordered_map<std::string, LoginAttempt> g_loginAttempts;
extern std::mutex g_rateLimitMutex;
extern std::unordered_map<std::string, RateLimitWindow> g_rateLimitMap;

extern std::mutex g_perIpConnectionMutex;
extern std::unordered_map<std::string, IPConnection> g_perIpConnections;

extern std::atomic<int> g_activeConnections;
extern int g_maxConnections;
extern int g_sseBroadcastIntervalMs;
extern std::atomic<long long> g_lastHttpConnPostTimeMs;
extern const long long kHttpConnThrottleMs;
extern std::atomic<long long> g_lastRejectedPostTimeMs;
extern const long long kRejectedThrottleMs;

extern std::mutex g_adapterStatusMutex;
extern std::string g_adapterStatus;

extern std::mutex g_sseClientsMutex;
extern std::vector<SSEClient> g_sseClients;
extern std::atomic_bool g_sseBroadcastRun;
extern std::thread g_sseBroadcaster;
extern std::mutex g_sseWakeMutex;
extern std::condition_variable g_sseWakeCv;

extern std::mutex g_stewartSseClientsMutex;
extern std::vector<SSEClient> g_stewartSseClients;
extern std::atomic_bool g_stewartSseBroadcastRun;
extern std::thread g_stewartSseBroadcaster;
extern std::mutex g_stewartSseWakeMutex;
extern std::condition_variable g_stewartSseWakeCv;
extern std::mutex g_stewartInverseStreamMutex;
extern StewartInverseStreamRequest g_stewartInverseStreamRequest;
extern std::mutex g_stewartForwardStreamMutex;
extern StewartForwardStreamRequest g_stewartForwardStreamRequest;
extern std::mutex g_stewartNativeMutex;
extern StewartNativeTargetRequest g_stewartNativeTargetRequest;
extern StewartNativePlannerState g_stewartNativePlannerState;
extern std::atomic_bool g_stewartNativePlannerRun;
extern std::thread g_stewartNativePlannerThread;

// Pose6D 硬限制：從 machine.xml 載入，由 AppController 設定，HTTP API 讀取
extern std::mutex g_pose6dLimitsMutex;
extern WuSOEM::Pose6DLimits g_pose6dLimits;
void SetPose6DLimits(const WuSOEM::Pose6DLimits& limits);
WuSOEM::Pose6DLimits GetPose6DLimits();

extern std::mutex g_uiStateMutex;
extern std::unordered_map<std::string, bool> g_uiDisabled;

extern std::mutex g_cycleMutex;
extern uint32_t g_desiredCycleTimeUs;
extern uint32_t g_commsTimeUs;
extern uint32_t g_actualCycleTimeUs;
extern uint32_t g_maxObservedCycleTimeUs;
extern uint32_t g_spikePhase;
extern uint32_t g_spikeCycleUs;
extern uint32_t g_spikeCommsUs;
extern uint32_t g_spikeCount;
extern std::chrono::system_clock::time_point g_ignoreWin32EchoUntil;

extern std::mutex g_mailboxMutex;
extern std::string g_mailboxUtf8;
extern std::mutex g_statusMutex;
extern std::string g_lastStatusUtf8;

extern HANDLE g_hMapFile;
extern const MMFWriter::MMFData* g_pMmView;

enum class RequestHandleResult {
    kNotHandled = 0,
    kHandledClose,
    kHandledKeepOpen,
};

struct HttpRequestContext {
    SOCKET client = INVALID_SOCKET;
    std::string clientIP;
    std::string method;
    std::string path;
    std::string requestRaw;
    std::string body;
};

enum class ApiResponseKind {
    kNone = 0,
    kNoContent,
    kJson,
};

struct ApiCommandAction {
    bool handled = false;
    bool shouldCacheMailbox = false;
    bool shouldUpdateDesiredCycle = false;
    uint32_t desiredCycleUs = 0;
    uint32_t postedWindowMessage = 0;
    ApiResponseKind responseKind = ApiResponseKind::kNone;
    std::string responseStatusLine;
    std::string responseBody;
    std::string command;
    std::string mailboxUtf8;
};

std::string TrimHttpValue(const std::string& s);
int ParseSlaveFromPath(const std::string& path);
bool TryParseCyclePeriodUs(const std::string& body, uint32_t& periodUs);
std::string ExtractSelectedSlaveValue(const std::string& body);
const char* GetDirectUiCommandForRoute(const std::string& path);
const char* GetMmfCommandForRoute(const std::string& path);
ApiCommandAction BuildApiCommandAction(const std::string& path, const std::string& body);
ApiCommandAction BuildConnectAction(bool busy, bool ethercatOk, const std::string& webAdapterUtf8);
ApiCommandAction BuildAutoConnectAction(const CoreHelpers::SystemStateSnapshot& snapshot);
ApiCommandAction BuildAutoWindowMessageAction(const std::string& path, const CoreHelpers::SystemStateSnapshot& snapshot);
ApiCommandAction BuildAutoFaultResetAction(const CoreHelpers::SystemStateSnapshot& snapshot);
ApiCommandAction BuildAutoDownToOriginAction(const CoreHelpers::SystemStateSnapshot& snapshot);
bool IsAutoConnectUiEnabled(const CoreHelpers::SystemStateSnapshot& snapshot);
bool IsAutoConnectRequestAllowed(const CoreHelpers::SystemStateSnapshot& snapshot);
const char* ValidateAutoFaultResetRequest(const CoreHelpers::SystemStateSnapshot& snapshot);
const char* ValidateAutoDownToOriginRequest(const CoreHelpers::SystemStateSnapshot& snapshot);
void NormalizeStatusCacheUtf8(std::string& utf8);
void AppendStatusCacheLine(std::string& utf8, const std::string& line);

std::string GetStaticRelativePath(const std::string& path);
bool IsApiRouteLike(const std::string& relPath);
bool IsForbiddenStaticPath(const std::string& relPath);
std::string GetStaticContentType(const std::string& filepath);
bool LoadStaticWebFile(const std::string& relPath, std::string& content);

void PostHttpConnCountIfChanged();
void CloseClientAndDecrement(SOCKET s);
bool SendAll(SOCKET s, const std::string& data);
bool SendAll(SOCKET s, const char* data);
std::string GetClientIPFromSocket(SOCKET client);
std::string GetExecutableDirA();
void EnsurePDOMapped();
void CleanupExpiredSessions();
bool IsSessionActive(const SessionInfo& session, const std::chrono::system_clock::time_point& now);
bool TryRefreshSession(SessionInfo& session, const std::chrono::system_clock::time_point& now);
bool TryRefreshSessionByClientIP(const std::string& clientIP,
                                 const std::chrono::system_clock::time_point& now);

std::string GetHeaderValue(const std::string& raw, const std::string& name);
bool IsRequestAuthorized(const std::string& raw, const std::string& clientIP);
bool IsLoginLockedOut(const std::string& clientIP);
void RecordFailedLoginAttempt(const std::string& clientIP);
void ClearLoginAttempts(const std::string& clientIP);
bool CheckRateLimit(const std::string& clientIP);

bool IsSSEConnectionForIP(const std::string& clientIP, SOCKET socket);
void RegisterOrReplaceIPConnection(const std::string& clientIP, SOCKET newSocket);
void UnregisterIPConnection(const std::string& clientIP, SOCKET socket);
bool ShouldTrackPerIpConnection(const HttpRequestContext& ctx);

RequestHandleResult TryHandleStaticRequest(const HttpRequestContext& ctx);
RequestHandleResult TryHandleApiRequest(const HttpRequestContext& ctx);

void HttpServerThreadFunc();
void BroadcastSSE(const std::string& json);
void StartSseBroadcaster();
void StopSseBroadcaster();
WuSOEM::Pose6D BuildPlatformPoseFromWorldPose(
    const WuSOEM::Pose6D& worldPose,
    long double initialHeight);
std::array<std::array<long double, 3>, 6> ComputeArmEndPoints(
    const WuSOEM::StewartParams& params,
    const std::array<long double, 6>& servoAngles);
std::vector<int> ExtractErrorArmsFromErrors(const std::vector<std::string>& errors);
StewartDiagnostics AnalyzeStewartResult(
    const WuSOEM::StewartParams& params,
    const StewartWebSettings& settings,
    const WuSOEM::IKResult& result);
std::string BuildForwardKinematicsJson(
    const WuSOEM::ForwardResult& forwardResult,
    const StewartResolvedPoseInput& resolvedPose,
    const std::array<long double, 6>& servoAngles,
    const std::array<std::array<long double, 3>, 6>& armEndPoints,
    const std::array<std::array<long double, 3>, 6>& platformPoints,
    const StewartDiagnostics& diagnostics,
    const std::vector<int>& errorArms);
void QueueStewartInverseStreamRequest(
    uint64_t seq,
    const std::string& clientIP,
    const WuSOEM::StewartParams& params,
    const WuSOEM::Pose6D& worldPose,
    const WuSOEM::Pose6D& platformPose,
    long double initialHeight);
void QueueStewartForwardStreamRequest(
    uint64_t seq,
    const std::string& clientIP,
    const WuSOEM::StewartParams& params,
    const StewartWebSettings& settings,
    const WuSOEM::Pose6D& initialPose,
    const std::array<long double, 6>& servoAngles);
void SubmitStewartNativeTarget(
    uint64_t seq,
    const WuSOEM::StewartParams& params,
    const WuSOEM::Pose6D& targetPose,
    long double linearSpeedMmPerSec,
    long double angularSpeedDegPerSec,
    bool snapToTarget);
void HoldStewartNativeTarget();
std::string GetStewartNativePlannerStateJson(bool includeEventType);
void StartStewartNativePlanner();
void StopStewartNativePlanner();
void StartStewartSseBroadcaster();
void StopStewartSseBroadcaster();

}  // namespace HttpServerInternal
