// File Explorer (C++ / Win32). Usage:
//   FileExplorer.exe [folder]            start (in that folder)
//   FileExplorer.exe icon out.ico        write the app icon
#include "app.h"
#include <objbase.h>
#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>

typedef BOOL(WINAPI* SetDpiCtx)(HANDLE);
typedef HRESULT(WINAPI* SetAppId)(PCWSTR);

static void dpi_awareness() {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (auto f = (SetDpiCtx)(void*)GetProcAddress(u, "SetProcessDpiAwarenessContext"))
        if (f((HANDLE)-4)) return;  // per monitor v2
    SetProcessDPIAware();
}

static HWND wheel_target(const MSG& msg) {
    // the wheel scrolls what's under the mouse, not what has the focus (like Windows 10+)
    POINT pt{GET_X_LPARAM(msg.lParam), GET_Y_LPARAM(msg.lParam)};
    HWND h = WindowFromPoint(pt);
    if (!h) return nullptr;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId()) return nullptr;
    if (!IsWindowEnabled(GetAncestor(h, GA_ROOT))) return nullptr;
    return h;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc >= 3 && !_wcsicmp(argv[1], L"icon")) {
        std::string ico = make_ico_file();
        bool ok = write_file_atomic(argv[2], ico);
        LocalFree(argv);
        return ok ? 0 : 1;
    }
    wstr start;
    bool new_window = false;
    for (int i = 1; i < argc; i++) {
        if (!_wcsicmp(argv[i], L"--new-window")) { new_window = true; continue; }
        if (start.empty() && argv[i][0]) {
            wstr a = expand_env(argv[i]);
            wchar_t full[MAX_PATH * 4];
            DWORD n = GetFullPathNameW(a.c_str(), MAX_PATH * 4, full, nullptr);  // relative to where we were started
            start = n && n < MAX_PATH * 4 ? clean_path(full) : clean_path(a);
        }
    }
    LocalFree(argv);

    // already running? hand the folder to that window (a new tab) and quit
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\FileExplorer.Cpp.Instance");
    if (!new_window && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = nullptr;
        for (int tries = 0; tries < 20 && !other; tries++) {  // it may still be starting
            EnumWindows([](HWND h, LPARAM l) -> BOOL {
                if (GetPropW(h, INSTANCE_PROP)) { *(HWND*)l = h; return FALSE; }
                return TRUE;
            }, (LPARAM)&other);
            if (!other) Sleep(100);
        }
        if (other) {
            DWORD pid = 0;
            GetWindowThreadProcessId(other, &pid);
            AllowSetForegroundWindow(pid);
            COPYDATASTRUCT cd{COPYDATA_OPEN, (DWORD)((start.size() + 1) * sizeof(wchar_t)), (void*)start.c_str()};
            DWORD_PTR res = 0;
            if (SendMessageTimeoutW(other, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG, 3000, &res) && res) {
                if (mutex) CloseHandle(mutex);
                return 0;
            }
        }
    }
    if (!start.empty() && !is_dir(start) && exists(start)) start = dir_name(start);  // a file: open its folder

    dpi_awareness();
    HMODULE sh = GetModuleHandleW(L"shell32.dll");
    if (auto f = (SetAppId)(void*)GetProcAddress(sh, "SetCurrentProcessExplicitAppUserModelID"))
        f(L"FileExplorer.Cpp");
    OleInitialize(nullptr);
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES};
    InitCommonControlsEx(&icc);

    int code = 0;
    {
        App* app = new App();
        if (!app->start(start)) {
            MessageBoxW(nullptr, L"Couldn't open the window.", L"File Explorer", MB_ICONERROR);
            code = 1;
        } else {
            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                if (ui_dispatch(msg)) continue;
                if (msg.message == WM_MOUSEWHEEL || msg.message == WM_MOUSEHWHEEL) {
                    HWND t = wheel_target(msg);
                    if (t && t != msg.hwnd) msg.hwnd = t;
                }
                if (app->pre_key(msg)) continue;
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            code = (int)msg.wParam;
        }
        delete app;
    }
    OleUninitialize();
    if (mutex) CloseHandle(mutex);
    return code;
}
