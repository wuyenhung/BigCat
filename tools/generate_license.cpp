#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <filesystem>
#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>
#include "LicenseChecker.h"

void print_usage() {
    std::cout << "generate_license - create license.txt (SHA-256) from device fingerprint\n";
    std::cout << "Usage:\n";
    std::cout << "  generate_license -f \"<fingerprint>\" [-o <outpath>]\n";
    std::cout << "  generate_license -i <inputfile> [-o <outpath>]\n";
    std::cout << "If no -o given, writes 'license.txt' into current directory.\n";
}

// Utility: convert UTF-16 to UTF-8
static std::string WideToUtf8(const std::wstring &w) {
    if (w.empty()) return std::string();
    int sz = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, NULL, 0, NULL, NULL);
    if (sz <= 0) return std::string();
    std::string out; out.resize(sz - 1);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &out[0], sz, NULL, NULL);
    return out;
}

// Utility: convert UTF-8 to UTF-16
static std::wstring Utf8ToWide(const std::string &s) {
    if (s.empty()) return std::wstring();
    int sz = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, NULL, 0);
    if (sz <= 0) return std::wstring();
    std::wstring out; out.resize(sz - 1);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &out[0], sz);
    return out;
}

// GUI implementation: small dialog window
struct GuiState {
    HWND hwnd;
    HWND hEditFp;
    HWND hEditKey;
    HWND hBtnLoad;
    HWND hBtnGen;
    HWND hBtnCopy;
    HWND hBtnSave;
    HFONT hf;
    std::wstring currentKey;
    GuiState() : hwnd(NULL), hEditFp(NULL), hEditKey(NULL), hBtnLoad(NULL), hBtnGen(NULL), hBtnCopy(NULL), hBtnSave(NULL), hf(NULL) {}
};

