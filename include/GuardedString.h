#pragma once

// GuardedString — value-semantics UTF-8 string whose every access is serialized by
// a mutex. Used for state that is written and read by different threads, so a plain
// std::string would be a data race (concurrent assignment + read can corrupt the
// heap, not just return stale text).
//
// Current user: MainWindow::currentIfName / MainWindow::webSelectedIfName — the
// adapter selection is written by the UI thread, the connect worker and HTTP
// workers (SetSelectedAdapter) and read by the UI thread, the watchdog/link monitor,
// the HTTP API and the SSE broadcaster (repo-facts 2026-10-03).
//
// Usage stays close to std::string:
//   GuardedString name;
//   name = "value";                  // locked assignment
//   if (!name.empty()) { ... }       // locked query
//   std::string copy = name;         // locked copy (implicit conversion)
//   Use(name);                       // const std::string& parameter: locked copy
//
// NOTE: there is intentionally no c_str()/data(). A pointer returned under the lock
// could outlive it. Take a copy first:
//   const std::string ifname = cache.Copy();
//   ... CheckLinkStatus(ifname.c_str()) ...

#include <mutex>
#include <ostream>
#include <string>

class GuardedString {
public:
    GuardedString() = default;
    ~GuardedString() = default;

    // Non-copyable/non-movable: the mutex must stay with the owned value.
    GuardedString(const GuardedString&) = delete;
    GuardedString& operator=(const GuardedString&) = delete;
    GuardedString(GuardedString&&) = delete;
    GuardedString& operator=(GuardedString&&) = delete;

    GuardedString& operator=(const std::string& value) {
        std::lock_guard<std::mutex> lk(mtx_);
        value_ = value;
        return *this;
    }

    GuardedString& operator=(const char* value) {
        std::lock_guard<std::mutex> lk(mtx_);
        value_ = (value != nullptr) ? std::string(value) : std::string();
        return *this;
    }

    // Snapshot the current value. The caller owns the returned copy, so it can be
    // used (c_str(), comparison, passing on) outside the lock.
    std::string Copy() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return value_;
    }

    // Implicit value conversion keeps ordinary read usage source-compatible.
    operator std::string() const { return Copy(); }

    bool empty() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return value_.empty();
    }

    void clear() {
        std::lock_guard<std::mutex> lk(mtx_);
        value_.clear();
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return value_.size();
    }

private:
    mutable std::mutex mtx_;
    std::string value_;
};

// Companions so the wrapper behaves like a value in the usual comparisons/logging.
// These are non-template overloads on purpose: the std::basic_string operators are
// templates, so they cannot deduce from GuardedString. Each one takes a locked copy.
inline bool operator==(const GuardedString& a, const std::string& b) { return a.Copy() == b; }
inline bool operator==(const std::string& a, const GuardedString& b) { return a == b.Copy(); }
inline bool operator!=(const GuardedString& a, const std::string& b) { return a.Copy() != b; }
inline bool operator!=(const std::string& a, const GuardedString& b) { return a != b.Copy(); }
inline bool operator==(const GuardedString& a, const char* b) { return a.Copy() == std::string(b ? b : ""); }
inline bool operator!=(const GuardedString& a, const char* b) { return !(a == b); }
inline bool operator==(const char* a, const GuardedString& b) { return std::string(a ? a : "") == b.Copy(); }
inline bool operator!=(const char* a, const GuardedString& b) { return !(a == b); }
inline std::ostream& operator<<(std::ostream& os, const GuardedString& value) { return os << value.Copy(); }
