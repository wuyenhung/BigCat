// cpu_burn_test.cpp — CPU 核心佔用測試工具 (GUI)
// 用法: cpu_burn_test.exe
//
// 提供簡單視窗 UI，以多欄 checkbox 勾選個別 CPU 核心或全選，
// 設定持續時間後啟動 busy-loop 佔用選定核心。
//
// 編譯 (Developer Command Prompt):
//   cl /EHsc /O2 /utf-8 /Fe:cpu_burn_test.exe tools\cpu_burn_test.cpp user32.lib comctl32.lib

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <commctrl.h>
#include <intrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>

// ── 常數 ──
#define MAX_CPUS           64
#define IDC_CPU_BASE       2000
#define IDC_SELECT_ALL     (IDC_CPU_BASE + MAX_CPUS + 1)
#define IDC_DESELECT_ALL   (IDC_CPU_BASE + MAX_CPUS + 2)
#define IDC_DURATION       (IDC_CPU_BASE + MAX_CPUS + 3)
#define IDC_START_STOP     (IDC_CPU_BASE + MAX_CPUS + 4)
#define IDC_STATUS         (IDC_CPU_BASE + MAX_CPUS + 5)

// ── 佈局常量 ──
#define MARGIN          12
#define CHK_W           86   // checkbox 寬度
#define CHK_H           20   // checkbox 高度
#define CHK_COL_GAP     8    // 欄間距
#define CHK_ROW_GAP     2    // 列間距
#define BTN_W           90
#define BTN_H           24
#define DUR_LABEL_W     170
#define DUR_EDIT_W      66
#define DUR_EDIT_H      21

// ── 每個核心的燒機 thread 狀態 ──
struct BurnThread {
    HANDLE hThread;
    volatile LONG running;
    int coreIndex;
    DWORD_PTR mask;
    unsigned long long counter;
};

static std::vector<BurnThread> g_threads;
static HWND g_hWnd = NULL;
static HWND g_hCpuChecks[MAX_CPUS] = {};
static int g_numCpus = 0;
static int g_cols = 4;
static DWORD g_startTick = 0;
static int g_durationSec = 0;
static bool g_burning = false;
static volatile LONG g_stopAll = 0;
static DWORD_PTR g_sysAffinityMask = 0;  // 系統可用核心遮罩

// ── 取得系統邏輯處理器數量 ──
static int GetLogicalCpuCount() {
    DWORD count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (count == 0) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        count = si.dwNumberOfProcessors;
    }
    if (count < 1) count = 1;
    if (count > MAX_CPUS) count = MAX_CPUS;
    return (int)count;
}

// ── Worker thread（省電型 busy-loop） ──
// 每 4096 次迭代插入 YieldProcessor()，降低實際功耗但保持 OS 回報 100% 使用率
static DWORD WINAPI BurnThreadProc(LPVOID param) {
    BurnThread *bt = (BurnThread *)param;

    DWORD_PTR prevMask = SetThreadAffinityMask(GetCurrentThread(), bt->mask);
    if (prevMask == 0) {
        // affinity 設定失敗，仍繼續執行但記錄
        bt->running = -1;
        return 0;
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);

    bt->counter = 0;
    bt->running = 1;

    while (bt->running && !g_stopAll) {
        bt->counter++;
        // 省電型：週期性 pause 提示 CPU 進入低功耗 spin 狀態
        if ((bt->counter & 0xFFF) == 0)
            YieldProcessor();
    }

    bt->running = 0;
    return 0;
}

