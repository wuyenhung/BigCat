#include "Win32Window.h"
#include "EtherCATMaster.h"
#include <windows.h>

// 將 EtherCATMaster 移到 main 範圍以移除不必要的全域定義

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, 
                   LPSTR lpCmdLine, int nCmdShow) {
    // Mark unused WinMain parameters as used to avoid C4100 warnings
    (void)hPrevInstance;
    (void)lpCmdLine;
    // 單例檢查：使用命名 Mutex 避免程式雙開
    // Use a global named mutex to prevent multiple instances across user sessions
    // and silently exit if another instance exists.
    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"Global\\BigCat_Singleton_Mutex");
    if (hMutex == NULL) {
        // If we cannot create the mutex, fail loudly (this is unexpected)
        MessageBoxW(NULL, L"無法建立 Mutex", L"錯誤", MB_OK | MB_ICONERROR);
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Another instance already exists on this machine (across sessions) - exit silently
        CloseHandle(hMutex);
        return 0;
    }

    // 建立視窗
    Win32Window window;
    if (!window.Create(hInstance)) {
        MessageBoxW(NULL, L"建立視窗失敗", L"錯誤", MB_OK | MB_ICONERROR);
        CloseHandle(hMutex);
        return 1;
    }

    // 設定 EtherCAT Master（使用 main 內的區域實例以避免不必要的全域）
    EtherCATMaster etherCATMaster;
    window.SetEtherCATMaster(&etherCATMaster);

    // 顯示視窗
    window.Show(nCmdShow);

    // 訊息迴圈
    int ret = window.MessageLoop();
    // 清理 Mutex: release ownership then close handle
    ReleaseMutex(hMutex);
    CloseHandle(hMutex);
    return ret;
}
