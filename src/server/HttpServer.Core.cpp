// HttpServer.Core.cpp - 簡要說明:
// 管理 HTTP 監聽 socket、連線生命週期、請求解析與分流。

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "HttpServer.h"
#include "MainWindow.h"
#include "core/AppConfig.h"
#include "core/CoreHelpers.h"
#include "HttpServer.Internal.h"

namespace HttpServerInternal {

std::thread g_httpThread;
std::atomic_bool g_httpServerRun(false);
SOCKET g_listenSocket = INVALID_SOCKET;
HWND g_hwndHttpTarget = NULL;

std::mutex g_slaveCacheMutex;
std::string g_slaveInfoCacheJson;
std::mutex g_slaveSdoCacheMutex;
std::unordered_map<int, std::string> g_slaveSdoCache;

int g_httpPort = 8080;
std::string g_httpApiKey;
std::string g_httpBindAddr;

std::mutex g_sessionMutex;
std::unordered_map<std::string, SessionInfo> g_sessions;
int g_sessionTtlSeconds = 24 * 60 * 60;
int g_sessionInactivityTimeoutSeconds = 24 * 60 * 60;
std::atomic<bool> g_forceLogoutOnNextRequest{false};

std::mutex g_loginAttemptMutex;
std::unordered_map<std::string, LoginAttempt> g_loginAttempts;
std::mutex g_rateLimitMutex;
std::unordered_map<std::string, RateLimitWindow> g_rateLimitMap;

std::mutex g_perIpConnectionMutex;
std::unordered_map<std::string, IPConnection> g_perIpConnections;

std::atomic<int> g_activeConnections{0};
int g_maxConnections = 32;
int g_sseBroadcastIntervalMs = 10;
std::atomic<long long> g_lastHttpConnPostTimeMs{0};
const long long kHttpConnThrottleMs = 300;
std::atomic<long long> g_lastRejectedPostTimeMs{0};
const long long kRejectedThrottleMs = 500;
const long long kPerIpReplacementGraceMs = 1500;

std::mutex g_adapterStatusMutex;
std::string g_adapterStatus;

std::mutex g_sseClientsMutex;
std::vector<SSEClient> g_sseClients;
std::atomic_bool g_sseBroadcastRun(false);
std::thread g_sseBroadcaster;
std::mutex g_sseWakeMutex;
std::condition_variable g_sseWakeCv;

std::mutex g_stewartSseClientsMutex;
std::vector<SSEClient> g_stewartSseClients;
std::atomic_bool g_stewartSseBroadcastRun(false);
std::thread g_stewartSseBroadcaster;
std::mutex g_stewartSseWakeMutex;
std::condition_variable g_stewartSseWakeCv;
std::mutex g_stewartInverseStreamMutex;
StewartInverseStreamRequest g_stewartInverseStreamRequest;
std::mutex g_stewartForwardStreamMutex;
StewartForwardStreamRequest g_stewartForwardStreamRequest;
std::mutex g_stewartNativeMutex;
StewartNativeTargetRequest g_stewartNativeTargetRequest;
StewartNativePlannerState g_stewartNativePlannerState;
std::atomic_bool g_stewartNativePlannerRun(false);
std::thread g_stewartNativePlannerThread;

std::mutex g_uiStateMutex;
std::unordered_map<std::string, bool> g_uiDisabled;

std::mutex g_cycleMutex;
uint32_t g_desiredCycleTimeUs = 2000;
uint32_t g_commsTimeUs = 0;
uint32_t g_actualCycleTimeUs = 0;
uint32_t g_maxObservedCycleTimeUs = 0;
std::chrono::system_clock::time_point g_ignoreWin32EchoUntil;

std::mutex g_mailboxMutex;
std::string g_mailboxUtf8;
std::mutex g_statusMutex;
std::string g_lastStatusUtf8;

HANDLE g_hMapFile = NULL;
const MMFWriter::MMFData* g_pMmView = NULL;

void PostHttpConnCountIfChanged() {
    (void)g_activeConnections.load();
}

void CloseClientAndDecrement(SOCKET s) {
    if (s == INVALID_SOCKET) {
        return;
    }

    shutdown(s, SD_BOTH);
    closesocket(s);

    int prev = g_activeConnections.fetch_sub(1);
    if (prev <= 0) {
        g_activeConnections.store(0);
    }
}

namespace {

bool TryParseContentLength(const std::string& requestHeaders, size_t& contentLength) {
    contentLength = 0;

    std::istringstream headerStream(requestHeaders);
    std::string line;
    while (std::getline(headerStream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line.empty()) {
            continue;
        }

        const size_t colonPos = line.find(':');
        if (colonPos == std::string::npos) {
            continue;
        }

        std::string name = line.substr(0, colonPos);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        if (name != "content-length") {
            continue;
        }

        std::string value = line.substr(colonPos + 1);
        const size_t first = value.find_first_not_of(" \t");
        if (first == std::string::npos) {
            return false;
        }

        const size_t last = value.find_last_not_of(" \t");
        value = value.substr(first, last - first + 1);

        try {
            const unsigned long long parsed = std::stoull(value);
            contentLength = static_cast<size_t>(parsed);
            return true;
        } catch (...) {
            return false;
        }
    }

    return true;
}

bool ReceiveCompleteHttpRequest(SOCKET client, std::string& requestRaw) {
    requestRaw.clear();

    char buffer[4096];
    size_t contentLength = 0;
    bool contentLengthParsed = false;
    size_t expectedTotalSize = 0;

    while (true) {
        const int received = recv(client, buffer, static_cast<int>(sizeof(buffer)), 0);
        if (received <= 0) {
            return !requestRaw.empty();
        }

        requestRaw.append(buffer, static_cast<size_t>(received));

        const size_t headerEnd = requestRaw.find("\r\n\r\n");
        if (headerEnd == std::string::npos) {
            continue;
        }

        if (!contentLengthParsed) {
            const std::string headerBlock = requestRaw.substr(0, headerEnd);
            if (!TryParseContentLength(headerBlock, contentLength)) {
                return false;
            }

            contentLengthParsed = true;
            expectedTotalSize = headerEnd + 4 + contentLength;
        }

        if (requestRaw.size() >= expectedTotalSize) {
            if (requestRaw.size() > expectedTotalSize) {
                requestRaw.resize(expectedTotalSize);
            }
            return true;
        }
    }
}

bool SendBuffer(SOCKET s, const char* data, size_t size) {
    if (s == INVALID_SOCKET || data == nullptr || size == 0) {
        return size == 0;
    }

    size_t totalSent = 0;
    while (totalSent < size) {
        const size_t remaining = size - totalSent;
        const size_t chunk = (std::min)(remaining, static_cast<size_t>((std::numeric_limits<int>::max)()));
        const int sent = send(s, data + totalSent, static_cast<int>(chunk), 0);
        if (sent == SOCKET_ERROR || sent <= 0) {
            return false;
        }
        totalSent += static_cast<size_t>(sent);
    }

    return true;
}

}  // namespace

bool SendAll(SOCKET s, const std::string& data) {
    return SendBuffer(s, data.data(), data.size());
}

bool SendAll(SOCKET s, const char* data) {
    if (data == nullptr) {
        return false;
    }
    return SendBuffer(s, data, std::strlen(data));
}

std::string GetClientIPFromSocket(SOCKET client) {
    sockaddr_in addr = {};
    int addrlen = sizeof(addr);
    if (getpeername(client, reinterpret_cast<sockaddr*>(&addr), &addrlen) == 0) {
        return std::string(inet_ntoa(addr.sin_addr));
    }
    return "unknown";
}

std::string GetExecutableDirA() {
    char buf[MAX_PATH] = {0};
    if (GetModuleFileNameA(NULL, buf, MAX_PATH) != 0) {
        std::string dir(buf);
        size_t p = dir.find_last_of("\\/");
        if (p != std::string::npos) {
            return dir.substr(0, p + 1);
        }
    }
    return std::string();
}

static std::mutex g_mmfMutex;

void EnsurePDOMapped() {
    std::lock_guard<std::mutex> lk(g_mmfMutex);
    if (g_pMmView) {
        return;
    }
    if (!g_hMapFile) {
        g_hMapFile = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Local\\WuEtherCAT_PDO");
    }
    if (g_hMapFile && !g_pMmView) {
        g_pMmView = reinterpret_cast<const MMFWriter::MMFData*>(
            MapViewOfFile(g_hMapFile, FILE_MAP_READ, 0, 0, sizeof(MMFWriter::MMFData)));
    }
}

bool IsSSEConnectionForIP(const std::string& clientIP, SOCKET socket) {
    std::lock_guard<std::mutex> lk(g_sseClientsMutex);
    for (const auto& client : g_sseClients) {
        if (client.ip == clientIP && client.socket == socket) {
            return true;
        }
    }

    std::lock_guard<std::mutex> stewartLk(g_stewartSseClientsMutex);
    for (const auto& client : g_stewartSseClients) {
        if (client.ip == clientIP && client.socket == socket) {
            return true;
        }
    }

    return false;
}

void RegisterOrReplaceIPConnection(const std::string& clientIP, SOCKET newSocket) {
    std::lock_guard<std::mutex> lk(g_perIpConnectionMutex);

    auto it = g_perIpConnections.find(clientIP);
    if (it != g_perIpConnections.end()) {
        const auto now = std::chrono::steady_clock::now();
        const auto existingAgeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - it->second.connectionTime);

        if (existingAgeMs.count() < kPerIpReplacementGraceMs) {
            return;
        }

        SOCKET oldSocket = it->second.socket;
        if (oldSocket != INVALID_SOCKET && oldSocket != newSocket && !IsSSEConnectionForIP(clientIP, oldSocket)) {
            try {
                CloseClientAndDecrement(oldSocket);
                PostHttpConnCountIfChanged();
            } catch (...) {
            }
        }
    }

