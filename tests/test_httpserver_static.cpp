#include "server/HttpServer.Internal.h"

#include <cstdlib>
#include <iostream>

namespace {

bool Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

HttpServerInternal::HttpRequestContext MakeRequest(const std::string& method,
                                                   const std::string& path,
                                                   const std::string& headers) {
    HttpServerInternal::HttpRequestContext ctx;
    ctx.method = method;
    ctx.path = path;
    ctx.requestRaw = method + " " + path + " HTTP/1.1\r\nHost: localhost\r\n" + headers + "\r\n";
    return ctx;
}

}  // namespace

int main() {
    using namespace HttpServerInternal;

    if (!Check(GetStaticRelativePath("/") == "index.html", "root path should resolve to index.html") ||
        !Check(GetStaticRelativePath("/auto.html?ts=1") == "auto.html", "query string should be removed from static paths") ||
        !Check(!IsApiRouteLike("auth.js"), "static JS asset should not be classified as API") ||
        !Check(IsApiRouteLike("session_status"), "session_status should remain an API route") ||
        !Check(IsForbiddenStaticPath("../secret.txt"), "path traversal should remain forbidden")) {
        return EXIT_FAILURE;
    }

    const HttpRequestContext topLevelHtml = MakeRequest(
        "GET",
        "/auto.html",
        "Accept: text/html\r\nSec-Fetch-Dest: document\r\nSec-Fetch-Mode: navigate");
    if (!Check(ShouldTrackPerIpConnection(topLevelHtml),
               "top-level HTML navigation should participate in per-IP replacement")) {
        return EXIT_FAILURE;
    }

    const HttpRequestContext legacyHtml = MakeRequest("GET", "/status.html", "Accept: text/html");
    if (!Check(ShouldTrackPerIpConnection(legacyHtml),
               "HTML pages without modern fetch headers should still be tracked")) {
        return EXIT_FAILURE;
    }

    const HttpRequestContext scriptAsset = MakeRequest(
        "GET",
        "/auth.js",
        "Accept: */*\r\nSec-Fetch-Dest: script\r\nSec-Fetch-Mode: no-cors");
    if (!Check(!ShouldTrackPerIpConnection(scriptAsset),
               "static script assets must not evict same-IP page loads")) {
        return EXIT_FAILURE;
    }

    const HttpRequestContext stylesheetAsset = MakeRequest(
        "GET",
        "/sidebar-menu.css",
        "Accept: text/css\r\nSec-Fetch-Dest: style\r\nSec-Fetch-Mode: no-cors");
    if (!Check(!ShouldTrackPerIpConnection(stylesheetAsset),
               "stylesheet assets must not participate in same-IP replacement")) {
        return EXIT_FAILURE;
    }

    const HttpRequestContext apiFetch = MakeRequest(
        "GET",
        "/session_status",
        "Accept: application/json\r\nSec-Fetch-Dest: empty\r\nSec-Fetch-Mode: cors");
    if (!Check(!ShouldTrackPerIpConnection(apiFetch),
               "API fetches must not evict same-IP page loads")) {
        return EXIT_FAILURE;
    }

    const HttpRequestContext mismatchedFetch = MakeRequest(
        "GET",
        "/index.html",
        "Accept: */*\r\nSec-Fetch-Dest: script\r\nSec-Fetch-Mode: no-cors");
    if (!Check(!ShouldTrackPerIpConnection(mismatchedFetch),
               "non-document fetch headers should prevent HTML replacement tracking")) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}