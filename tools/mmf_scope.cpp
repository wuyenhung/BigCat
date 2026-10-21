// mmf_scope.cpp
// MMF 波形示波器：讀取 Local\OUTPUT_MMF_6DOF 並即時顯示位置/速度波形
// 配合 mmf_sine_writer.exe 使用
// 用法: mmf_scope.exe
//
// 1ms 讀取週期，60Hz 畫面更新，雙通道（位置/速度）

#include <windows.h>
#include <mmsystem.h>
#include <avrt.h>
#include <intrin.h>
#include <cstdio>
#include <cmath>
#include <algorithm>

// ── 常數 ──────────────────────────────────────────
static constexpr const wchar_t* kMmfName = L"Local\\OUTPUT_MMF_6DOF";
static constexpr const wchar_t* kWindowClass = L"MmfScopeWindow";
static constexpr const wchar_t* kWindowTitle = L"MMF Scope - Heave (Pos/Vel)";
static constexpr int kChannelCount = 6;
static constexpr int kBufferSize = 5000;        // 5 秒 @ 1ms
static constexpr double kCycleMs = 1.0;          // 讀取週期
static constexpr int kRenderHz = 60;             // 畫面更新率
static constexpr int kRenderPeriodMs = 1000 / kRenderHz;  // ~16ms

static constexpr COLORREF kBgColor = RGB(12, 12, 18);
static constexpr COLORREF kGridColor = RGB(40, 40, 55);
static constexpr COLORREF kPosColor = RGB(0, 220, 120);   // 綠色：位置
static constexpr COLORREF kVelColor = RGB(80, 180, 255);  // 藍色：速度
static constexpr COLORREF kZeroColor = RGB(80, 80, 100);
static constexpr COLORREF kTextColor = RGB(180, 180, 200);

// ── 環形緩衝區 ────────────────────────────────────
struct Sample {
    float pos;  // Heave 位置 (mm)
    float vel;  // Heave 速度 (mm/s)
};

static Sample g_buffer[kBufferSize] = {};
static volatile LONG g_head = 0;          // 下一個寫入位置
static volatile LONG g_writeCount = 0;    // 總寫入次數（判斷緩衝區是否已填滿一輪）
static CRITICAL_SECTION g_bufferCs;
static volatile bool g_running = true;
static volatile bool g_mmfReady = false;  // MMF 已成功開啟

// ── 取樣快照（供繪製端讀取）───────────────────────
// 一律回傳 kBufferSize 筆：繞回後資料連續，初始未填滿區域為 0（對齊 fade-in）
static int buffer_snapshot(Sample* out, int outMax) {
    EnterCriticalSection(&g_bufferCs);
    LONG head = g_head;
    int count = outMax;
    // 讀取最近 count 筆（從 head-1 往回），照時間順序 (舊→新) 排列
    for (int i = 0; i < count; ++i) {
        LONG idx = head - 1 - i;
        while (idx < 0) idx += kBufferSize;
        out[count - 1 - i] = g_buffer[idx];
    }
    LeaveCriticalSection(&g_bufferCs);
    return count;
}