// ── 開始 burning ──
static void StartBurning(HWND hWnd) {
    if (g_burning) return;

    g_stopAll = 0;
    g_threads.clear();

    WCHAR durText[32] = {};
    GetDlgItemTextW(hWnd, IDC_DURATION, durText, 32);
    g_durationSec = _wtoi(durText);
    if (g_durationSec < 0) g_durationSec = 0;

    g_startTick = GetTickCount();

    // ── 先擴展 process affinity 以確保所有核心可被 thread 鎖定 ──
    DWORD_PTR procMask, sysMask;
    if (GetProcessAffinityMask(GetCurrentProcess(), &procMask, &sysMask)) {
        g_sysAffinityMask = sysMask;
        if (procMask != sysMask) {
            SetProcessAffinityMask(GetCurrentProcess(), sysMask);
        }
    } else {
        g_sysAffinityMask = ~(DWORD_PTR)0;
    }

    std::vector<int> selected;
    for (int i = 0; i < g_numCpus; i++) {
        if (g_hCpuChecks[i] &&
            SendMessageW(g_hCpuChecks[i], BM_GETCHECK, 0, 0) == BST_CHECKED) {
            selected.push_back(i);
        }
    }

    if (selected.empty()) {
        MessageBoxW(hWnd,
            L"請至少勾選一個 CPU 核心。",
            L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }

    for (int core : selected) {
        DWORD_PTR mask = (DWORD_PTR)1 << core;
        if (!(mask & g_sysAffinityMask)) {
            WCHAR buf[128];
            swprintf_s(buf, L"CPU %d 不在系統可用遮罩內 (0x%llX)，跳過。",
                       core, (unsigned long long)g_sysAffinityMask);
            MessageBoxW(hWnd, buf, L"警告", MB_OK | MB_ICONWARNING);
            continue;
        }
        BurnThread bt = {};
        bt.coreIndex = core;
        bt.mask = mask;
        g_threads.push_back(bt);
    }

    for (size_t i = 0; i < g_threads.size(); i++) {
        g_threads[i].hThread = CreateThread(NULL, 0, BurnThreadProc,
                                            &g_threads[i], 0, NULL);
    }

    g_burning = true;

    SetDlgItemTextW(hWnd, IDC_START_STOP, L"停止 (&S)");

    for (int i = 0; i < g_numCpus; i++) {
        if (g_hCpuChecks[i]) EnableWindow(g_hCpuChecks[i], FALSE);
    }
    EnableWindow(GetDlgItem(hWnd, IDC_DURATION), FALSE);
    EnableWindow(GetDlgItem(hWnd, IDC_SELECT_ALL), FALSE);
    EnableWindow(GetDlgItem(hWnd, IDC_DESELECT_ALL), FALSE);

    SetTimer(hWnd, 1, 500, NULL);
}

// ── 停止 burning ──
static void StopBurning(HWND hWnd) {
    if (!g_burning) return;

    KillTimer(hWnd, 1);
    InterlockedExchange(&g_stopAll, 1);

    for (auto &bt : g_threads) {
        bt.running = 0;
    }

    for (auto &bt : g_threads) {
        if (bt.hThread) {
            WaitForSingleObject(bt.hThread, 3000);
            CloseHandle(bt.hThread);
            bt.hThread = NULL;
        }
    }
    g_threads.clear();
    g_burning = false;

    SetDlgItemTextW(hWnd, IDC_START_STOP, L"開始 (&B)");
    SetDlgItemTextW(hWnd, IDC_STATUS, L"已停止。");

    for (int i = 0; i < g_numCpus; i++) {
        if (g_hCpuChecks[i]) EnableWindow(g_hCpuChecks[i], TRUE);
    }
    EnableWindow(GetDlgItem(hWnd, IDC_DURATION), TRUE);
    EnableWindow(GetDlgItem(hWnd, IDC_SELECT_ALL), TRUE);
    EnableWindow(GetDlgItem(hWnd, IDC_DESELECT_ALL), TRUE);
}

// ── 更新狀態文字 ──
static void UpdateStatus(HWND hWnd) {
    if (!g_burning) return;

    DWORD elapsed = (GetTickCount() - g_startTick) / 1000;

    WCHAR status[256];
    int len = swprintf_s(status, L"執行中... 已過 %u 秒", elapsed);

    if (g_durationSec > 0) {
        len += swprintf_s(status + len, 256 - len,
                          L" / %d 秒", g_durationSec);
        if ((int)elapsed >= g_durationSec) {
            StopBurning(hWnd);
            return;
        }
    }

    int runningCount = 0;
    for (auto &bt : g_threads) {
        if (bt.running) runningCount++;
    }
    swprintf_s(status + len, 256 - len,
               L"\r\n活躍核心: %d / %zu", runningCount, g_threads.size());

    SetDlgItemTextW(hWnd, IDC_STATUS, status);
}

// ── 計算佈局 ──
// columns: 根據 CPU 數量決定欄數
static int CalcColumns(int nCpus) {
    if (nCpus <= 8)  return 2;
    if (nCpus <= 16) return 4;
    if (nCpus <= 32) return 4;
    return 8;
}

// groupBox 內部寬度 (不含外框)
static int GroupInnerW(int cols) { return cols * CHK_W + (cols - 1) * CHK_COL_GAP; }

// groupBox 內部高度 (不含外框標題)
static int GroupInnerH(int nCpus, int cols) {
    int rows = (nCpus + cols - 1) / cols;
    return rows * (CHK_H + CHK_ROW_GAP) - CHK_ROW_GAP;
}

// ── Window procedure ──
static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_hWnd = hWnd;
        HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrW(hWnd, GWLP_HINSTANCE);

        g_numCpus = GetLogicalCpuCount();
        g_cols = CalcColumns(g_numCpus);
        int rows = (g_numCpus + g_cols - 1) / g_cols;

        int gbX = MARGIN;
        int gbY = MARGIN;
        int gbInnerW = GroupInnerW(g_cols);
        int gbInnerH = GroupInnerH(g_numCpus, g_cols);
        int gbW = gbInnerW + MARGIN * 2;
        int gbH = gbInnerH + 24; // +title room

        // ── GroupBox ──
        CreateWindowW(L"BUTTON", L"CPU 核心選擇",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            gbX, gbY, gbW, gbH,
            hWnd, (HMENU)(INT_PTR)0xFFFF, hInst, NULL);

        // ── Checkboxes in grid ──
        int chkX0 = gbX + MARGIN;
        int chkY0 = gbY + 16;
        for (int i = 0; i < g_numCpus; i++) {
            int col = i % g_cols;
            int row = i / g_cols;
            int cx = chkX0 + col * (CHK_W + CHK_COL_GAP);
            int cy = chkY0 + row * (CHK_H + CHK_ROW_GAP);

            WCHAR label[16];
            swprintf_s(label, L"CPU %d", i);
            g_hCpuChecks[i] = CreateWindowW(L"BUTTON", label,
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                cx, cy, CHK_W, CHK_H,
                hWnd, (HMENU)(INT_PTR)(IDC_CPU_BASE + i), hInst, NULL);
        }

        // ── 下方控制區 ──
        int ctrlY = gbY + gbH + MARGIN;

        // 全選 / 取消全選
        CreateWindowW(L"BUTTON", L"全選 (&A)",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            MARGIN, ctrlY, BTN_W, BTN_H,
            hWnd, (HMENU)(INT_PTR)IDC_SELECT_ALL, hInst, NULL);
        CreateWindowW(L"BUTTON", L"取消全選 (&D)",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            MARGIN + BTN_W + 6, ctrlY, BTN_W, BTN_H,
            hWnd, (HMENU)(INT_PTR)IDC_DESELECT_ALL, hInst, NULL);

        // 持續時間
        int durY = ctrlY + BTN_H + 10;
        CreateWindowW(L"STATIC", L"持續時間 (秒，0=手動停止):",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            MARGIN, durY + 3, DUR_LABEL_W, 16,
            hWnd, (HMENU)(INT_PTR)0, hInst, NULL);
        CreateWindowW(L"EDIT", L"0",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
            MARGIN + DUR_LABEL_W + 4, durY, DUR_EDIT_W, DUR_EDIT_H,
            hWnd, (HMENU)(INT_PTR)IDC_DURATION, hInst, NULL);

        // 開始/停止按鈕
        int btnY = durY + DUR_EDIT_H + 10;
        CreateWindowW(L"BUTTON", L"開始 (&B)",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            MARGIN, btnY, 120, 28,
            hWnd, (HMENU)(INT_PTR)IDC_START_STOP, hInst, NULL);

        // 狀態文字
        CreateWindowW(L"STATIC", L"就緒。請勾選要佔用的 CPU 核心後按開始。",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            MARGIN, btnY + 34, gbW + MARGIN, 36,
            hWnd, (HMENU)(INT_PTR)IDC_STATUS, hInst, NULL);
        return 0;
    }

    case WM_COMMAND: {
        WORD id = LOWORD(wParam);
        if (id == IDC_SELECT_ALL) {
            for (int i = 0; i < g_numCpus; i++) {
                if (g_hCpuChecks[i])
                    SendMessageW(g_hCpuChecks[i], BM_SETCHECK, BST_CHECKED, 0);
            }
            return 0;
        }
        if (id == IDC_DESELECT_ALL) {
            for (int i = 0; i < g_numCpus; i++) {
                if (g_hCpuChecks[i])
                    SendMessageW(g_hCpuChecks[i], BM_SETCHECK, BST_UNCHECKED, 0);
            }
            return 0;
        }
        if (id == IDC_START_STOP) {
            if (g_burning)
                StopBurning(hWnd);
            else
                StartBurning(hWnd);
            return 0;
        }
        break;
    }

    case WM_TIMER:
        if (wParam == 1) {
            UpdateStatus(hWnd);
            return 0;
        }
        break;

    case WM_DESTROY:
        StopBurning(hWnd);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ── Entry point ──
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                    LPWSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;

    INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icex);

    // 計算視窗大小
    int nCpus = GetLogicalCpuCount();
    int cols = CalcColumns(nCpus);
    int gbW = GroupInnerW(cols) + MARGIN * 2;
    int gbH = GroupInnerH(nCpus, cols) + 24;
    int clientW = gbW + MARGIN * 2;
    // 控制區: 按鈕列(24) + gap(10) + 持續列(21) + gap(10) + 開始按鈕(28) + gap(6) + 狀態(36) + bottom margin(12)
    int clientH = MARGIN + gbH + MARGIN + BTN_H + 10 + DUR_EDIT_H + 10 + 28 + 6 + 36 + MARGIN;

    // 註冊視窗類別
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"CpuBurnTestWnd";

    if (!RegisterClassW(&wc)) {
        MessageBoxW(NULL, L"無法註冊視窗類別。", L"錯誤", MB_OK | MB_ICONERROR);
        return 1;
    }

    // 調整視窗大小以容納 client area
    RECT rc = { 0, 0, clientW, clientH };
    AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);

    HWND hWnd = CreateWindowExW(
        0, L"CpuBurnTestWnd", L"CPU Burn Test",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        NULL, NULL, hInstance, NULL);

    if (!hWnd) {
        MessageBoxW(NULL, L"無法建立視窗。", L"錯誤", MB_OK | MB_ICONERROR);
        return 1;
    }

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return (int)msg.wParam;
}
