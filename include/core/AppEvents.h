#pragma once

#include <string>

namespace CoreHelpers {

struct LogEvent {
    std::string source;
    std::string message;
};

struct StatusEvent {
    bool ethercat_ok = false;
    bool busy = false;
    bool error = false;
    bool ready = false;
    bool home_done = false;
    bool online = false;
    bool master_initialized = false;
    bool worker_running = false;
};

}  // namespace CoreHelpers