    if (!IsSSEConnectionForIP(clientIP, newSocket)) {
        g_perIpConnections[clientIP] = {newSocket, std::chrono::steady_clock::now()};
    }
}

void UnregisterIPConnection(const std::string& clientIP, SOCKET socket) {
    std::lock_guard<std::mutex> lk(g_perIpConnectionMutex);

    auto it = g_perIpConnections.find(clientIP);
    if (it != g_perIpConnections.end() && it->second.socket == socket) {
        g_perIpConnections.erase(it);
    }
}

static std::string TrimCopy(const std::string& v) {
    size_t s = 0;
    while (s < v.size() && std::isspace(static_cast<unsigned char>(v[s])) != 0) {
        ++s;
    }

    size_t e = v.size();
    while (e > s && std::isspace(static_cast<unsigned char>(v[e - 1])) != 0) {
        --e;
    }

    return v.substr(s, e - s);
}

static void LoadHttpApiKeyFromDisk() {
    AppConfig::EnsureDefaultIniFile();
    g_httpApiKey = TrimCopy(CoreHelpers::WideToUtf8(AppConfig::ReadServerPassword()));
    if (g_httpApiKey.empty()) {
        g_httpApiKey = "1234";
    }
}

static void InitializeDefaultUiState() {
    std::lock_guard<std::mutex> lk(g_uiStateMutex);
    g_uiDisabled["oscilloscope"] = true;
    g_uiDisabled["attitude"] = true;
    g_uiDisabled["mechanism"] = true;
    g_uiDisabled["auto_test"] = true;
    g_uiDisabled["toggle_status"] = true;
    g_uiDisabled["connect"] = true;
    g_uiDisabled["fault_reset"] = true;
    g_uiDisabled["down_to_origin"] = true;
    g_uiDisabled["auto_connect"] = true;
}

