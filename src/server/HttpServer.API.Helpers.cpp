#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>

#include "HttpServer.Internal.h"

namespace HttpServerInternal {

namespace {

constexpr size_t kStatusCacheCompactThresholdBytes = kStatusCacheMaxBytes * 2;

const char* ToHttpErrorJson(CoreHelpers::CommandPolicyRejection rejection) {
    switch (rejection) {
    case CoreHelpers::CommandPolicyRejection::kEtherCATNotOk:
        return "{\"error\":\"EtherCAT not OK\"}";
    case CoreHelpers::CommandPolicyRejection::kSystemBusy:
        return "{\"error\":\"system busy\"}";
    case CoreHelpers::CommandPolicyRejection::kSystemError:
        return "{\"error\":\"system error\"}";
    case CoreHelpers::CommandPolicyRejection::kResetNotNeeded:
        return "{\"error\":\"system ready and no error - reset not needed\"}";
    default:
        break;
    }

    return "{\"error\":\"preconditions not satisfied\"}";
}

std::string BuildAutoConnectErrorJson(const CoreHelpers::SystemStateSnapshot& snapshot) {
    std::ostringstream ss;
    ss << "{\"error\":\"preconditions not satisfied\"";
    ss << ",\"oEtherCatOk\":" << (snapshot.ethercat_ok ? "true" : "false");
    ss << ",\"oReady\":" << (snapshot.ready ? "true" : "false");
    ss << ",\"oHomeDone\":" << (snapshot.home_done ? "true" : "false");
    ss << ",\"oError\":" << (snapshot.error ? "true" : "false");
    ss << ",\"oBusy\":" << (snapshot.busy ? "true" : "false");
    ss << ",\"oOnline\":" << (snapshot.online ? "true" : "false");
    ss << "}";
    return ss.str();
}

}  // namespace

std::string TrimHttpValue(const std::string& s) {
    size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin])) != 0) {
        ++begin;
    }

    size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1])) != 0) {
        --end;
    }

    return s.substr(begin, end - begin);
}

int ParseSlaveFromPath(const std::string& path) {
    size_t qpos = path.find('?');
    if (qpos == std::string::npos) {
        return -1;
    }

    std::string queryString = path.substr(qpos + 1);
    size_t slavePos = queryString.find("slave=");
    if (slavePos == std::string::npos) {
        return -1;
    }

    slavePos += std::strlen("slave=");
    size_t ampersandPos = queryString.find('&', slavePos);
    std::string value = ampersandPos == std::string::npos
                            ? queryString.substr(slavePos)
                            : queryString.substr(slavePos, ampersandPos - slavePos);

    try {
        return std::stoi(value);
    } catch (...) {
        return -1;
    }
}

bool TryParseCyclePeriodUs(const std::string& body, uint32_t& periodUs) {
    std::string value = TrimHttpValue(body);
    if (value.empty()) {
        return false;
    }

    try {
        double periodMs = std::stod(value);
        if (periodMs < 0.5 || periodMs > 10.0) {
            periodMs = 2.0;
        }

        periodUs = static_cast<uint32_t>(std::round(periodMs * 1000.0));
        return true;
    } catch (...) {
        return false;
    }
}

std::string ExtractSelectedSlaveValue(const std::string& body) {
    size_t keyPos = body.find("\"selected\"");
    if (keyPos == std::string::npos) {
        return std::string();
    }

    size_t colonPos = body.find(':', keyPos);
    if (colonPos == std::string::npos) {
        return std::string();
    }

    size_t valueStart = body.find('"', colonPos);
    if (valueStart == std::string::npos) {
        return std::string();
    }

    size_t valueEnd = body.find('"', valueStart + 1);
    if (valueEnd == std::string::npos) {
        return std::string();
    }

    return body.substr(valueStart + 1, valueEnd - (valueStart + 1));
}

void NormalizeStatusCacheUtf8(std::string& utf8) {
    if (utf8.size() > kStatusCacheMaxBytes) {
        const size_t overflow = utf8.size() - kStatusCacheMaxBytes;
        const size_t lineBreak = utf8.find('\n', overflow);
        if (lineBreak == std::string::npos) {
            utf8.erase(0, overflow);
        } else {
            utf8.erase(0, lineBreak + 1);
        }
    }

    if (utf8.capacity() > kStatusCacheCompactThresholdBytes) {
        std::string compacted(utf8);
        utf8.swap(compacted);
    }
}

