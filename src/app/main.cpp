// main.cpp - 簡要說明:
// Windows 應用程式進入點，負責初始化單例檢查、TPM 驗證、CPU 核心隔離，並啟動主視窗與 EtherCAT 邏輯。

#include "MainWindow.h"
#include "EtherCATMaster.h"
// #include "core/CpuIsolation.h" - removed
#include <windows.h>
#include <thread>
#include "SafeMem.h"
#include "LicenseChecker.h"
#include "HttpServer.h"
#include "AppController.h"
#include "core/AppConfig.h"
// Core helpers (Utf8/Wide conversion used to launch browser URL)
#include "../include/core/CoreHelpers.h"

static void AppendRestrictionReason(std::string& combined, const std::string& reason) {
    if (reason.empty()) return;
    if (!combined.empty()) combined += "; ";
    combined += reason;
}

// Helper: show a message box that reliably appears on top by creating a
// temporary topmost owner window and using it as the MessageBox owner.
static int ShowTopmostMessageBox(const std::wstring &text, const std::wstring &caption, UINT uType) {
    HINSTANCE hInst = GetModuleHandle(NULL);
    // Create a tiny popup window as owner. Use TOOLWINDOW so it doesn't appear in taskbar.
    HWND hOwner = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                                  L"STATIC", L"", WS_POPUP,
                                  0, 0, 1, 1, NULL, NULL, hInst, NULL);
    if (hOwner) {
        // Ensure it's topmost (SetWindowPos is sometimes needed)
        SetWindowPos(hOwner, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        // Hide the window (it's only used as an owner)
        ShowWindow(hOwner, SW_HIDE);
        int r = MessageBoxW(hOwner, text.c_str(), caption.c_str(), uType | MB_TOPMOST | MB_SETFOREGROUND);
        DestroyWindow(hOwner);
        return r;
    }
    // Fallback
    return MessageBoxW(NULL, text.c_str(), caption.c_str(), uType | MB_TOPMOST | MB_SETFOREGROUND | MB_SYSTEMMODAL);
}

// CPU feature detection helpers - used at startup to reject incompatible CPUs early
#include <vector>
#include <sstream>
#ifdef _MSC_VER
  #include <intrin.h>
#endif

static void cpuid(int info[4], int leaf, int subleaf = 0) {
#ifdef _MSC_VER
    __cpuidex(info, leaf, subleaf);
#else
    __cpuid_count(leaf, subleaf, info[0], info[1], info[2], info[3]);
#endif
}

static unsigned long long xgetbv(unsigned int index) {
#ifdef _MSC_VER
    return _xgetbv(index);
#else
    unsigned int eax = 0, edx = 0;
    __asm__ ("xgetbv" : "=a"(eax), "=d"(edx) : "c"(index));
    return ((unsigned long long)edx << 32) | eax;
#endif
}

static bool has_sse2() {
    int info[4] = {0}; cpuid(info, 1, 0);
    return (info[3] & (1 << 26)) != 0;
}
static bool has_sse4_1() {
    int info[4] = {0}; cpuid(info, 1, 0);
    return (info[2] & (1 << 19)) != 0;
}
static bool has_fma() {
    int info[4] = {0}; cpuid(info, 1, 0);
    return (info[2] & (1 << 12)) != 0;
}
static bool has_avx() {
    int info[4] = {0}; cpuid(info, 1, 0);
    bool osxsave = (info[2] & (1 << 27)) != 0;
    bool avx = (info[2] & (1 << 28)) != 0;
    if (!osxsave || !avx) return false;
    unsigned long long xcr = xgetbv(0);
    // Check XCR0 bits 1 and 2 are set (XMM and YMM state enabled by OS)
    return ( (xcr & 0x6ULL) == 0x6ULL );
}
static bool has_avx2() {
    int info[4] = {0}; cpuid(info, 7, 0);
    return (info[1] & (1 << 5)) != 0; // EBX bit 5
}
static bool has_bmi2() {
    int info[4] = {0}; cpuid(info, 7, 0);
    return (info[1] & (1 << 8)) != 0; // EBX bit 8
}

