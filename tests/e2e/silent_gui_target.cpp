#include <windows.h>

#include <cstdio>
#include <string>

namespace {
std::string g_statePath;

void WriteState() {
    HWND console = GetConsoleWindow();
    char className[128] = {};
    if (console) GetClassNameA(console, className, static_cast<int>(sizeof(className)));
    FILE* file = nullptr;
    fopen_s(&file, g_statePath.c_str(), "wb");
    if (!file) return;
    std::fprintf(file,
                 "{\"pid\":%lu,\"console_window\":%s,\"console_class\":\"%s\"}\n",
                 static_cast<unsigned long>(GetCurrentProcessId()),
                 console ? "true" : "false", className);
    std::fclose(file);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_TIMER) {
        WriteState();
        return 0;
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(window, message, wParam, lParam);
}
} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
    char statePath[MAX_PATH * 4] = {};
    const DWORD size = GetEnvironmentVariableA("CORTEX_SILENT_STATE", statePath,
                                                static_cast<DWORD>(sizeof(statePath)));
    if (!size || size >= sizeof(statePath)) return 2;
    g_statePath.assign(statePath, size);

    WNDCLASSA wc{};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = "CortexSilentGuiTarget";
    if (!RegisterClassA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 3;

    HWND window = CreateWindowExA(0, wc.lpszClassName, "Cortex Silent GUI Target",
                                  WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                  320, 180, nullptr, nullptr, instance, nullptr);
    if (!window) return 4;
    ShowWindow(window, SW_HIDE);
    SetTimer(window, 1, 100, nullptr);
    WriteState();

    MSG message{};
    while (GetMessageA(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }
    return 0;
}