void HttpServerThreadFunc() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return;
    }

    LoadHttpApiKeyFromDisk();

    std::string bindAddr = "0.0.0.0";
    char envBuf[64] = {0};
    DWORD bindLen = GetEnvironmentVariableA("WUSOEM_HTTP_BIND_ADDR", envBuf, static_cast<DWORD>(sizeof(envBuf)));
    if (bindLen > 0 && bindLen < sizeof(envBuf)) {
        bindAddr = std::string(envBuf, static_cast<size_t>(bindLen));
    }
    g_httpBindAddr = bindAddr;

    InitializeDefaultUiState();

    char maxEnv[32] = {0};
    DWORD maxLen = GetEnvironmentVariableA("WUSOEM_HTTP_MAX_CONNS", maxEnv, static_cast<DWORD>(sizeof(maxEnv)));
    if (maxLen > 0 && maxLen < sizeof(maxEnv)) {
        int v = std::atoi(maxEnv);
        if (v > 0) {
            g_maxConnections = v;
        }
    }

    g_listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listenSocket == INVALID_SOCKET) {
        WSACleanup();
        return;
    }

    sockaddr_in srv{};
    srv.sin_family = AF_INET;
    srv.sin_addr.S_un.S_addr = inet_addr(bindAddr.c_str());
    srv.sin_port = htons(static_cast<unsigned short>(g_httpPort));

    BOOL reuse = TRUE;
    setsockopt(g_listenSocket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    if (bind(g_listenSocket, reinterpret_cast<sockaddr*>(&srv), sizeof(srv)) == SOCKET_ERROR) {
        closesocket(g_listenSocket);
        g_listenSocket = INVALID_SOCKET;
        WSACleanup();
        return;
    }

    if (listen(g_listenSocket, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(g_listenSocket);
        g_listenSocket = INVALID_SOCKET;
        WSACleanup();
        return;
    }

    try {
        std::string json = CoreHelpers::EnumerateAdaptersJson();
        (void)json;
    } catch (...) {
    }

    fd_set readfds;
    timeval tv;

    while (g_httpServerRun.load()) {
        FD_ZERO(&readfds);
        FD_SET(g_listenSocket, &readfds);
        tv.tv_sec = 0;
        tv.tv_usec = 200000;

        int sel = select(static_cast<int>(g_listenSocket) + 1, &readfds, NULL, NULL, &tv);
        if (sel <= 0 || !FD_ISSET(g_listenSocket, &readfds)) {
            continue;
        }

        SOCKET client = accept(g_listenSocket, NULL, NULL);
        if (client == INVALID_SOCKET) {
            continue;
        }

        int prev = g_activeConnections.fetch_add(1);
        if (prev >= g_maxConnections) {
            int activeNow = g_activeConnections.load();
            std::string body;
            try {
                std::ostringstream b;
                b << "{\"error\":\"server busy\",\"active\":" << activeNow << ",\"max\":" << g_maxConnections << "}";
                body = b.str();
            } catch (...) {
                body = "{\"error\":\"server busy\"}";
            }

            std::ostringstream hdr;
            hdr << "HTTP/1.1 503 Service Unavailable\r\n"
                << "Connection: close\r\n"
                << "Retry-After: 3\r\n"
                << "Content-Type: application/json; charset=utf-8\r\n"
                << "Content-Length: " << body.size() << "\r\n\r\n";

            std::string busy = hdr.str() + body;
            SendAll(client, busy);

            auto now = std::chrono::steady_clock::now();
            long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
            long long lastMs = g_lastRejectedPostTimeMs.load();
            if (nowMs - lastMs >= kRejectedThrottleMs) {
                g_lastRejectedPostTimeMs.store(nowMs);
            }

            CloseClientAndDecrement(client);
            PostHttpConnCountIfChanged();
            continue;
        }

        PostHttpConnCountIfChanged();

        // Dispatch each accepted connection to its own thread so the accept
        // loop is never blocked by slow I/O (static file reads, large API
        // payloads, etc.).  This is the primary fix for the slow-page-load
        // issue that required users to refresh their browser repeatedly.
        std::thread([client]() {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

            timeval connTv;
            connTv.tv_sec = 3;
            connTv.tv_usec = 0;
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&connTv), sizeof(connTv));

            {
                static std::atomic<int> cleanupCounter{0};
                if (cleanupCounter.fetch_add(1) % 50 == 0) {
                    CleanupExpiredSessions();
                }
            }

            std::string clientIP = GetClientIPFromSocket(client);

            std::string req;
            if (!ReceiveCompleteHttpRequest(client, req) || req.empty()) {
                UnregisterIPConnection(clientIP, client);
                CloseClientAndDecrement(client);
                PostHttpConnCountIfChanged();
                return;
            }

            std::istringstream iss(req);
            std::string method;
            std::string path;
            std::string httpver;
            iss >> method >> path >> httpver;

            size_t bodyPos = req.find("\r\n\r\n");
            std::string body;
            if (bodyPos != std::string::npos) {
                body = req.substr(bodyPos + 4);
            }

            HttpRequestContext ctx;
            ctx.client = client;
            ctx.clientIP = clientIP;
            ctx.method = method;
            ctx.path = path;
            ctx.requestRaw = req;
            ctx.body = body;

            if (ShouldTrackPerIpConnection(ctx)) {
                RegisterOrReplaceIPConnection(clientIP, client);
            }

            RequestHandleResult result = TryHandleStaticRequest(ctx);
            if (result == RequestHandleResult::kHandledKeepOpen) {
                return;
            }
            if (result == RequestHandleResult::kHandledClose) {
                UnregisterIPConnection(clientIP, client);
                CloseClientAndDecrement(client);
                PostHttpConnCountIfChanged();
                return;
            }

            result = TryHandleApiRequest(ctx);
            if (result == RequestHandleResult::kHandledKeepOpen) {
                return;
            }
            if (result == RequestHandleResult::kHandledClose) {
                UnregisterIPConnection(clientIP, client);
                CloseClientAndDecrement(client);
                PostHttpConnCountIfChanged();
                return;
            }

            const char* notf = "HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Type: text/plain; charset=utf-8\r\n\r\nNot Found";
            SendAll(client, notf);

            UnregisterIPConnection(clientIP, client);
            CloseClientAndDecrement(client);
            PostHttpConnCountIfChanged();
        }).detach();
    }

    if (g_listenSocket != INVALID_SOCKET) {
        closesocket(g_listenSocket);
        g_listenSocket = INVALID_SOCKET;
    }

    WSACleanup();
}

}  // namespace HttpServerInternal

