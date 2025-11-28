// CrashHandler.cpp - 簡要說明:
// 提供應用程式崩潰處理與 minidump 生成功能。此檔案會安裝頂層未處理例外篩選器 (SEH)、訊號處理器
// 以及 CRT 的錯誤處理器，當發生崩潰或錯誤時會嘗試將程序狀態寫入 .dmp 檔以便事後分析。
// 對外暴露的 API: InstallCrashHandler() 與 WriteMinidump().

#include "CrashHandler.h"
#include <windows.h>
#include <DbgHelp.h>
#include <stdio.h>
#include <string>
#include <signal.h>
#include <crtdbg.h>

#pragma comment(lib, "Dbghelp.lib")

static int WriteMinidumpInternal(EXCEPTION_POINTERS* pExceptionInfo, const char* optionalSuffix)
{
    char dumpPath[MAX_PATH];
    // Write dump to executable directory for easier discovery
    char exePath[MAX_PATH] = {0};
    DWORD exeLen = GetModuleFileNameA(NULL, exePath, MAX_PATH);
    char dumpDir[MAX_PATH] = {0};
    if (exeLen > 0 && exeLen < MAX_PATH) {
        // Extract directory from exe path
        strcpy_s(dumpDir, MAX_PATH, exePath);
        char* lastSlash = strrchr(dumpDir, '\\');
        if (lastSlash) {
            *(lastSlash + 1) = '\0'; // Keep trailing backslash
        } else {
            dumpDir[0] = '\0'; // Fallback to current directory
        }
    } else {
        // Fallback to current directory on failure
        dumpDir[0] = '\0';
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    if (optionalSuffix && optionalSuffix[0]) {
        sprintf_s(dumpPath, MAX_PATH, "%sBigCat_crash_%04d%02d%02d_%02d%02d%02d_%s.dmp",
            dumpDir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, optionalSuffix);
    } else {
        sprintf_s(dumpPath, MAX_PATH, "%sBigCat_crash_%04d%02d%02d_%02d%02d%02d.dmp",
            dumpDir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    }
    

    HANDLE hFile = CreateFileA(dumpPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        MINIDUMP_EXCEPTION_INFORMATION mdei;
        mdei.ThreadId = GetCurrentThreadId();
        mdei.ExceptionPointers = pExceptionInfo;
        mdei.ClientPointers = FALSE;

        // Use very rich dump flags including full memory to maximize the
        // chance cdb/windbg can resolve frames, variables and heap contents
        // during post-mortem analysis. Note: full memory dumps can be
        // large depending on process working set.
        MINIDUMP_TYPE dumpType = (MINIDUMP_TYPE)(
            MiniDumpWithFullMemory |
            MiniDumpWithDataSegs |
            MiniDumpWithHandleData |
            MiniDumpWithFullMemoryInfo |
            MiniDumpWithThreadInfo |
            MiniDumpWithUnloadedModules);

        BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile,
            dumpType, (pExceptionInfo ? &mdei : NULL), NULL, NULL);
        if (!ok) {
            char err[256];
            sprintf_s(err, sizeof(err), "MiniDumpWriteDump failed (GetLastError=%u)\n", (unsigned)GetLastError());
            OutputDebugStringA(err);
        }
        CloseHandle(hFile);
        return ok ? 1 : 0;
    }
    return 0;
}


int WriteMinidump(const char* optionalSuffix)
{
    // We can call the same internal helper with no exception pointers.
    return WriteMinidumpInternal(NULL, optionalSuffix);
}

static LONG WINAPI TopLevelExceptionHandler(EXCEPTION_POINTERS* pExceptionInfo)
{
    // Try to write a rich minidump. Ignore failures, let the system continue
    // with its normal exception handling so the crash remains visible.
    WriteMinidumpInternal(pExceptionInfo, NULL);
    return EXCEPTION_EXECUTE_HANDLER;
}

// Signal handler to capture abort(), pure virtual calls, etc.
static void SignalHandler(int sig)
{
    const char* sigName = "UNKNOWN";
    switch(sig) {
        case SIGABRT: sigName = "SIGABRT"; break;
        case SIGSEGV: sigName = "SIGSEGV"; break;
        case SIGILL:  sigName = "SIGILL"; break;
        case SIGFPE:  sigName = "SIGFPE"; break;
        case SIGTERM: sigName = "SIGTERM"; break;
    }
    
    char msg[256];
    sprintf_s(msg, sizeof(msg), "Signal handler triggered: %s (%d)\n", sigName, sig);
    OutputDebugStringA(msg);
    
    // Write minidump without exception pointers
    WriteMinidumpInternal(NULL, sigName);
    
    // Re-raise to allow normal crash reporting
    signal(sig, SIG_DFL);
    raise(sig);
}

// Capture pure virtual function calls
static void __cdecl PureCallHandler()
{
    OutputDebugStringA("Pure virtual function call detected\n");
    WriteMinidumpInternal(NULL, "purecall");
    abort();
}

// Capture invalid parameter handler
static void __cdecl InvalidParameterHandler(
    const wchar_t* /*expression*/,
    const wchar_t* /*function*/,
    const wchar_t* /*file*/,
    unsigned int /*line*/,
    uintptr_t /*pReserved*/)
{
    OutputDebugStringA("Invalid parameter detected\n");
    WriteMinidumpInternal(NULL, "invalid_param");
    abort();
}

void InstallCrashHandler(void)
{
    // SEH handler for access violations, etc.
    SetUnhandledExceptionFilter(TopLevelExceptionHandler);
    
    // Signal handlers for abort(), etc.
    signal(SIGABRT, SignalHandler);
    signal(SIGSEGV, SignalHandler);
    signal(SIGILL,  SignalHandler);
    signal(SIGFPE,  SignalHandler);
    signal(SIGTERM, SignalHandler);
    
    // CRT error handlers
    _set_purecall_handler(PureCallHandler);
    _set_invalid_parameter_handler(InvalidParameterHandler);
    
    // Disable CRT error reporting dialogs in debug builds
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    
    // Disable Windows Error Reporting dialog
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
}

// Ensure handler is installed even if nobody calls InstallCrashHandler()
// by using a static initializer.
struct _CrashInstaller {
    _CrashInstaller() { InstallCrashHandler(); }
};

static _CrashInstaller _crash_installer;
