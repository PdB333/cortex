// cortex_test_target: minimal x86/x64 target for testing Cortex end-to-end.
//
// Exposes well-known memory values at exported symbols so tests can verify
// /memory/read, scans, and freezes without needing a real game.
//
// Pass --crash-null to trigger an intentional unhandled access violation
// after startup. This is used to verify Cortex crash-report generation.

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" __declspec(dllexport) uint32_t g_cortex_u32   = 0xDEADBEEFu;
extern "C" __declspec(dllexport) uint64_t g_cortex_u64   = 0x0123456789ABCDEFull;
extern "C" __declspec(dllexport) float    g_cortex_float = 3.14159265f;
extern "C" __declspec(dllexport) double   g_cortex_double = 2.7182818284;
extern "C" __declspec(dllexport) char     g_cortex_str[32] = "cortex-canary";

extern "C" __declspec(dllexport) volatile uint32_t g_cortex_frame  = 0;
extern "C" __declspec(dllexport) volatile uint32_t g_cortex_wpress = 0;
extern "C" __declspec(dllexport) volatile uint32_t g_cortex_health = 100;

static void PrintCanary() {
    std::printf("cortex_test_target ready pid=%lu\n"
                "  &g_cortex_u32    = %p  = 0x%08X\n"
                "  &g_cortex_u64    = %p  = 0x%016llX\n"
                "  &g_cortex_float  = %p  = %f\n"
                "  &g_cortex_double = %p  = %f\n"
                "  &g_cortex_str    = %p  = \"%s\"\n"
                "  &g_cortex_frame  = %p\n"
                "  &g_cortex_health = %p (starts at 100, use freeze to test)\n",
                GetCurrentProcessId(),
                (void*)&g_cortex_u32,    g_cortex_u32,
                (void*)&g_cortex_u64,    (unsigned long long)g_cortex_u64,
                (void*)&g_cortex_float,  g_cortex_float,
                (void*)&g_cortex_double, g_cortex_double,
                (void*)&g_cortex_str,    g_cortex_str,
                (void*)&g_cortex_frame,
                (void*)&g_cortex_health);
    std::fflush(stdout);
}

__declspec(noinline) static void TriggerNullCrash() {
    std::puts("cortex_test_target: triggering intentional null write");
    std::fflush(stdout);
    volatile uint32_t* pointer = nullptr;
    *pointer = 0xC07ECAFEu;
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_DESTROY: PostQuitMessage(0); return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc; GetClientRect(h, &rc);
            HBRUSH bg = CreateSolidBrush(RGB(0x33, 0x66, 0x99));
            FillRect(dc, &rc, bg);
            DeleteObject(bg);
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "cortex_test_target  frame=%u  W=%u  health=%u",
                          g_cortex_frame, g_cortex_wpress, g_cortex_health);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(255, 255, 255));
            TextOutA(dc, 10, 10, buf, (int)std::strlen(buf));
            EndPaint(h, &ps);
            return 0;
        }
    }
    return DefWindowProcA(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int show) {
    PrintCanary();

    WNDCLASSA wc{};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "CortexTestTarget";
    RegisterClassA(&wc);

    HWND h = CreateWindowA("CortexTestTarget", "cortex_test_target",
                           WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                           640, 200, nullptr, nullptr, inst, nullptr);
    ShowWindow(h, show);

    const bool crashNull = std::strstr(GetCommandLineA(), "--crash-null") != nullptr;
    uint32_t crashCountdown = crashNull ? 60 : 0;

    for (;;) {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return 0;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        g_cortex_frame++;
        if (GetAsyncKeyState('W') & 0x8000) g_cortex_wpress++;
        InvalidateRect(h, nullptr, FALSE);
        if (crashCountdown > 0 && --crashCountdown == 0) TriggerNullCrash();
        Sleep(16);
    }
}