// ── 讀取執行緒 ────────────────────────────────────
DWORD WINAPI ReaderThread(LPVOID) {
    // 即時優先級
    if (!SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS)) {
        SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    // 等待 MMF 出現（最多等 30 秒）
    HANDLE hMap = nullptr;
    for (int retry = 0; retry < 3000 && g_running; ++retry) {
        hMap = OpenFileMappingW(FILE_MAP_READ, FALSE, kMmfName);
        if (hMap) break;
        Sleep(10);
    }
    if (!hMap) {
        g_running = false;
        return 1;
    }

    const DWORD mmfSize = static_cast<DWORD>(sizeof(float) * kChannelCount);
    float* view = static_cast<float*>(MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, mmfSize));
    if (!view) {
        CloseHandle(hMap);
        g_running = false;
        return 1;
    }
    g_mmfReady = true;

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    const LONGLONG cycleTicks = static_cast<LONGLONG>(kCycleMs / 1000.0 * freq.QuadPart);

    // 初始位置設定（避免第一筆速度爆衝）
    float prevPos = view[2];
    {
        EnterCriticalSection(&g_bufferCs);
        g_buffer[0].pos = prevPos;
        g_buffer[0].vel = 0.0f;
        g_head = 1;
        LeaveCriticalSection(&g_bufferCs);
    }

    LARGE_INTEGER nextWake;
    QueryPerformanceCounter(&nextWake);

    while (g_running) {
        float pos = view[2];  // Heave
        // 速度 = (pos2 - pos1) / dt, dt = 1ms = 0.001s
        float vel = (pos - prevPos) * 1000.0f;
        prevPos = pos;

        EnterCriticalSection(&g_bufferCs);
        int idx = g_head;
        g_buffer[idx].pos = pos;
        g_buffer[idx].vel = vel;
        idx = (idx + 1) % kBufferSize;
        g_head = idx;
        InterlockedIncrement(&g_writeCount);
        LeaveCriticalSection(&g_bufferCs);

        // 精確 1ms spin-wait
        nextWake.QuadPart += cycleTicks;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (nextWake.QuadPart > now.QuadPart) {
            const LONGLONG kSpinPause = cycleTicks / 20;
            const LONGLONG kSleep0Threshold = cycleTicks / 2;
            while (true) {
                QueryPerformanceCounter(&now);
                if (now.QuadPart >= nextWake.QuadPart) break;
                LONGLONG remaining = nextWake.QuadPart - now.QuadPart;
                if (remaining > kSleep0Threshold) {
                    Sleep(0);
                } else if (remaining > kSpinPause) {
                    _mm_pause();
                }
            }
        } else {
            nextWake = now;
        }
    }

    UnmapViewOfFile(view);
    CloseHandle(hMap);
    return 0;
}

// ── 繪製輔助 ──────────────────────────────────────
struct DrawContext {
    HWND hwnd;
    HDC memDc;
    HBITMAP memBmp;
    RECT rc;
    int w, h;
    int topH;       // 上半部（位置）高度
    int botH;       // 下半部（速度）高度

    void begin(HDC dc) {
        w = rc.right - rc.left;
        h = rc.bottom - rc.top;
        topH = h * 50 / 100;
        botH = h - topH - 1;
        memDc = CreateCompatibleDC(dc);
        memBmp = CreateCompatibleBitmap(dc, w, h);
        SelectObject(memDc, memBmp);

        // 背景
        HBRUSH bg = CreateSolidBrush(kBgColor);
        FillRect(memDc, &rc, bg);
        DeleteObject(bg);
    }

    void end(HDC dc) {
        BitBlt(dc, 0, 0, w, h, memDc, 0, 0, SRCCOPY);
        DeleteDC(memDc);
        DeleteObject(memBmp);
    }

    void grid(int y0, int y1, float yMin, float yMax, int tickStep) {
        // 水平格線
        HPEN gridPen = CreatePen(PS_SOLID, 1, kGridColor);
        HPEN oldPen = (HPEN)SelectObject(memDc, gridPen);
        int rangeH = y1 - y0;
        float range = yMax - yMin;
        if (range <= 0.0f) range = 10.0f;

        for (float v = yMin; v <= yMax + 0.001f; v += tickStep) {
            int y = y1 - (int)((v - yMin) / range * rangeH);
            MoveToEx(memDc, rc.left, y, nullptr);
            LineTo(memDc, rc.right, y);
        }
        // 垂直格線 (每秒一條)
        HPEN vPen = CreatePen(PS_DOT, 1, kGridColor);
        SelectObject(memDc, vPen);
        for (int s = 0; s < 10; ++s) {
            int x = rc.right - (int)((float)(s) / 10.0f * w);
            MoveToEx(memDc, x, y0, nullptr);
            LineTo(memDc, x, y1);
        }
        DeleteObject(vPen);
        SelectObject(memDc, oldPen);
        DeleteObject(gridPen);
    }