void StartLocalHttpServer(int port, HWND hwnd) {
    using namespace HttpServerInternal;

    if (g_httpServerRun.load()) {
        return;
    }

    g_httpPort = port;
    g_hwndHttpTarget = hwnd;
    g_httpServerRun.store(true);
    g_httpThread = std::thread(HttpServerThreadFunc);
}

void StopLocalHttpServer() {
    using namespace HttpServerInternal;

    if (!g_httpServerRun.load()) {
        return;
    }

    g_httpServerRun.store(false);
    ::StopSseBroadcaster();
    HttpServerInternal::StopStewartSseBroadcaster();
    ::StopStewartNativePlanner();

    if (g_listenSocket != INVALID_SOCKET) {
        closesocket(g_listenSocket);
        g_listenSocket = INVALID_SOCKET;
    }

    if (g_httpThread.joinable()) {
        g_httpThread.join();
    }

    g_hwndHttpTarget = NULL;
}

std::string GetHttpServerUrl() {
    using namespace HttpServerInternal;

    if (!g_httpServerRun.load()) {
        return std::string();
    }

    std::ostringstream ss;
    ss << "http://localhost:" << g_httpPort << "/";
    return ss.str();
}

int GetActiveHttpConnections() {
    return HttpServerInternal::g_activeConnections.load();
}

int GetMaxHttpConnections() {
    return HttpServerInternal::g_maxConnections;
}
