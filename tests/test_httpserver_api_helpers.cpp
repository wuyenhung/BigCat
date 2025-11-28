#include "server/HttpServer.Internal.h"

#include <windows.h>

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

bool Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

std::string MakeTaggedLine(int index, size_t payloadSize) {
    return std::string("line-") + std::to_string(index) + ":" +
           std::string(payloadSize, static_cast<char>('a' + (index % 26)));
}

}  // namespace

int main() {
    using namespace HttpServerInternal;

    if (!Check(TrimHttpValue(" \tstatus\r\n") == "status",
               "TrimHttpValue should strip surrounding whitespace")) {
        return EXIT_FAILURE;
    }

    std::string cache;
    AppendStatusCacheLine(cache, "first");
    AppendStatusCacheLine(cache, "second");
    if (!Check(cache == "first\nsecond",
               "AppendStatusCacheLine should preserve newline-separated history")) {
        return EXIT_FAILURE;
    }

    std::string oversized = "discard-this-line\n" + std::string(kStatusCacheMaxBytes, 'x') +
                            "\nkeep-this-line";
    NormalizeStatusCacheUtf8(oversized);
    if (!Check(oversized.size() <= kStatusCacheMaxBytes,
               "NormalizeStatusCacheUtf8 should bound cache size")) {
        return EXIT_FAILURE;
    }
    if (!Check(oversized.find("keep-this-line") != std::string::npos,
               "NormalizeStatusCacheUtf8 should retain the newest status text")) {
        return EXIT_FAILURE;
    }
    if (!Check(oversized.find("discard-this-line") == std::string::npos,
               "NormalizeStatusCacheUtf8 should discard the oldest status text first")) {
        return EXIT_FAILURE;
    }

    std::string appended;
    for (int index = 0; index < 200; ++index) {
        AppendStatusCacheLine(appended, MakeTaggedLine(index, 4096));
    }
    if (!Check(appended.size() <= kStatusCacheMaxBytes,
               "Repeated appends should stay within the configured cache budget")) {
        return EXIT_FAILURE;
    }
    if (!Check(appended.find("line-199:") != std::string::npos,
               "Repeated appends should keep the newest line")) {
        return EXIT_FAILURE;
    }
    if (!Check(appended.find("line-0:") == std::string::npos,
               "Repeated appends should evict the oldest lines once the cache is full")) {
        return EXIT_FAILURE;
    }

    // Stop-class endpoints must remain reachable while the system is in a
    // fault-reset-only state (manual_reset_required and/or error). The
    // desktop AutoWindow buttons and external 0x60FD bit0/bit5 inputs do
    // not gate on those flags; the web stop endpoints must match that
    // policy so the operator-visible web stop button never silently
    // becomes the only failing entry point.
    {
        CoreHelpers::SystemStateSnapshot lockedSnapshot;
        lockedSnapshot.master_initialized = true;
        lockedSnapshot.ethercat_ok = true;
        lockedSnapshot.error = true;
        lockedSnapshot.manual_reset_required = true;

        ApiCommandAction immediate = BuildAutoWindowMessageAction("/auto/immediate_stop", lockedSnapshot);
        if (!Check(immediate.handled,
                   "/auto/immediate_stop should be handled even when fault-reset-only")) {
            return EXIT_FAILURE;
        }
        if (!Check(immediate.responseKind == ApiResponseKind::kNoContent,
                   "/auto/immediate_stop should respond 204 No Content while fault-reset-only")) {
            return EXIT_FAILURE;
        }
        if (!Check(immediate.postedWindowMessage == static_cast<uint32_t>(WM_USER + 202),
                   "/auto/immediate_stop must post WM_USER_AUTO_IMMEDIATE_STOP_REQUEST (WM_USER+202)")) {
            return EXIT_FAILURE;
        }
        if (!Check(immediate.responseStatusLine.empty() && immediate.responseBody.empty(),
                   "/auto/immediate_stop must not return an HTTP error envelope while fault-reset-only")) {
            return EXIT_FAILURE;
        }

        ApiCommandAction emergency = BuildAutoWindowMessageAction("/auto/emergency_stop", lockedSnapshot);
        if (!Check(emergency.handled,
                   "/auto/emergency_stop should be handled even when fault-reset-only")) {
            return EXIT_FAILURE;
        }
        if (!Check(emergency.responseKind == ApiResponseKind::kNoContent,
                   "/auto/emergency_stop should respond 204 No Content while fault-reset-only")) {
            return EXIT_FAILURE;
        }
        if (!Check(emergency.postedWindowMessage == static_cast<uint32_t>(WM_USER + 207),
                   "/auto/emergency_stop must post WM_USER_AUTO_EMERGENCY_STOP_REQUEST (WM_USER+207)")) {
            return EXIT_FAILURE;
        }
        if (!Check(emergency.responseStatusLine.empty() && emergency.responseBody.empty(),
                   "/auto/emergency_stop must not return an HTTP error envelope while fault-reset-only")) {
            return EXIT_FAILURE;
        }
    }

    {
        CoreHelpers::SystemStateSnapshot normalSnapshot;
        normalSnapshot.master_initialized = true;
        normalSnapshot.ethercat_ok = true;
        normalSnapshot.online = true;

        ApiCommandAction immediate = BuildAutoWindowMessageAction("/auto/immediate_stop", normalSnapshot);
        if (!Check(immediate.handled && immediate.postedWindowMessage == static_cast<uint32_t>(WM_USER + 202),
                   "/auto/immediate_stop must remain reachable in the normal running state")) {
            return EXIT_FAILURE;
        }

        ApiCommandAction emergency = BuildAutoWindowMessageAction("/auto/emergency_stop", normalSnapshot);
        if (!Check(emergency.handled && emergency.postedWindowMessage == static_cast<uint32_t>(WM_USER + 207),
                   "/auto/emergency_stop must remain reachable in the normal running state")) {
            return EXIT_FAILURE;
        }

        ApiCommandAction unrelated = BuildAutoWindowMessageAction("/auto/unrelated", normalSnapshot);
        if (!Check(!unrelated.handled,
                   "BuildAutoWindowMessageAction must ignore unrelated routes")) {
            return EXIT_FAILURE;
        }
    }

    return EXIT_SUCCESS;
}