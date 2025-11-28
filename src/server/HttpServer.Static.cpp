// HttpServer.Static.cpp - 靜態檔案服務。

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "HttpServer.Internal.h"

namespace {

std::string SafeReadFile(const std::string& path) {
    std::ifstream ifs(path, std::ios::in | std::ios::binary | std::ios::ate);
    if (!ifs) {
        return std::string();
    }

    std::streamsize size = ifs.tellg();
    if (size <= 0 || size > 64 * 1024 * 1024) {
        return std::string();
    }

    ifs.seekg(0, std::ios::beg);
    std::string content;
    content.resize(static_cast<size_t>(size));
    if (!ifs.read(&content[0], size)) {
        return std::string();
    }

    return content;
}

bool LoadWebFile(const std::string& relPath, std::string& content) {
    std::string exeDir = HttpServerInternal::GetExecutableDirA();

    if (!exeDir.empty()) {
        for (int up = 0; up <= 6; ++up) {
            std::string candidate;
            if (up == 0) {
                candidate = exeDir + "resources\\web\\" + relPath;
            } else {
                std::string prefix;
                for (int i = 0; i < up; ++i) {
                    prefix += "..\\";
                }
                candidate = exeDir + prefix + "resources\\web\\" + relPath;
            }

            content = SafeReadFile(candidate);
            if (!content.empty()) {
                return true;
            }
        }
    }

    content = SafeReadFile("resources\\web\\" + relPath);
    return !content.empty();
}

}  // namespace

namespace HttpServerInternal {

bool LoadStaticWebFile(const std::string& relPath, std::string& content) {
    std::string exeDir = HttpServerInternal::GetExecutableDirA();

    if (!exeDir.empty()) {
        for (int up = 0; up <= 6; ++up) {
            std::string candidate;
            if (up == 0) {
                candidate = exeDir + "resources\\web\\" + relPath;
            } else {
                std::string prefix;
                for (int i = 0; i < up; ++i) {
                    prefix += "..\\";
                }
                candidate = exeDir + prefix + "resources\\web\\" + relPath;
            }

            content = SafeReadFile(candidate);
            if (!content.empty()) {
                return true;
            }
        }
    }

    content = SafeReadFile("resources\\web\\" + relPath);
    return !content.empty();
}
RequestHandleResult TryHandleStaticRequest(const HttpRequestContext& ctx) {
    if (ctx.method != "GET") {
        return RequestHandleResult::kNotHandled;
    }

    std::string relPath = GetStaticRelativePath(ctx.path);

    if (relPath.empty()) {
        return RequestHandleResult::kNotHandled;
    }

    if (IsApiRouteLike(relPath)) {
        return RequestHandleResult::kNotHandled;
    }

    if (IsForbiddenStaticPath(relPath)) {
        const char* resp403 = "HTTP/1.1 403 Forbidden\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
        SendAll(ctx.client, resp403);
        return RequestHandleResult::kHandledClose;
    }

    std::string content;
    if (!LoadStaticWebFile(relPath, content)) {
        return RequestHandleResult::kNotHandled;
    }

    std::string contentType = GetStaticContentType(relPath);
    std::ostringstream hdr;
    hdr << "HTTP/1.1 200 OK\r\n"
        << "Connection: close\r\n"
        << "Cache-Control: public, max-age=300\r\n"
        << "Content-Type: " << contentType << "\r\n"
        << "Content-Length: " << content.size() << "\r\n\r\n";

    std::string header = hdr.str();
    if (!SendAll(ctx.client, header) || !SendAll(ctx.client, content)) {
        return RequestHandleResult::kHandledClose;
    }
    return RequestHandleResult::kHandledClose;
}

}  // namespace HttpServerInternal
