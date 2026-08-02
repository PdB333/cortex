// Renderer-specific test target for exercising Cortex's screenshot hook,
// overlay, and background input on D3D9 / D3D11. Same canary symbols as
// test_target/main.cpp so /memory tests are identical across binaries.
//
// Pick the backend via -DTEST_RENDERER=9 or =11 at compile time. Hardware is
// preferred, with deterministic software fallbacks for headless CI runners.

#include <windows.h>
#include <cstdint>
#include <cstdio>

#if TEST_RENDERER == 9
  #include <d3d9.h>
  #pragma comment(lib, "d3d9")
#elif TEST_RENDERER == 11
  #include <d3d11.h>
  #include <dxgi.h>
  #pragma comment(lib, "d3d11")
  #pragma comment(lib, "dxgi")
#else
  #error "TEST_RENDERER must be 9 or 11"
#endif

extern "C" __declspec(dllexport) uint32_t g_cortex_u32   = 0xDEADBEEFu;
extern "C" __declspec(dllexport) uint64_t g_cortex_u64   = 0x0123456789ABCDEFull;
extern "C" __declspec(dllexport) float    g_cortex_float = 3.14159265f;
extern "C" __declspec(dllexport) volatile uint32_t g_cortex_frame = 0;
extern "C" __declspec(dllexport) volatile uint32_t g_cortex_health = 100;
extern "C" __declspec(dllexport) char g_cortex_str[32] = "cortex-canary";

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}

static HWND MakeWindow(const char* title) {
    WNDCLASSA c{};
    c.lpfnWndProc = WndProc;
    c.hInstance = GetModuleHandleA(nullptr);
    c.lpszClassName = "cortex_test_d3d";
    c.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassA(&c);
    return CreateWindowA(c.lpszClassName, title, WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                         CW_USEDEFAULT, CW_USEDEFAULT, 640, 480,
                         nullptr, nullptr, c.hInstance, nullptr);
}

#if TEST_RENDERER == 9
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    HWND wnd = MakeWindow("cortex_test D3D9");
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) return 1;

    IDirect3DDevice9* dev = nullptr;
    D3DPRESENT_PARAMETERS p{};
    p.Windowed = TRUE;
    p.SwapEffect = D3DSWAPEFFECT_DISCARD;
    p.BackBufferFormat = D3DFMT_X8R8G8B8;
    p.hDeviceWindow = wnd;

    HRESULT result = d3d->CreateDevice(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &p, &dev);
    if (FAILED(result)) {
        result = d3d->CreateDevice(
            D3DADAPTER_DEFAULT, D3DDEVTYPE_REF, wnd,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &p, &dev);
    }
    if (FAILED(result) || !dev) {
        d3d->Release();
        return 2;
    }

    MSG msg{};
    while (true) {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return 0;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        dev->Clear(0, nullptr, D3DCLEAR_TARGET,
                   D3DCOLOR_XRGB(0x33, 0x66, 0x99), 1.0f, 0);
        dev->Present(nullptr, nullptr, nullptr, nullptr);
        ++g_cortex_frame;
        Sleep(16);
    }
}
#else
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    HWND wnd = MakeWindow("cortex_test D3D11");
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = wnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl{};
    HRESULT result = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION,
        &sd, &sc, &dev, &fl, &ctx);
    if (FAILED(result)) {
        result = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION,
            &sd, &sc, &dev, &fl, &ctx);
    }
    if (FAILED(result) || !sc || !dev || !ctx) return 2;

    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb))) || !bb)
        return 3;
    ID3D11RenderTargetView* rtv = nullptr;
    result = dev->CreateRenderTargetView(bb, nullptr, &rtv);
    bb->Release();
    if (FAILED(result) || !rtv) return 4;

    MSG msg{};
    while (true) {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return 0;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        const float clr[4] = {0x33 / 255.f, 0x66 / 255.f, 0x99 / 255.f, 1.f};
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->ClearRenderTargetView(rtv, clr);
        sc->Present(1, 0);
        ++g_cortex_frame;
    }
}
#endif
