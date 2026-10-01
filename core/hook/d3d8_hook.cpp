#include "d3d8_hook.h"
#include "../overlay/overlay.h"
#include "../capture/capture.h"
#include "../log.h"

#include <windows.h>
#include <d3d8.h>
#include <MinHook.h>
#include <cstdio>
#include <atomic>

namespace hook {

namespace {
    using EndScene_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice8*);

    EndScene_t oEndScene = nullptr;
    bool g_frameInitialized = false;
    std::atomic<bool> g_hookInstalled{false};
    long g_endSceneCalls = 0;

    HRESULT STDMETHODCALLTYPE hkEndScene(IDirect3DDevice8* device) {
        if (g_endSceneCalls < 3 || (g_endSceneCalls % 300) == 0) {
            dbglog::Line("hkEndScene called (#%ld), device=%p", g_endSceneCalls, (void*)device);
        }
        ++g_endSceneCalls;
        if (!g_frameInitialized) {
            D3DDEVICE_CREATION_PARAMETERS params = {};
            if (SUCCEEDED(device->GetCreationParameters(&params)) && params.hFocusWindow) {
                overlay::Init(device, params.hFocusWindow);
                g_frameInitialized = true;
            }
        }

        if (g_frameInitialized) overlay::OnFrame(device);
        capture::OnEndScene(device);
        return oEndScene(device);
    }

    // Creates a throwaway device solely to read the shared EndScene vtable
    // address. Hooking that address also hooks the game's real D3D8 device.
    bool GetD3D8EndSceneAddress(void** outEndScene) {
        WNDCLASSEXA wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpszClassName = "CortexDummyWnd";
        RegisterClassExA(&wc);

        HWND dummyHwnd = CreateWindowExA(0, wc.lpszClassName, "", WS_OVERLAPPEDWINDOW,
                                          0, 0, 640, 480, nullptr, nullptr, wc.hInstance, nullptr);
        if (!dummyHwnd) return false;

        bool ok = false;
        IDirect3D8* d3d = Direct3DCreate8(D3D_SDK_VERSION);
        if (d3d) {
            D3DPRESENT_PARAMETERS pp = {};
            pp.Windowed = TRUE;
            pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
            pp.BackBufferFormat = D3DFMT_UNKNOWN;
            pp.BackBufferWidth = 640;
            pp.BackBufferHeight = 480;
            pp.hDeviceWindow = dummyHwnd;

            IDirect3DDevice8* device = nullptr;
            HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, dummyHwnd,
                                            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device);
            if (FAILED(hr)) {
                hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_REF, dummyHwnd,
                                        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device);
            }

            dbglog::Line("dummy CreateDevice hr=0x%08lX device=%p", (unsigned long)hr, (void*)device);
            if (SUCCEEDED(hr) && device) {
                void** vtable = *reinterpret_cast<void***>(device);
                *outEndScene = vtable[35];
                dbglog::Line("vtable=%p EndScene=%p", (void*)vtable, *outEndScene);
                device->Release();
                ok = true;
            }
            d3d->Release();
        }

        DestroyWindow(dummyHwnd);
        UnregisterClassA(wc.lpszClassName, wc.hInstance);
        return ok;
    }
}

bool InitD3D8Hook() {
    void* endSceneAddr = nullptr;
    if (!GetD3D8EndSceneAddress(&endSceneAddr)) {
        dbglog::Line("GetD3D8EndSceneAddress failed");
        return false;
    }

    const MH_STATUS create = MH_CreateHook(endSceneAddr, reinterpret_cast<void*>(&hkEndScene),
                                           reinterpret_cast<void**>(&oEndScene));
    dbglog::Line("MH_CreateHook(EndScene) = %d", static_cast<int>(create));
    if (create != MH_OK) return false;

    const MH_STATUS enable = MH_EnableHook(endSceneAddr);
    dbglog::Line("MH_EnableHook(EndScene) = %d", static_cast<int>(enable));
    if (enable != MH_OK) return false;

    g_hookInstalled = true;
    return true;
}

void ShutdownD3D8Hook() {
    MH_DisableHook(MH_ALL_HOOKS);
    overlay::Shutdown();
    g_frameInitialized = false;
    g_hookInstalled = false;
}

bool IsD3D8HookInstalled() { return g_hookInstalled.load(); }

} // namespace hook
