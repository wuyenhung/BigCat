// HttpServer.Auth.cpp - 認證、會話、速率限制。

#include <algorithm>
#include <cctype>
#include <chrono>
#include <mutex>
#include <string>

#include "HttpServer.h"
#include "HttpServer.Internal.h"

namespace {

constexpr int kMaxLoginAttempts = 5;
constexpr int kLoginLockoutSeconds = 300;
constexpr int kMaxRequestsPerSecond = 10;
constexpr int kRateLimitWindowMs = 1000;

bool TrimmedEquals(const std::string& val, const std::string& key) {
    size_t s = 0;
    while (s < val.size() && std::isspace(static_cast<unsigned char>(val[s])) != 0) {
        ++s;
    }

    size_t e = val.size();
    while (e > s && std::isspace(static_cast<unsigned char>(val[e - 1])) != 0) {
        --e;
    }

    return val.substr(s, e - s) == key;
}

}  // namespace

namespace HttpServerInternal {

bool IsSessionActive(const SessionInfo& session, const std::chrono::system_clock::time_point& now) {
    if (session.expiry <= now) {
        return false;
    }

    const auto timeSinceLastActivity =
        std::chrono::duration_cast<std::chrono::seconds>(now - session.lastActivity);
    return timeSinceLastActivity.count() <= g_sessionInactivityTimeoutSeconds;
}

bool TryRefreshSession(SessionInfo& session, const std::chrono::system_clock::time_point& now) {
    if (!IsSessionActive(session, now)) {
        return false;
    }

    session.lastActivity = now;
    session.expiry = now + std::chrono::seconds(g_sessionTtlSeconds);
    return true;
}

bool TryRefreshSessionByClientIP(const std::string& clientIP,
                                 const std::chrono::system_clock::time_point& now) {
    auto it = g_sessions.find(clientIP);
    if (it == g_sessions.end()) {
        return false;
    }

    if (!TryRefreshSession(it->second, now)) {
        g_sessions.erase(it);
        return false;
    }

    return true;
}

void CleanupExpiredSessions() {
    std::lock_guard<std::mutex> lk(g_sessionMutex);
    auto now = std::chrono::system_clock::now();
    auto it = g_sessions.begin();
    while (it != g_sessions.end()) {
        if (!IsSessionActive(it->second, now)) {
            it = g_sessions.erase(it);
        } else {
            ++it;
        }
    }
}

std::string GetHeaderValue(const std::string& raw, const std::string& name) {
    std::string lower = raw;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    std::string lname = "\r\n" + name;
    std::transform(lname.begin(), lname.end(), lname.begin(), ::tolower);

    size_t pos = lower.find(lname + ":");
    if (pos == std::string::npos) {
        return std::string();
    }

    pos += lname.size() + 1;
    while (pos < lower.size() && lower[pos] == ' ') {
        ++pos;
    }

    size_t end = lower.find("\r\n", pos);
    if (end == std::string::npos) {
        end = lower.size();
    }

    return raw.substr(pos, end - pos);
}

bool IsLoginLockedOut(const std::string& clientIP) {
    std::lock_guard<std::mutex> lk(g_loginAttemptMutex);
    auto it = g_loginAttempts.find(clientIP);
    if (it == g_loginAttempts.end()) {
        return false;
    }

    auto now = std::chrono::steady_clock::now();
    auto elapsed =
        std::chrono::duration_cast<std::chrono::seconds>(now - it->second.lastAttemptTime).count();

    if (elapsed > kLoginLockoutSeconds) {
        g_loginAttempts.erase(it);
        return false;
    }

    return it->second.failedCount >= kMaxLoginAttempts;
}

void RecordFailedLoginAttempt(const std::string& clientIP) {
    std::lock_guard<std::mutex> lk(g_loginAttemptMutex);
    auto& attempt = g_loginAttempts[clientIP];
    attempt.failedCount++;
    attempt.lastAttemptTime = std::chrono::steady_clock::now();
}

void ClearLoginAttempts(const std::string& clientIP) {
    std::lock_guard<std::mutex> lk(g_loginAttemptMutex);
    g_loginAttempts.erase(clientIP);
}

bool CheckRateLimit(const std::string& clientIP) {
    std::lock_guard<std::mutex> lk(g_rateLimitMutex);
    auto now = std::chrono::steady_clock::now();

    auto it = g_rateLimitMap.find(clientIP);
    if (it == g_rateLimitMap.end()) {
        g_rateLimitMap[clientIP] = {1, now};
        return true;
    }

    auto& window = it->second;
    auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - window.windowStart).count();

    if (elapsed > kRateLimitWindowMs) {
        window.requestCount = 1;
        window.windowStart = now;
        return true;
    }

    if (window.requestCount >= kMaxRequestsPerSecond) {
        return false;
    }

    window.requestCount++;
    return true;
}

bool IsRequestAuthorized(const std::string& raw, const std::string& clientIP) {
    if (g_httpApiKey.empty()) {
        return true;
    }

    std::string val = GetHeaderValue(raw, "x-api-key");
    if (!val.empty() && TrimmedEquals(val, g_httpApiKey)) {
        return true;
    }

    {
        std::lock_guard<std::mutex> lk(g_sessionMutex);
        const auto now = std::chrono::system_clock::now();
        if (TryRefreshSessionByClientIP(clientIP, now)) {
            return true;
        }
    }

    return false;
}

}  // namespace HttpServerInternal

bool IsValidApiKey(const std::string& key) {
    if (HttpServerInternal::g_httpApiKey.empty()) {
        return true;
    }
    return TrimmedEquals(key, HttpServerInternal::g_httpApiKey);
}

void ClearAllSessions() {
    std::lock_guard<std::mutex> lk(HttpServerInternal::g_sessionMutex);
    HttpServerInternal::g_sessions.clear();
}

void SetForceLogoutFlag() {
    HttpServerInternal::g_forceLogoutOnNextRequest.store(true);
}

bool CheckAndClearForceLogoutFlag() {
    return HttpServerInternal::g_forceLogoutOnNextRequest.exchange(false);
}

int GetActiveSessionCount() {
    std::lock_guard<std::mutex> lk(HttpServerInternal::g_sessionMutex);
    return static_cast<int>(HttpServerInternal::g_sessions.size());
}