static LRESULT CALLBACK GL_WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    GuiState* s = (GuiState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE:
        if (lParam) {
            CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;
            if (cs->lpCreateParams) {
                s = (GuiState*)cs->lpCreateParams;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)s);
                s->hwnd = hwnd;
            }
        }
        return 0;
    case WM_COMMAND:
        if (!s) break;
        {
            int id = LOWORD(wParam);
            if (id == 2001) { // Load
                OPENFILENAMEW ofn = {0};
                wchar_t file[MAX_PATH] = {0};
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = hwnd;
                ofn.lpstrFile = file;
                ofn.nMaxFile = MAX_PATH;
                ofn.lpstrFilter = L"Text files\0*.txt\0All files\0*.*\0";
                ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                if (GetOpenFileNameW(&ofn)) {
                    std::filesystem::path p(file);
                    std::ifstream ifs(p);
                    if (!ifs) {
                        std::wstring msg = L"Failed to open file: ";
                        msg += file;
                        MessageBoxW(hwnd, msg.c_str(), L"Error", MB_OK | MB_ICONERROR);
                        break;
                    }
                    std::string line;
                    while (std::getline(ifs, line)) {
                        size_t s1 = line.find_first_not_of(" \t\r\n");
                        if (s1 == std::string::npos) continue;
                        size_t e = line.find_last_not_of(" \t\r\n");
                        std::string fp = line.substr(s1, e - s1 + 1);
                        std::wstring wfp = Utf8ToWide(fp);
                        SetWindowTextW(s->hEditFp, wfp.c_str());
                        break;
                    }
                }
            } else if (id == 2002) { // Generate
                // Read fingerprint text from edit control
                int len = GetWindowTextLengthW(s->hEditFp);
                std::vector<wchar_t> buf(len + 1);
                GetWindowTextW(s->hEditFp, buf.data(), len + 1);
                std::wstring wfp(buf.data() ? buf.data() : L"");

                // Trim
                auto trim_range = [&](const std::wstring &w)->std::pair<size_t,size_t>{
                    size_t st = w.find_first_not_of(L" \t\r\n");
                    if (st == std::wstring::npos) return {std::wstring::npos, 0};
                    size_t ed = w.find_last_not_of(L" \t\r\n");
                    return {st, ed};
                };

                // If multi-line text was pasted, use its first non-empty line as fingerprint.
                auto get_first_nonempty_line = [&](const std::wstring &full)->std::wstring {
                    size_t pos = 0;
                    while (pos < full.size()) {
                        // find end of current line
                        size_t nl = full.find_first_of(L"\r\n", pos);
                        std::wstring line;
                        if (nl == std::wstring::npos) {
                            line = full.substr(pos);
                            // will break after
                            pos = full.size();
                        } else {
                            line = full.substr(pos, nl - pos);
                            // advance past newline sequences
                            size_t adv = nl;
                            // skip both \r and \n
                            while (adv < full.size() && (full[adv] == L'\r' || full[adv] == L'\n')) adv++;
                            pos = adv;
                        }
                        // trim whitespace on this line
                        auto rr = trim_range(line);
                        if (rr.first != std::wstring::npos) {
                            return line.substr(rr.first, rr.second - rr.first + 1);
                        }
                    }
                    return std::wstring();
                };

                std::wstring firstLine = get_first_nonempty_line(wfp);
                auto r = trim_range(firstLine);
                if (r.first == std::wstring::npos) {
                    // empty: obtain the full local device fingerprint (TPM/NCrypt/TBS preferred)
                    std::string deviceFp = LicenseChecker::GetDeviceFingerprint();
                    if (deviceFp.empty()) {
                        MessageBoxW(hwnd, L"Fingerprint empty and failed to read local fingerprint", L"Error", MB_OK | MB_ICONERROR);
                        break;
                    }
                    std::wstring wdevice = Utf8ToWide(deviceFp);
                    SetWindowTextW(s->hEditFp, wdevice.c_str());
                    wfp = wdevice;
                    r = trim_range(wfp);
                    if (r.first == std::wstring::npos) {
                        MessageBoxW(hwnd, L"Failed to obtain fingerprint", L"Error", MB_OK | MB_ICONERROR);
                        break;
                    }
                }

                // now produce license
                std::wstring trimmed = wfp.substr(r.first, r.second - r.first + 1);
                std::string utf8fp = WideToUtf8(trimmed);
                std::string license = LicenseChecker::ComputeSHA256Hex(utf8fp);
                s->currentKey = Utf8ToWide(license);
                SetWindowTextW(s->hEditKey, s->currentKey.c_str());
                // copy to clipboard
                if (OpenClipboard(hwnd)) {
                    EmptyClipboard();
                    size_t cb = (s->currentKey.size()+1)*sizeof(wchar_t);
                    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, cb);
                    if (hg) {
                        void* p = GlobalLock(hg);
                        if (p) {
                            memcpy(p, s->currentKey.c_str(), cb);
                            GlobalUnlock(hg);
                            SetClipboardData(CF_UNICODETEXT, hg);
                        } else {
                            GlobalFree(hg);
                        }
                    }
                    CloseClipboard();
                }
                } else if (id == 2003) { // Copy
                if (!s->currentKey.empty()) {
                    if (OpenClipboard(hwnd)) {
                        EmptyClipboard();
                        size_t cb = (s->currentKey.size()+1)*sizeof(wchar_t);
                        HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, cb);
                        if (hg) {
                            void* p = GlobalLock(hg);
                            if (p) {
                                memcpy(p, s->currentKey.c_str(), cb);
                                GlobalUnlock(hg);
                                SetClipboardData(CF_UNICODETEXT, hg);
                            } else {
                                GlobalFree(hg);
                            }
                        }
                        CloseClipboard();
                    }
                }
            } else if (id == 2004) { // Save
                if (s->currentKey.empty()) {
                    MessageBoxW(hwnd, L"No licence generated", L"Info", MB_OK | MB_ICONINFORMATION);
                } else {
                    OPENFILENAMEW ofn = {0};
                    wchar_t file[MAX_PATH] = {0};
                    // default filename
                    wcscpy_s(file, MAX_PATH, L"license.txt");
                    ofn.lStructSize = sizeof(ofn);
                    ofn.hwndOwner = hwnd;
                    ofn.lpstrFile = file;
                    ofn.nMaxFile = MAX_PATH;
                    ofn.lpstrFilter = L"Text files\0*.txt\0All files\0*.*\0";
                    ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
                    if (GetSaveFileNameW(&ofn)) {
                        std::filesystem::path p(file);
                        std::ofstream ofs(p);
                        if (!ofs) {
                            std::wstring msg = L"Failed to create or write file: ";
                            msg += file;
                            MessageBoxW(hwnd, msg.c_str(), L"Error", MB_OK | MB_ICONERROR);
                        } else {
                            std::string utf8 = WideToUtf8(s->currentKey);
                            ofs << utf8 << "\n";
                        }
                    }
                }
            }
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
    int RunGui(HINSTANCE hInst) {

    const wchar_t *clsName = L"GenerateLicenseWndClass";
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = GL_WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = clsName;
    RegisterClassW(&wc);

    int width = 640, height = 300;
    GuiState *state = new GuiState();

    HWND hwnd = CreateWindowExW(0, clsName, L"Generate License (GUI)", WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, width, height, NULL, NULL, hInst, NULL);
    if (!hwnd) {
        MessageBoxW(NULL, L"Failed to create window", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    // controls: fingerprint edit, load button, generate button, result edit, copy and save
    HFONT hf = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    const int pad = 10;
    CreateWindowW(L"STATIC", L"Device fingerprint:", WS_CHILD | WS_VISIBLE, pad, pad, width - 2*pad, 18, hwnd, NULL, hInst, NULL);
    HWND hEditFp = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", NULL, WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                                    pad, pad+20, width - 2*pad - 120, 80, hwnd, (HMENU)1001, hInst, NULL);
    SendMessageW(hEditFp, WM_SETFONT, (WPARAM)hf, TRUE);

    HWND hBtnLoad = CreateWindowW(L"BUTTON", L"Load...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  width - pad - 100, pad+30, 100, 28, hwnd, (HMENU)2001, hInst, NULL);

    HWND hBtnGen = CreateWindowW(L"BUTTON", L"Generate", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                 width - pad - 100, pad+70, 100, 28, hwnd, (HMENU)2002, hInst, NULL);

    CreateWindowW(L"STATIC", L"License key (SHA-256):", WS_CHILD | WS_VISIBLE, pad, pad+110, width - 2*pad, 18, hwnd, NULL, hInst, NULL);
    HWND hEditKey = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", NULL, WS_CHILD | WS_VISIBLE | ES_READONLY | ES_AUTOHSCROLL,
                                    pad, pad+130, width - 2*pad, 28, hwnd, (HMENU)1002, hInst, NULL);
    SendMessageW(hEditKey, WM_SETFONT, (WPARAM)hf, TRUE);

    HWND hBtnCopy = CreateWindowW(L"BUTTON", L"Copy", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  pad, pad+170, 80, 28, hwnd, (HMENU)2003, hInst, NULL);
    HWND hBtnSave = CreateWindowW(L"BUTTON", L"Save file...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  pad + 90, pad+170, 120, 28, hwnd, (HMENU)2004, hInst, NULL);

    // associate controls with state and attach to window
    state->hEditFp = hEditFp;
    state->hEditKey = hEditKey;
    state->hBtnCopy = hBtnCopy;
    state->hBtnGen = hBtnGen;
    state->hBtnLoad = hBtnLoad;
    state->hBtnSave = hBtnSave;
    state->hf = hf;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)state);

    ShowWindow(hwnd, SW_SHOW);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    GuiState *gs = (GuiState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (gs) delete gs;
    return 0;
}

// When built as a WIN32 subsystem, link expects WinMain; provide a WinMain wrapper
// forward declare main so WinMain can call it
int main(int argc, char** argv);

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd) {
    int argc = 0;
    LPWSTR *argvw = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> argv_strs;
    argv_strs.reserve(argc);
    std::vector<char*> argv_ptrs;
    argv_ptrs.reserve(argc+1);
    for (int i = 0; i < argc; ++i) {
        argv_strs.push_back(WideToUtf8(argvw[i]));
    }
    for (auto &s : argv_strs) argv_ptrs.push_back(const_cast<char*>(s.c_str()));
    argv_ptrs.push_back(nullptr);
    LocalFree(argvw);
    return main(argc, argv_ptrs.data());
}

int main(int argc, char** argv) {
    if (argc < 2) {
        // launch GUI when no args provided
        return RunGui(GetModuleHandle(NULL));
    }

    std::string fingerprint;
    std::string inFile;
    std::string outPath = "license.txt"; // default

    bool useLocal = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-l" || a == "--local") { useLocal = true; continue; }
        if (a == "-f" && i+1 < argc) { fingerprint = argv[++i]; }
        else if ((a == "-i" || a == "--input") && i+1 < argc) { inFile = argv[++i]; }
        else if ((a == "-o" || a == "--output") && i+1 < argc) { outPath = argv[++i]; }
        else if (a == "-h" || a == "--help") { print_usage(); return 0; }
        else { std::cerr << "Unknown arg: " << a << "\n"; print_usage(); return 1; }
    }

    if (inFile.empty() && fingerprint.empty()) {
        std::cout << "No fingerprint provided. Enter fingerprint (or empty line to cancel):\n";
        std::getline(std::cin, fingerprint);
    }

    if (!inFile.empty()) {
        // read file and take first non-empty line
        std::filesystem::path pin(Utf8ToWide(inFile));
        std::ifstream ifs(pin);
        if (!ifs) { std::cerr << "Failed to open input file: " << inFile << "\n"; return 2; }
        std::string line;
        while (std::getline(ifs, line)) {
            // trim
            size_t s = line.find_first_not_of(" \t\r\n");
            if (s == std::string::npos) continue;
            size_t e = line.find_last_not_of(" \t\r\n");
            fingerprint = line.substr(s, e - s + 1);
            break;
        }
    }

    if (useLocal) {
        // compute the full device fingerprint (may include TPM/NCrypt path)
        fingerprint = LicenseChecker::GetDeviceFingerprint();
    }

    if (fingerprint.empty()) {
        std::cerr << "Empty fingerprint. Aborting.\n";
        return 3;
    }

    std::string license = LicenseChecker::ComputeSHA256Hex(fingerprint);
    if (license.empty()) {
        std::cerr << "Failed to compute SHA256 for fingerprint.\n";
        return 4;
    }

    // determine output: if a directory, append license.txt
    // prefer wide path construction so UTF-8 paths from argv are handled correctly on Windows
    std::filesystem::path outp(Utf8ToWide(outPath));
    if (std::filesystem::is_directory(outp)) {
        outp /= "license.txt";
    }

    std::ofstream ofs(outp);
    if (!ofs) {
        std::cerr << "Failed to open output file for writing: " << outp.string() << "\n";
        return 5;
    }
    ofs << license << "\n";
    ofs.close();

    std::cout << "Wrote license key to " << outp.string() << "\n";
    std::cout << "License: " << license << "\n";
    return 0;
}