void AppendStatusCacheLine(std::string& utf8, const std::string& line) {
    if (line.empty()) {
        return;
    }

    if (!utf8.empty()) {
        utf8.push_back('\n');
    }
    utf8 += line;
    NormalizeStatusCacheUtf8(utf8);
}

const char* GetDirectUiCommandForRoute(const std::string& path) {
    if (path == "/clear_status") return "HTTP_CMD:CLEAR_STATUS";
    if (path == "/start_auto") return "HTTP_CMD:START_AUTO";
    if (path == "/stop_auto") return "HTTP_CMD:STOP_AUTO";
    if (path == "/toggle_status") return "HTTP_CMD:TOGGLE_STATUS";
    if (path == "/dump_esi") return "HTTP_CMD:DUMP_ESI";
    if (path == "/browse_esi") return "HTTP_CMD:BROWSE_ESI";
    if (path == "/disconnect") return "HTTP_CMD:DISCONNECT";
    if (path == "/read_sdo") return "HTTP_CMD:READ_SDO";
    if (path == "/write_sdo") return "HTTP_CMD:WRITE_SDO";
    if (path == "/read_all_sdo") return "HTTP_CMD:READ_ALL_SDO";
    if (path == "/write_all_sdo") return "HTTP_CMD:WRITE_ALL_SDO";
    return nullptr;
}

const char* GetMmfCommandForRoute(const std::string& path) {
    if (path == "/api/mmf/enable") return "HTTP_CMD:MMF_ENABLE";
    if (path == "/api/mmf/disable") return "HTTP_CMD:MMF_DISABLE";
    return nullptr;
}

ApiCommandAction BuildApiCommandAction(const std::string& path, const std::string& body) {
    ApiCommandAction action;

    if (path == "/send_mailbox") {
        action.handled = true;
        action.responseKind = ApiResponseKind::kNoContent;
        if (!body.empty()) {
            action.shouldCacheMailbox = true;
            action.mailboxUtf8 = body;
            action.command = std::string("HTTP_CMD:MAILBOX:") + body;
        } else {
            action.command = "HTTP_CMD:SEND_MAILBOX";
        }
        return action;
    }

    if (path == "/set_period") {
        action.handled = true;
        action.responseKind = ApiResponseKind::kNoContent;
        uint32_t periodUs = 0;
        if (TryParseCyclePeriodUs(body, periodUs)) {
            action.shouldUpdateDesiredCycle = true;
            action.desiredCycleUs = periodUs;
            action.command = std::string("HTTP_CMD:SET_PERIOD:") + std::to_string(periodUs);
        }
        return action;
    }

    if (path == "/select_slave") {
        action.handled = true;
        action.responseKind = ApiResponseKind::kNoContent;
        std::string selectedValue = ExtractSelectedSlaveValue(body);
        if (!selectedValue.empty()) {
            action.command = std::string("HTTP_CMD:SELECT_SLAVE:") + selectedValue;
        }
        return action;
    }

    if (const char* directCommand = GetDirectUiCommandForRoute(path)) {
        action.handled = true;
        action.responseKind = ApiResponseKind::kNoContent;
        action.command = directCommand;
        return action;
    }

    if (const char* mmfCommand = GetMmfCommandForRoute(path)) {
        action.handled = true;
        action.responseKind = ApiResponseKind::kJson;
        action.responseStatusLine = "HTTP/1.1 200 OK";
        if (path == "/api/mmf/enable") {
            action.responseBody = "{\"status\":\"enabled\"}";
        } else {
            action.responseBody = "{\"status\":\"disabled\"}";
        }
        action.command = mmfCommand;
        return action;
    }

    return action;
}

ApiCommandAction BuildConnectAction(bool busy, bool ethercatOk, const std::string& webAdapterUtf8) {
    ApiCommandAction action;
    action.handled = true;

    if (busy || ethercatOk) {
        action.responseKind = ApiResponseKind::kJson;
        action.responseStatusLine = "HTTP/1.1 409 Conflict";
        action.responseBody = std::string("{\"error\":\"cannot connect: already ") +
                              (busy ? "busy (connecting/disconnecting)" : "connected") +
                              "\"}";
        return action;
    }

    action.responseKind = ApiResponseKind::kNoContent;
    if (!webAdapterUtf8.empty()) {
        action.command = std::string("HTTP_CMD:CONNECT_WEB:") + webAdapterUtf8;
    } else {
        action.command = "HTTP_CMD:CONNECT";
    }
    return action;
}