    template<typename F>
    void trace(Sample* data, int count, COLORREF color, int y0, int y1,
               float yMin, float yMax, F getter) {
        if (count < 2) return;
        HPEN pen = CreatePen(PS_SOLID, 2, color);
        HPEN oldPen = (HPEN)SelectObject(memDc, pen);
        float range = yMax - yMin;
        if (range <= 0.0f) range = 10.0f;
        int rangeH = y1 - y0;

        // 從最舊畫到最新（左到右）
        int maxPoints = w;
        int step = (count > maxPoints) ? count / maxPoints : 1;
        int prevX = -1, prevY = -1;

        for (int i = 0; i < count; i += step) {
            float v = getter(data[i]);
            float ratio = (v - yMin) / range;
            if (ratio < 0.0f) ratio = 0.0f;
            if (ratio > 1.0f) ratio = 1.0f;
            int x = rc.left + (int)((float)i / count * w);
            int y = y1 - (int)(ratio * rangeH);

            if (prevX >= 0) {
                MoveToEx(memDc, prevX, prevY, nullptr);
                LineTo(memDc, x, y);
            }
            prevX = x;
            prevY = y;
        }

        SelectObject(memDc, oldPen);
        DeleteObject(pen);
    }

    void label(int y0, int y1, float yMin, float yMax, int tickStep) {
        SetBkMode(memDc, TRANSPARENT);
        SetTextColor(memDc, kTextColor);
        HFONT font = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                  CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                                  DEFAULT_PITCH, L"Consolas");
        HFONT oldFont = (HFONT)SelectObject(memDc, font);

        int rangeH = y1 - y0;
        float range = yMax - yMin;
        if (range <= 0.0f) range = 10.0f;
        for (float v = yMin; v <= yMax + 0.001f; v += tickStep) {
            int y = y1 - (int)((v - yMin) / range * rangeH);
            wchar_t buf[32];
            swprintf_s(buf, L"%.0f", v);
            TextOutW(memDc, rc.left + 4, y - 8, buf, (int)wcslen(buf));
        }

        SelectObject(memDc, oldFont);
        DeleteObject(font);
    }
};

