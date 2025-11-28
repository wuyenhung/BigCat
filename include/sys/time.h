#pragma once
// Minimal sys/time.h shim for Windows to provide timeval and gettimeofday
// This is intended only to satisfy code that expects sys/time.h on POSIX.

#include <windows.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

struct timeval {
    long tv_sec;  /* seconds */
    long tv_usec; /* microseconds */
};

static inline int gettimeofday(struct timeval* tp, void* /*tzp*/) {
    if (!tp) return -1;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    // Convert FILETIME (100-nanosecond intervals since Jan 1, 1601) to
    // Unix epoch (seconds since Jan 1, 1970)
    unsigned long long tmp = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    // Windows epoch (1601) to Unix epoch (1970) in 100-ns
    const unsigned long long EPOCH_DIFF = 116444736000000000ULL;
    tmp -= EPOCH_DIFF;
    tp->tv_sec = (long)(tmp / 10000000ULL);
    tp->tv_usec = (long)((tmp % 10000000ULL) / 10ULL);
    return 0;
}

#ifdef __cplusplus
}
#endif