static std::wstring Utf8ToWide(const std::string &s) {
    if (s.empty()) return std::wstring();
    int needed = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, NULL, 0);
    if (needed <= 0) return std::wstring();
    std::wstring out; out.resize(needed - 1);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &out[0], needed);
    return out;
}

static bool CheckCpuCompatibility(std::string &outReason) {
    // Testing hook: set BIGCAT_TEST_FORCE_CPU_INCOMPATIBLE=1 to force a fail-path for manual testing
    char testBuf[8] = {0};
    if (GetEnvironmentVariableA("BIGCAT_TEST_FORCE_CPU_INCOMPATIBLE", testBuf, (DWORD)sizeof(testBuf)) > 0) {
        outReason = "(測試) 強制拒絕 CPU 相容性測試 - BIGCAT_TEST_FORCE_CPU_INCOMPATIBLE=1";
        return false;
    }

    std::vector<std::string> required;
    // Always ensure SSE2 baseline for x86 builds
#if defined(_M_X64) || defined(__x86_64__) || defined(__amd64__)
    // x64 requires SSE2 by ABI
    required.push_back("SSE2");
#endif
    // Detect features the binary was compiled for via predefined macros
#if defined(__AVX2__) || defined(_MSC_VER) && defined(__AVX2__)
    required.push_back("AVX2");
#elif defined(__AVX__) || defined(_MSC_VER) && defined(__AVX__)
    required.push_back("AVX");
#endif
#if defined(__SSE4_1__)
    required.push_back("SSE4.1");
#endif
#if defined(__FMA__)
    required.push_back("FMA");
#endif

    // If no special requirements detected, be conservative and only require SSE2
    if (required.empty()) {
        required.push_back("SSE2");
    }

    std::vector<std::string> missing;
    for (const auto &f : required) {
        if (f == "SSE2" && !has_sse2()) missing.push_back(f);
        else if (f == "SSE4.1" && !has_sse4_1()) missing.push_back(f);
        else if (f == "FMA" && !has_fma()) missing.push_back(f);
        else if (f == "AVX" && !has_avx()) missing.push_back(f);
        else if (f == "AVX2" && !has_avx2()) missing.push_back(f);
    }

    if (!missing.empty()) {
        std::ostringstream ss;
        ss << "此程式在您目前的系統上無法執行。\n\n";
        ss << "缺少以下 CPU 指令集或 OS 支援 (請在支援的硬體上執行或重新編譯以較低特性)：\n";
        for (const auto &m : missing) ss << " - " << m << "\n";
        ss << "\n建議: 使用支援這些指令集的 CPU，或聯絡開發團隊取得非加速版本。";
        outReason = ss.str();
        return false;
    }
    return true;
}

