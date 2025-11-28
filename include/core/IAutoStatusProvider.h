#pragma once

struct AutoStatusSnapshot {
    bool ethercat_ok = false;
    bool busy = false;
    bool error = false;
    bool manual_reset_required = false;
    bool ready = false;
    bool home_done = false;
    bool online = false;
};

class IAutoStatusProvider {
public:
    virtual ~IAutoStatusProvider() = default;
    virtual AutoStatusSnapshot GetAutoStatusSnapshot() const = 0;
};