ApiCommandAction BuildAutoConnectAction(const CoreHelpers::SystemStateSnapshot& snapshot) {
    ApiCommandAction action;
    action.handled = true;

    if (!IsAutoConnectRequestAllowed(snapshot)) {
        action.responseKind = ApiResponseKind::kJson;
        action.responseStatusLine = "HTTP/1.1 400 Bad Request";
        action.responseBody = BuildAutoConnectErrorJson(snapshot);
        return action;
    }

    action.responseKind = ApiResponseKind::kNoContent;
    action.command = "HTTP_CMD:AUTO_CONNECT";
    return action;
}

ApiCommandAction BuildAutoWindowMessageAction(const std::string& path, const CoreHelpers::SystemStateSnapshot& snapshot) {
    ApiCommandAction action;
    (void)snapshot;

    // Stop-class endpoints must remain reachable even when the system is in a
    // fault-reset-only state (snapshot.error / manual_reset_required). The
    // downstream MainWindow handlers already handle missing master, busy
    // bits, and latched stop state safely; an HTTP 400 here would silently
    // disable the only operator-visible web stop button while the desktop
    // AutoWindow buttons and the external 0x60FD bit0/bit5 inputs continue
    // to work, which violates the established stop-bypass policy.
    if (path == "/auto/immediate_stop") {
        action.handled = true;
        action.responseKind = ApiResponseKind::kNoContent;
        action.postedWindowMessage = WM_USER + 202;
        return action;
    }

    if (path == "/auto/emergency_stop") {
        action.handled = true;
        action.responseKind = ApiResponseKind::kNoContent;
        action.postedWindowMessage = WM_USER + 207;
        return action;
    }

    return action;
}

ApiCommandAction BuildAutoFaultResetAction(const CoreHelpers::SystemStateSnapshot& snapshot) {
    ApiCommandAction action;
    action.handled = true;

    if (const char* errorJson = ValidateAutoFaultResetRequest(snapshot)) {
        action.responseKind = ApiResponseKind::kJson;
        action.responseStatusLine = "HTTP/1.1 400 Bad Request";
        action.responseBody = errorJson;
        return action;
    }

    action.responseKind = ApiResponseKind::kNoContent;
    action.command = "HTTP_CMD:AUTO_FAULT_RESET";
    return action;
}

ApiCommandAction BuildAutoDownToOriginAction(const CoreHelpers::SystemStateSnapshot& snapshot) {
    ApiCommandAction action;
    action.handled = true;

    if (const char* errorJson = ValidateAutoDownToOriginRequest(snapshot)) {
        action.responseKind = ApiResponseKind::kJson;
        action.responseStatusLine = "HTTP/1.1 400 Bad Request";
        action.responseBody = errorJson;
        return action;
    }

    action.responseKind = ApiResponseKind::kNoContent;
    action.command = "HTTP_CMD:AUTO_DOWN_TO_ORIGIN";
    return action;
}

bool IsAutoConnectUiEnabled(const CoreHelpers::SystemStateSnapshot& snapshot) {
    return CoreHelpers::EvaluateAutoConnect(snapshot).allowed;
}

bool IsAutoConnectRequestAllowed(const CoreHelpers::SystemStateSnapshot& snapshot) {
    return CoreHelpers::EvaluateAutoConnect(snapshot).allowed;
}

const char* ValidateAutoFaultResetRequest(const CoreHelpers::SystemStateSnapshot& snapshot) {
    const auto decision = CoreHelpers::EvaluateAutoFaultReset(snapshot);
    if (decision.allowed) {
        return nullptr;
    }
    return ToHttpErrorJson(decision.rejection);
}

const char* ValidateAutoDownToOriginRequest(const CoreHelpers::SystemStateSnapshot& snapshot) {
    const auto decision = CoreHelpers::EvaluateAutoDownToOrigin(snapshot);
    if (decision.allowed) {
        return nullptr;
    }
    return ToHttpErrorJson(decision.rejection);
}

}  // namespace HttpServerInternal