// ── 繪製 ──────────────────────────────────────────
static void render(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);

    DrawContext ctx;
    ctx.rc = rc;
    ctx.begin(dc);

    // 讀取緩衝區快照
    Sample snap[kBufferSize];
    int count = buffer_snapshot(snap, kBufferSize);

    // 計算 Y 軸範圍
    float posMin = 1e9f, posMax = -1e9f, velMin = 1e9f, velMax = -1e9f;
    for (int i = 0; i < count; ++i) {
        if (snap[i].pos < posMin) posMin = snap[i].pos;
        if (snap[i].pos > posMax) posMax = snap[i].pos;
        if (snap[i].vel < velMin) velMin = snap[i].vel;
        if (snap[i].vel > velMax) velMax = snap[i].vel;
    }
    if (count == 0) { posMin = -10; posMax = 10; velMin = -100; velMax = 100; }
    // 加 10% padding
    float posRng = posMax - posMin;
    if (posRng < 1.0f) posRng = 10.0f;
    posMin -= posRng * 0.1f; posMax += posRng * 0.1f;
    float velRng = velMax - velMin;
    if (velRng < 1.0f) velRng = 100.0f;
    velMin -= velRng * 0.1f; velMax += velRng * 0.1f;

    int splitY = ctx.topH;

    // 上半部：位置
    int posTick = (int)(posRng / 5.0f);
    if (posTick < 1) posTick = 1;
    ctx.grid(rc.top, splitY, posMin, posMax, posTick);
    ctx.trace(snap, count, kPosColor, rc.top, splitY, posMin, posMax,
              [](const Sample& s) { return s.pos; });
    ctx.label(rc.top, splitY, posMin, posMax, posTick);

    // 分隔線
    {
        HPEN sepPen = CreatePen(PS_SOLID, 1, kZeroColor);
        HPEN oldPen = (HPEN)SelectObject(ctx.memDc, sepPen);
        MoveToEx(ctx.memDc, rc.left, splitY, nullptr);
        LineTo(ctx.memDc, rc.right, splitY);
        SelectObject(ctx.memDc, oldPen);
        DeleteObject(sepPen);
    }

    // 下半部：速度
    int velY0 = splitY + 1;
    int velY1 = rc.bottom;
    int velTick = (int)(velRng / 5.0f);
    if (velTick < 1) velTick = 1;
    ctx.grid(velY0, velY1, velMin, velMax, velTick);
    ctx.trace(snap, count, kVelColor, velY0, velY1, velMin, velMax,
              [](const Sample& s) { return s.vel; });
    ctx.label(velY0, velY1, velMin, velMax, velTick);

    // 標題
    {
        SetBkMode(ctx.memDc, TRANSPARENT);
        SetTextColor(ctx.memDc, kTextColor);
        HFONT titleFont = CreateFontW(20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                       CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                                       DEFAULT_PITCH, L"Consolas");
        HFONT oldFont = (HFONT)SelectObject(ctx.memDc, titleFont);
        wchar_t title[128];
        LONG total = g_writeCount;
        int visible = (total < kBufferSize) ? (int)total : kBufferSize;
        if (count > 0 && total > 0) {
            swprintf_s(title, L"Pos: %+.1f mm  |  Vel: %+.1f mm/s  |  %d samples (%ds window)",
                       snap[count-1].pos, snap[count-1].vel, visible, kBufferSize / 1000);
        } else {
            swprintf_s(title, L"Waiting for MMF: %ls...", kMmfName);
        }
        SetTextColor(ctx.memDc, g_mmfReady ? kPosColor : RGB(255, 120, 80));
        TextOutW(ctx.memDc, rc.left + 8, rc.top + 2, title, (int)wcslen(title));
        SelectObject(ctx.memDc, oldFont);
        DeleteObject(titleFont);
    }

    ctx.end(dc);
    EndPaint(hwnd, &ps);
}

// ── 主視窗程序（搭配 timer 重繪）─────────────────
static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        SetTimer(hwnd, 1, kRenderPeriodMs, nullptr);
        return 0;
    }
    case WM_TIMER:
        if (wp == 1) InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1; // 雙緩衝自行處理
    case WM_PAINT:
        render(hwnd);
        return 0;
    case WM_DESTROY:
        g_running = false;
        KillTimer(hwnd, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ── 進入點 ────────────────────────────────────────
int main() {
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    int nShow = SW_SHOWDEFAULT;
    InitializeCriticalSection(&g_bufferCs);

    // 註冊視窗類別
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(kBgColor);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    // 建立視窗
    int w = 900, h = 480;
    HWND hwnd = CreateWindowExW(0, kWindowClass, kWindowTitle,
                                 WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME,
                                 CW_USEDEFAULT, CW_USEDEFAULT, w, h,
                                 nullptr, nullptr, hInst, nullptr);

    // 啟動讀取執行緒
    HANDLE hReader = CreateThread(nullptr, 0, ReaderThread, nullptr, 0, nullptr);
    if (!hReader) {
        MessageBoxW(nullptr, L"Failed to create reader thread", L"Error", MB_ICONERROR);
        return 1;
    }

    ShowWindow(hwnd, nShow);

    // 訊息迴圈
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // 清理
    g_running = false;
    WaitForSingleObject(hReader, 2000);
    CloseHandle(hReader);
    DeleteCriticalSection(&g_bufferCs);

    return (int)msg.wParam;
}
