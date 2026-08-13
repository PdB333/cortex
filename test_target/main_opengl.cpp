// OpenGL renderer-specific test target for exercising Cortex's OpenGL hook,
// screenshot capture, overlay path, and background input. The exported canary
// symbols mirror the other renderer fixtures so the same memory checks can be
// reused by validation scripts.

#include <windows.h>
#include <GL/gl.h>
#include <cstdint>

extern "C" __declspec(dllexport) uint32_t g_cortex_u32   = 0xDEADBEEFu;
extern "C" __declspec(dllexport) uint64_t g_cortex_u64   = 0x0123456789ABCDEFull;
extern "C" __declspec(dllexport) float    g_cortex_float = 3.14159265f;
extern "C" __declspec(dllexport) volatile uint32_t g_cortex_frame = 0;
extern "C" __declspec(dllexport) volatile uint32_t g_cortex_health = 100;
extern "C" __declspec(dllexport) char g_cortex_str[32] = "cortex-canary";

static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(window, message, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
    WNDCLASSA windowClass{};
    windowClass.lpfnWndProc = WndProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = "cortex_test_opengl";
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.style = CS_OWNDC;
    if (!RegisterClassA(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return 1;

    HWND window = CreateWindowA(windowClass.lpszClassName, "cortex_test OpenGL",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                CW_USEDEFAULT, CW_USEDEFAULT, 640, 480,
                                nullptr, nullptr, instance, nullptr);
    if (!window) return 2;

    HDC deviceContext = GetDC(window);
    if (!deviceContext) return 3;

    PIXELFORMATDESCRIPTOR format{};
    format.nSize = sizeof(format);
    format.nVersion = 1;
    format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    format.iPixelType = PFD_TYPE_RGBA;
    format.cColorBits = 32;
    format.cAlphaBits = 8;
    format.cDepthBits = 24;
    format.iLayerType = PFD_MAIN_PLANE;

    const int pixelFormat = ChoosePixelFormat(deviceContext, &format);
    if (!pixelFormat || !SetPixelFormat(deviceContext, pixelFormat, &format)) {
        ReleaseDC(window, deviceContext);
        return 4;
    }

    HGLRC context = wglCreateContext(deviceContext);
    if (!context || !wglMakeCurrent(deviceContext, context)) {
        if (context) wglDeleteContext(context);
        ReleaseDC(window, deviceContext);
        return 5;
    }

    MSG message{};
    bool running = true;
    while (running) {
        while (PeekMessageA(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                running = false;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }
        if (!running) break;

        glViewport(0, 0, 640, 480);
        glClearColor(0x33 / 255.0f, 0x66 / 255.0f, 0x99 / 255.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        SwapBuffers(deviceContext);
        ++g_cortex_frame;
        Sleep(16);
    }

    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(context);
    ReleaseDC(window, deviceContext);
    DestroyWindow(window);
    return 0;
}
