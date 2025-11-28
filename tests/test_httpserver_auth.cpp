#include "server/HttpServer.Internal.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <unordered_map>

namespace HttpServerInternal {

std::string g_httpApiKey;

std::mutex g_sessionMutex;
std::unordered_map<std::string, SessionInfo> g_sessions;
int g_sessionTtlSeconds = 60 * 60;
int g_sessionInactivityTimeoutSeconds = 5 * 60;
std::atomic<bool> g_forceLogoutOnNextRequest{false};

std::mutex g_loginAttemptMutex;
std::unordered_map<std::string, LoginAttempt> g_loginAttempts;
std::mutex g_rateLimitMutex;
std::unordered_map<std::string, RateLimitWindow> g_rateLimitMap;

}  // namespace HttpServerInternal

namespace {

bool Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

}  // namespace

int main() {
    using namespace HttpServerInternal;

    const auto now = std::chrono::system_clock::now();

    SessionInfo activeSession{now + std::chrono::seconds(10), now - std::chrono::seconds(30)};
    const auto originalExpiry = activeSession.expiry;
    if (!Check(IsSessionActive(activeSession, now), "recent session should be active") ||
        !Check(TryRefreshSession(activeSession, now), "active session should refresh") ||
        !Check(activeSession.lastActivity == now, "refresh should update lastActivity") ||
        !Check(activeSession.expiry > originalExpiry, "refresh should extend expiry")) {
        return EXIT_FAILURE;
    }

    SessionInfo idleSession{now + std::chrono::seconds(10), now - std::chrono::seconds(301)};
    if (!Check(!IsSessionActive(idleSession, now), "idle session should expire by inactivity") ||
        !Check(!TryRefreshSession(idleSession, now), "inactive session must not refresh")) {
        return EXIT_FAILURE;
    }

    SessionInfo expiredSession{now - std::chrono::seconds(1), now};
    if (!Check(!IsSessionActive(expiredSession, now), "absolute expiry should invalidate session")) {
        return EXIT_FAILURE;
    }

    g_httpApiKey = "required";
    g_sessions.clear();
    g_sessions["127.0.0.1"] = {now + std::chrono::seconds(5), now - std::chrono::seconds(15)};
    const auto previousSessionExpiry = g_sessions["127.0.0.1"].expiry;
    const std::string request = "GET /status HTTP/1.1\r\nHost: localhost\r\n\r\n";

    if (!Check(IsRequestAuthorized(request, "127.0.0.1"), "session-backed request should authorize") ||
        !Check(g_sessions["127.0.0.1"].expiry > previousSessionExpiry,
               "authorized request should slide expiry forward")) {
        return EXIT_FAILURE;
    }

    g_sessions["127.0.0.2"] = {now - std::chrono::seconds(1), now - std::chrono::seconds(1)};
    if (!Check(!IsRequestAuthorized(request, "127.0.0.2"), "expired session must not authorize") ||
        !Check(g_sessions.find("127.0.0.2") == g_sessions.end(),
               "expired session should be pruned on authorization check")) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}