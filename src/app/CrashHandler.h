// Simple crash handler API
#pragma once

#ifndef CRASH_HANDLER_H
#define CRASH_HANDLER_H

#ifdef __cplusplus
extern "C" {
#endif

// Install a top-level unhandled-exception filter that writes a minidump
// when the process crashes. On MSVC/Windows this will produce a .dmp file
// next to the working directory. The project CMake option
// `ENABLE_CRASH_HANDLER` controls whether the source is compiled/linked.
void InstallCrashHandler(void);

// Convenience API that explicitly writes a minidump for the current
// process into the system temporary folder. Returns 1 on success, 0 on
// failure. This is useful for unit/integration tests and for forcing a
// dump from code paths without triggering an exception.
int WriteMinidump(const char* optionalSuffix);

#ifdef __cplusplus
}
#endif

#endif // CRASH_HANDLER_H
