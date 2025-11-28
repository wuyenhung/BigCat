#include <algorithm>
#include <cctype>
#include <string>

#include "HttpServer.Internal.h"

namespace {

std::string ToLowerCopy(const std::string& value) {
    std::string lowered = value;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    return lowered;
}

std::string UrlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());

    auto hexToInt = [](char h) -> int {
        if (h >= '0' && h <= '9') {
            return h - '0';
        }
        return std::tolower(static_cast<unsigned char>(h)) - 'a' + 10;
    };

    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '%' && i + 2 < s.size()) {
            char a = s[i + 1];
            char b = s[i + 2];
            if (std::isxdigit(static_cast<unsigned char>(a)) != 0 &&
                std::isxdigit(static_cast<unsigned char>(b)) != 0) {
                out.push_back(static_cast<char>((hexToInt(a) << 4) | hexToInt(b)));
                i += 2;
                continue;
            }
        } else if (c == '+') {
            out.push_back(' ');
            continue;
        }

        out.push_back(c);
    }

    return out;
}

std::string GetHeaderValueFromRaw(const std::string& raw, const std::string& name) {
    std::string lower = raw;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });

    std::string lname = "\r\n" + name;
    std::transform(lname.begin(), lname.end(), lname.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });

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

bool IsHtmlDocumentPath(const std::string& relPath) {
    size_t dot = relPath.find_last_of('.');
    if (dot == std::string::npos) {
        return true;
    }

    const std::string ext = ToLowerCopy(relPath.substr(dot));
    return ext == ".html" || ext == ".htm";
}

bool HasHtmlNavigationAcceptHeader(const std::string& acceptHeader) {
    if (acceptHeader.empty()) {
        return true;
    }

    const std::string lowered = ToLowerCopy(acceptHeader);
    return lowered.find("text/html") != std::string::npos ||
           lowered.find("application/xhtml+xml") != std::string::npos;
}

}  // namespace

namespace HttpServerInternal {

std::string GetStaticRelativePath(const std::string& path) {
    std::string pathNoQuery = path;
    size_t qpos = pathNoQuery.find('?');
    if (qpos != std::string::npos) {
        pathNoQuery = pathNoQuery.substr(0, qpos);
    }

    if (pathNoQuery == "/" || pathNoQuery.empty()) {
        return "index.html";
    }

    if (!pathNoQuery.empty() && pathNoQuery[0] == '/') {
        pathNoQuery.erase(0, 1);
    }

    std::string relPath = UrlDecode(pathNoQuery);
    if (ToLowerCopy(relPath) == "favicon.ico") {
        return "ka52.ico";
    }

    return relPath;
}

bool IsApiRouteLike(const std::string& relPath) {
    if (relPath.empty()) {
        return false;
    }

    static const char* kRoutes[] = {
        "adapters",       "slaves",        "status",        "features",      "info",
        "conn_status",    "pdo_data",      "pdo_sse",       "ui_state",      "login",
        "logout",         "connect",       "disconnect",    "set_adapter",   "clear_status",
        "toggle_status",  "dump_esi",      "browse_esi",    "upload_esi",    "send_mailbox",
        "auto_connect",   "start_auto",    "stop_auto",     "set_ui_state",  "session_status",
        "check_force_logout",
        "select_slave",   "read_sdo",      "write_sdo",     "read_all_sdo",  "write_all_sdo",
        "set_period",
    };

    for (const char* route : kRoutes) {
        if (relPath == route) {
            return true;
        }
    }

    if (relPath.rfind("sdo", 0) == 0) {
        return true;
    }
    if (relPath.rfind("auto/", 0) == 0) {
        return true;
    }
    if (relPath.rfind("api/", 0) == 0) {
        return true;
    }

    return false;
}

bool IsForbiddenStaticPath(const std::string& relPath) {
    return relPath.find("..") != std::string::npos || relPath.find(":\\") != std::string::npos ||
           relPath.find(":/") != std::string::npos;
}

std::string GetStaticContentType(const std::string& filepath) {
    size_t dot = filepath.find_last_of('.');
    if (dot == std::string::npos) {
        return "application/octet-stream";
    }

    std::string ext = ToLowerCopy(filepath.substr(dot));
    if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".js") return "application/javascript; charset=utf-8";
    if (ext == ".json") return "application/json; charset=utf-8";
    if (ext == ".xml") return "application/xml; charset=utf-8";
    if (ext == ".txt") return "text/plain; charset=utf-8";
    if (ext == ".stl") return "application/octet-stream";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".gif") return "image/gif";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".ico") return "image/x-icon";

    return "application/octet-stream";
}

bool ShouldTrackPerIpConnection(const HttpRequestContext& ctx) {
    if (ctx.method != "GET") {
        return false;
    }

    const std::string relPath = GetStaticRelativePath(ctx.path);
    if (relPath.empty() || IsApiRouteLike(relPath) || IsForbiddenStaticPath(relPath) ||
        !IsHtmlDocumentPath(relPath)) {
        return false;
    }

    const std::string fetchDest = ToLowerCopy(GetHeaderValueFromRaw(ctx.requestRaw, "sec-fetch-dest"));
    if (!fetchDest.empty() && fetchDest != "document" && fetchDest != "iframe") {
        return false;
    }

    const std::string fetchMode = ToLowerCopy(GetHeaderValueFromRaw(ctx.requestRaw, "sec-fetch-mode"));
    if (!fetchMode.empty() && fetchMode != "navigate") {
        return false;
    }

    return HasHtmlNavigationAcceptHeader(GetHeaderValueFromRaw(ctx.requestRaw, "accept"));
}

}  // namespace HttpServerInternal