// 將 EtherCATMaster 移到 main 範圍以移除不必要的全域定義

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, 
                   LPSTR lpCmdLine, int nCmdShow) {
    // Mark unused WinMain parameters as used to avoid C4100 warnings
    (void)hPrevInstance;
    (void)lpCmdLine;
    
    // ========================================================================
    // 單例檢查 (提前執行)：使用命名 Mutex 避免程式雙開
    // 重要：把單例檢查移到最早期，這樣可以避免執行昂貴或改變系統狀態的初始化
    // 當另一個 instance 存在於同一台機器（跨 session）時，立即退出。
    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"Global\\BigCat_Singleton_Mutex");
    if (hMutex == NULL) {
        MessageBoxW(NULL, L"無法建立 Mutex", L"錯誤", MB_OK | MB_ICONERROR);
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND hExisting = FindWindowW(L"EtherCATWindowClass", NULL);
        if (hExisting) {
            PostMessageW(hExisting, WM_USER_RESTORE_WINDOW, 0, 0);
        }
        CloseHandle(hMutex);
        return 0;
    }

    bool physicalEthercatAllowed = true;
    std::string physicalEthercatRestrictionReason;

    // TPM/CPU checks no longer prevent simulation mode from starting. They only
    // disable physical EtherCAT communication before SOEM/Npcap touches a NIC.
    if (!LicenseChecker::IsTpmReady()) {
        physicalEthercatAllowed = false;
        AppendRestrictionReason(physicalEthercatRestrictionReason, "TPM not enabled or ready");
        ShowTopmostMessageBox(L"未偵測到已啟用且準備就緒的 TPM (可信平台模組)。\n\n程式將以模擬測試模式繼續執行，但會停用實體 EtherCAT 通訊。\n若要連接實體 EtherCAT，請在 BIOS/UEFI 與 Windows 中啟用並初始化 TPM。", L"TPM 未就緒 - 僅允許模擬", MB_OK | MB_ICONWARNING);
    }

    // CPU compatibility check: missing optional CPU features also puts the app
    // into simulation-only mode instead of blocking startup.
    {
        std::string reason;
        if (!CheckCpuCompatibility(reason)) {
            physicalEthercatAllowed = false;
            AppendRestrictionReason(physicalEthercatRestrictionReason, reason);
            std::wstring wreason = Utf8ToWide(reason + "\n\n程式將以模擬測試模式繼續執行，但會停用實體 EtherCAT 通訊。");
            ShowTopmostMessageBox(wreason, L"CPU 相容性限制 - 僅允許模擬", MB_OK | MB_ICONWARNING);
        }
    }

    EtherCATMaster::SetPhysicalEthercatAllowed(physicalEthercatAllowed, physicalEthercatRestrictionReason);

    // ========================================================================
    // CPU 核心隔離初始化
    // ========================================================================
    // 新版實作已移至 MainWindow::Create() 中呼叫 CoreHelpers::CpuIsolation::StartIsolationMonitor()
    // 此處舊有邏輯移除，避免編譯錯誤與邏輯衝突。

    // 授權檢查：使用 LicenseChecker（TPM2 為主，WMI fallback）來取得 device fingerprint
    // 驗證流程：若環境變數 BIGCAT_DISABLE_LICENSE_CHECK 設定或 cmdline 包含 "wuyenhung" 則允許（開發便利）；
    // 否則預期命令列中包含 64 字元 hex（SHA-256）或環境變數 BIGCAT_LICENSE_KEY，與本機 fingerprint 做比對。
    {
        std::string cmdline = lpCmdLine ? std::string(lpCmdLine) : std::string();

        // Auto-load license from file (exe dir or overridden by BIGCAT_LICENSE_FILE)
        auto TryLoadLicenseFile = []() -> bool {
            // Check explicit env var path first
            char pathBuf[_MAX_PATH] = {0};
            DWORD r = GetEnvironmentVariableA("BIGCAT_LICENSE_FILE", pathBuf, (DWORD)sizeof(pathBuf));
            std::string candidatePath;
            if (r > 0 && r < sizeof(pathBuf)) {
                candidatePath = std::string(pathBuf);
            } else {
                // construct exe directory path + license.txt
                char exePath[_MAX_PATH] = {0};
                if (GetModuleFileNameA(NULL, exePath, (DWORD)sizeof(exePath))) {
                    std::string p(exePath);
                    size_t pos = p.find_last_of("\\/");
                    std::string dir = (pos == std::string::npos) ? std::string(".") : p.substr(0, pos);
                    candidatePath = dir + "\\license.txt";
                }
            }

            if (candidatePath.empty()) return false;
            // Read first non-empty line
            FILE* f = nullptr;
            if (fopen_s(&f, candidatePath.c_str(), "r") != 0 || !f) {
                return false;
            }
            char buf[1024];
            std::string line;
            while (fgets(buf, sizeof(buf), f)) {
                std::string s(buf);
                // trim whitespace
                auto l = s.find_first_not_of(" \t\r\n");
                if (l == std::string::npos) continue;
                auto rpos = s.find_last_not_of(" \t\r\n");
                line = s.substr(l, rpos - l + 1);
                break;
            }
            fclose(f);
            if (line.empty()) return false;

            // If it looks like a 64-char hex, set it into process env so VerifyLicenseFromCmdLine picks it up
            bool looks64 = (line.size() == 64);
            if (looks64) {
                SetEnvironmentVariableA("BIGCAT_LICENSE_KEY", line.c_str());
                return true;
            }
            // Not a valid-looking key
            return false;
        };

        // If license file is present and valid it will set BIGCAT_LICENSE_KEY for the process
        TryLoadLicenseFile();
        bool ok = LicenseChecker::VerifyLicenseFromCmdLine(cmdline);
        if (!ok) {
            // 授權驗證失敗：第一次先顯示警告訊息
            // 接下來會在背景啟動計時器，1分鐘後再次警告並停用 MMF 輸入
            std::string fingerprint = LicenseChecker::GetDeviceFingerprint();
            if (fingerprint.empty()) {
                fingerprint = LicenseChecker::GetDeviceFingerprintWmiOnly();
            }
            std::string licenseKey = LicenseChecker::ComputeSHA256Hex(fingerprint);

            // Convert to wide strings (UTF-8 -> UTF-16)
            auto ToWide = [](const std::string &s)->std::wstring {
                if (s.empty()) return std::wstring();
                int needed = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, NULL, 0);
                if (needed <= 0) return std::wstring();
                std::wstring out; out.resize(needed - 1);
                MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &out[0], needed);
                return out;
            };

            std::wstring wfinger = ToWide(fingerprint);
            std::wstring wkey = ToWide(licenseKey);
            std::wstring copyText = wfinger;

            auto CopyToClipboard = [&](const std::wstring &text)->bool {
                if (text.empty()) return false;
                if (!OpenClipboard(NULL)) return false;
                if (!EmptyClipboard()) { CloseClipboard(); return false; }
                size_t cb = (text.size() + 1) * sizeof(wchar_t);
                HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, cb);
                if (!hg) { CloseClipboard(); return false; }
                void* ptr = GlobalLock(hg);
                if (!ptr) { GlobalFree(hg); CloseClipboard(); return false; }
                if (!safe_memcpy(ptr, text.c_str(), cb)) { GlobalUnlock(hg); GlobalFree(hg); CloseClipboard(); return false; }
                GlobalUnlock(hg);
                if (!SetClipboardData(CF_UNICODETEXT, hg)) { GlobalFree(hg); CloseClipboard(); return false; }
                CloseClipboard();
                return true;
            };

            bool copied = CopyToClipboard(copyText);

            std::wstring message;
            if (wfinger.empty()) wfinger = L"(無法取得 fingerprint)";
            if (wkey.empty()) wkey = L"(無法計算 license key)";
            if (copied) {
                message += L"10分鐘後將停用 MMF 輸入訊號\n\n";
                message = L"評估版 升級為 正式版 方法如下:\n\n";
                message += L"Device fingerprint (已複製到剪貼簿):\n" + wfinger + L"\n\n";
                message += L"貼到e-mail寄回[立洋科技]公司，或正版 license.txt 放到 BigCat.exe 同一目錄。\n\n";
                message += L"聯絡支援 / Contact: 電話 0989140456, wuyenhung@gmail.com";
            } else {
                message += L"10分鐘後將停用 MMF 輸入訊號\n\n";                
                message = L"評估版 升級為 正式版 方法如下:\n\n";
                message += L"Device fingerprint:\n" + wfinger + L"\n\n";
                message += L"(序號e-mail寄回[立洋科技]公司，或正版 license.txt 放到 BigCat.exe 同一目錄。)\n\n";
                message += L"聯絡支援 / Contact: 電話 0989140456, wuyenhung@gmail.com";
            }

            ShowTopmostMessageBox(message, L"評估版提醒", MB_OK | MB_ICONWARNING);
        }
    }
    
    // 記錄授權狀態供後續使用
    bool licenseValid = LicenseChecker::VerifyLicenseFromCmdLine(lpCmdLine ? std::string(lpCmdLine) : std::string());

    // 授權失敗時的到期策略：若授權驗證失敗，允許程式在到期日 (含) 之前繼續執行；
    // 若目前日期已超過到期日 (2026-07-28)，則立即顯示警告並退出。
    auto IsAfterExpiryDate = [](int y, int m, int d)->bool {
        SYSTEMTIME st; GetLocalTime(&st);
        if (st.wYear > y) return true;
        if (st.wYear < y) return false;
        if (st.wMonth > m) return true;
        if (st.wMonth < m) return false;
        if (st.wDay > d) return true;
        return false;
    };

    const int kExpiryYear = 2026;
    const int kExpiryMonth = 7; // July
    const int kExpiryDay = 28;

    if (!licenseValid) {
        // 若已過到期日，強制離開
        if (IsAfterExpiryDate(kExpiryYear, kExpiryMonth, kExpiryDay)) {
            // Do NOT display the exact expiry date to the user — only show a generic
            // 'license expired' message per product policy.
            ShowTopmostMessageBox(
                L"授權已過期。\n目前裝置未通過授權，請聯絡立洋科技取得正式授權或更新授權檔。",
                L"授權過期",
                MB_OK | MB_ICONERROR
            );
            // 清理並退出
            ReleaseMutex(hMutex);
            CloseHandle(hMutex);
            return 1;
        } else {
            // 尚未到期 — 紀錄剩餘天數以便日誌與提醒（如果需要）
            SYSTEMTIME st; GetLocalTime(&st);
            // compute approximate remaining days (not used for logic beyond logging)
            // simple human-friendly log
        }
    }

    // single-instance mutex already created earlier at startup

    // 設定 EtherCAT Master（使用 main 內的區域實例以避免不必要的全域）
    // CRITICAL: 必須在 window 之前宣告，確保解構順序正確：
    // 解構時會先解構 window (停止所有使用 master 的執行緒)
    // 然後才解構 etherCATMaster (刪除 critical sections)
    EtherCATMaster etherCATMaster;
    // 預設啟用 DC 漂移補償，以提升 PDO 週期穩定度
    etherCATMaster.EnableDcDriftCompensation(true);

    // =============================================================
    // Warm‑up network stack / SOEM library on first run.  Some NIC
    // drivers and the pcap backend load lazily when the first frame is
    // transmitted; this can cause jitter or an occasional lost PDO cycle
    // when the user connects.  Attempt a quick initialization on the
    // first enumerated adapter so that the real connection (later in the
    // UI flow) experiences a "warmed" driver.
    if (EtherCATMaster::IsPhysicalEthercatAllowed()) {
        std::string err;
        std::vector<CoreHelpers::AdapterEntry> adapters = CoreHelpers::EnumerateAdapters(err);
        if (!adapters.empty()) {
            const std::string &firstIf = adapters[0].pcapName;
            // ignore result; failure is non‑fatal
            EtherCATMaster::PreloadInterface(firstIf.c_str());
        }
    }

    // Decide startup mode early so tray startup can avoid showing the main window first.
    AppConfig::EnsureDefaultIniFile();
    bool httpEnabledOnStart = AppConfig::ReadBool(L"Server", L"EnableLocalHttpServer", false);
    bool showMainOnStart = AppConfig::ReadBool(L"Startup", L"ShowMainWindow", false);
    bool autoTrigger = AppConfig::ReadBool(L"Startup", L"AutoTrigger", true);

    // 建立視窗
    MainWindow window;
    if (!window.Create(hInstance, showMainOnStart)) {
        MessageBoxW(NULL, L"建立視窗失敗", L"錯誤", MB_OK | MB_ICONERROR);
        CloseHandle(hMutex);
        return 1;
    }

    // 每次程式啟動時先清空/截斷舊的 status.log，之後再開始記錄新的日誌
    // 使用現有的 OnClearStatus() 封裝，該函式會在有 child window 時更新 UI，
    // 在啟動階段 child window 尚未建立，傳入 NULL 的情況下僅會截斷檔案並初始化緩衝。
    window.OnClearStatus();

    // 初始化 AppController，綁定現有的核心元件
    AppController::GetInstance().Initialize(&etherCATMaster, &window);

    window.PrependStatus(L"[Startup] 程式啟動流程開始\r\n");
    window.PrependStatus(httpEnabledOnStart
        ? L"[Startup] 已啟用 Web 伺服器，程式啟動時允許 HTTP server 啟動\r\n"
        : L"[Startup] 未啟用 Web 伺服器，程式啟動時略過 HTTP server\r\n");
    window.PrependStatus(showMainOnStart
        ? L"[Startup] 啟動模式：顯示主視窗\r\n"
        : L"[Startup] 啟動模式：最小化到系統匣\r\n");
    window.PrependStatus(autoTrigger
        ? L"[Startup] 自動啟動序列：已啟用\r\n"
        : L"[Startup] 自動啟動序列：未啟用\r\n");

    if (showMainOnStart) {
        window.Show(nCmdShow);
        window.PrependStatus(L"[Startup] 主視窗顯示完成\r\n");
    } else {
        // Window is already shown by Create() for proper initialization
        // Now hide it to system tray.
        // Directly minimize to tray using the public method
        window.MinimizeToTray();
        window.PrependStatus(L"[Startup] 已最小化到系統匣\r\n");

        // 程式啟動時根據 INI 決定是否自動執行連接與自動程序。
        // Do not block startup waiting for the HTTP server: the browser is no longer
        // auto-opened here, and delaying AutoStartup by the HTTP readiness poll adds
        // several seconds of dead time before connection begins.
        if (autoTrigger) {
            window.PrependStatus(L"[Startup] 觸發自動啟動序列\r\n");
            window.TriggerAutoStartupSequence(httpEnabledOnStart);
        }
    }

    // -----------------------------
    // 授權計時器：如果授權失敗，啟動背景執行緒在10分鐘後再次警告並停用 MMF 輸入和所有 UI
    // -----------------------------
    std::thread* licenseTimerThread = nullptr;
    if (!licenseValid) {
        licenseTimerThread = new std::thread([&etherCATMaster, &window]() {
            // 等待 10 分鐘
            Sleep(600000);
            
            // 停用 MMF 輸入訊號
            etherCATMaster.StopMMF1In();
            
            // 停用內建 HTTP 伺服器
            
            // 不再禁用主畫面 UI — 使用者仍可查看/操作畫面 (但 MMF 與 HTTP 可能已停用)
            
            // 顯示第二次警告訊息
            std::wstring finalMessage = L"評估版本提醒_如有bug請回饋..非常感謝\n\n";
            finalMessage += L"MMF 輸入訊號已停用\n";
            finalMessage += L"如需購買正版 請洽詢 [立洋科技]\n\n";
            finalMessage += L"聯絡支援 / Contact: 電話 0989140456, wuyenhung@gmail.com";
            
            // 附加強制退出提示：3分鐘後強制終止程式，提醒使用者盡快完成操作以避免無預警停機導致安全事故
            finalMessage += L"\n重要：接下來程式會在 3 分鐘後關閉。\n請盡快完成作業並儲存/備份，避免無預警停機造成安全事故。\n";
            // 啟動一個短暫的倒數計時（3 分鐘）後嘗試溫和關閉；若等待期結束仍未關閉，則採用強制退出做為最後手段
            // NOTE: 啟動倒數計時在顯示提醒視窗之前，這樣即使使用者沒有關閉提醒視窗，計時仍會繼續進行
            const HWND hwndForExit = window.GetHwnd();
            std::thread([hwndForExit](){
                // 3 minutes in ms
                const DWORD exitDelayMs = 3 * 60 * 1000;
                const DWORD gracefulWaitMs = 15 * 1000; // 等待主視窗回應關閉的寬限時間 (15 秒)
                const DWORD pollIntervalMs = 500;

                Sleep(exitDelayMs);

                OutputDebugStringA("[BigCat] Graceful-exit timeout reached due to license failure - attempting graceful shutdown\n");

                HWND hwnd = hwndForExit;
                if (hwnd && IsWindow(hwnd)) {
                    PostMessage(hwnd, WM_CLOSE, 0, 0);

                    // 等待視窗關閉（輪詢）
                    DWORD waited = 0;
                    while (waited < gracefulWaitMs) {
                        if (!IsWindow(hwnd)) {
                            OutputDebugStringA("[BigCat] Main window closed gracefully after WM_CLOSE\n");
                            return; // 已被正常關閉
                        }
                        Sleep(pollIntervalMs);
                        waited += pollIntervalMs;
                    }

                    // 若到這裡視窗仍存在，改以強制方式結束
                    OutputDebugStringA("[BigCat] Graceful shutdown timed out - forcing process exit\n");
                    ExitProcess(1);
                } else {
                    // 找不到視窗，直接強制結束
                    OutputDebugStringA("[BigCat] No main window found for graceful shutdown; forcing process exit\n");
                    ExitProcess(1);
                }
            }).detach();

            // 現在顯示提醒視窗。由於倒數已經啟動，即使此視窗長時間未被關閉，倒數仍會在背景執行。
            ShowTopmostMessageBox(finalMessage, L"評估版提醒 — 即將強制退出", MB_OK | MB_ICONERROR);
        });
    }

    // -----------------------------
    // 測試用：可選擇性觸發未處理例外 (測試 minidump)
    // 覆寫方式：
    //  - 環境變數: BIGCAT_TEST_CRASH=1
    //  - 或使用命令列參數: -test-crash
    // 若未設定則不會崩潰。
    // -----------------------------
    {
        bool triggerCrash = false;
        // 檢查環境變數
        char envVal[16] = {0};
        DWORD len = GetEnvironmentVariableA("BIGCAT_TEST_CRASH", envVal, sizeof(envVal));
        if (len > 0 && (_stricmp(envVal, "1") == 0 || _stricmp(envVal, "true") == 0)) {
            triggerCrash = true;
        }

        // 檢查 INI 設定 (取代命令列啟動參數)
        if (!triggerCrash) {
            // Read from INI: [Startup] TriggerTestCrash = 1
            wchar_t bufIniPath[MAX_PATH] = {0};
            if (GetModuleFileNameW(NULL, bufIniPath, MAX_PATH) != 0) {
                // reuse same pattern as ReadIniBoolLocal above
                std::wstring iniPath = std::wstring(bufIniPath);
                wchar_t *dot = wcsrchr(&iniPath[0], L'.');
                wchar_t *p = wcsrchr(&iniPath[0], L'\\');
                if (dot && (!p || dot > p)) *dot = L'\0';
                iniPath += L".ini";
                int v = GetPrivateProfileIntW(L"Startup", L"TriggerTestCrash", 0, iniPath.c_str());
                if (v != 0) triggerCrash = true;
            }
        }

        if (triggerCrash) {
            // Test crash is intentionally destructive — restrict to debug builds.
            // This prevents accidental production crashes if an env var is set in
            // non-development environments while allowing devs to exercise the
            // crash handler during debugging.
#if defined(_DEBUG)
            // 稍微等待 UI 完成顯示，再故意觸發存取違規以測試 crash handler
            OutputDebugStringA("[BigCat] Test crash requested -> triggering access violation\n");
            // 使用未處理的存取違例 (write to null pointer)
            volatile int* p = nullptr;
            *p = 0xDEADBEEF; // 觸發 AV
            // 不會到達此處
#else
            OutputDebugStringA("[BigCat] Test crash requested but ignored in non-debug build\n");
#endif
        }
    }

    // 訊息迴圈
    int ret = window.MessageLoop();
    
    
    // 清理授權計時器執行緒（如果存在）
    if (licenseTimerThread) {
        // 注意：執行緒可能正在等待或已完成，直接等待其結束
        if (licenseTimerThread->joinable()) {
            licenseTimerThread->join();
        }
        delete licenseTimerThread;
        licenseTimerThread = nullptr;
    }
    
    // 清理 Mutex: release ownership then close handle
    ReleaseMutex(hMutex);
    CloseHandle(hMutex);
    return ret;
}
