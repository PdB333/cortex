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
    typedef HRESULT(STDMETHODCALLTYPE* EndScene_t)(IDirect3DDevice8*);
    typedef HRESULT(STDMETHODCALLTYPE* Reset_t)(IDirect3DDevice8*, D3DPRESENT_PARAMETERS*);

    EndScene_t oEndScene = nullptr;
    Reset_t oReset = nullptr;
    bool g_overlayInitialized = false;
    std::atomic<bool> g_hookInstalled{false};
    long g_endSceneCalls = 0;

    HRESULT STDMETHODCALLTYPE hkEndScene(IDirect3DDevice8* device) {
        if (g_endSceneCalls < 3 || (g_endSceneCalls % 300) == 0) {
            dbglog::Line("hkEndScene called (#%ld), device=%p", g_endSceneCalls, (void*)device);
        }
        ++g_endSceneCalls;
        if (!g_overlayInitialized) {
            D3DDEVICE_CREATION_PARAMETERS params = {};
            if (SUCCEEDED(device->GetCreationParameters(&params)) && params.hFocusWindow) {
                overlay::Init(device, params.hFocusWindow);
                g_overlayInitialized = true;
            }
        }

        if (g_overlayInitialized) {
            overlay::OnFrame(device);
        }
        capture::OnEndScene(device);

        return oEndScene(device);
    }

    HRESULT STDMETHODCALLTYPE hkReset(IDirect3DDevice8* device, D3DPRESENT_PARAMETERS* pp) {
        if (g_overlayInitialized) overlay::PreResetD3D8();
        HRESULT hr = oReset(device, pp);
        if (SUCCEEDED(hr) && g_overlayInitialized) overlay::PostReset(device);
        return hr;
    }

    // Creates a throwaway device solely to read vtable function addresses.
    // The vtable is shared by every device instance from the same driver, so
    // hooking these addresses hooks the game's real device too.
    bool GetD3D8VTableAddresses(void** outEndScene, void** outReset) {
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
                *outReset = vtable[14];
                *outEndScene = vtable[35];
                dbglog::Line("vtable=%p Reset=%p EndScene=%p", (void*)vtable, *outReset, *outEndScene);
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
    void* resetAddr = nullptr;
    if (!GetD3D8VTableAddresses(&endSceneAddr, &resetAddr)) {
        dbglog::Line("GetD3D8VTableAddresses failed");
        return false;
    }

    MH_STATUS s1 = MH_CreateHook(endSceneAddr, reinterpret_cast<void*>(&hkEndScene), reinterpret_cast<void**>(&oEndScene));
    dbglog::Line("MH_CreateHook(EndScene) = %d", (int)s1);
    if (s1 != MH_OK) return false;

    MH_STATUS s2 = MH_CreateHook(resetAddr, reinterpret_cast<void*>(&hkReset), reinterpret_cast<void**>(&oReset));
    dbglog::Line("MH_CreateHook(Reset) = %d", (int)s2);
    if (s2 != MH_OK) return false;

    MH_STATUS s3 = MH_EnableHook(endSceneAddr);
    dbglog::Line("MH_EnableHook(EndScene) = %d", (int)s3);
    if (s3 != MH_OK) return false;

    MH_STATUS s4 = MH_EnableHook(resetAddr);
    dbglog::Line("MH_EnableHook(Reset) = %d", (int)s4);
    if (s4 != MH_OK) return false;

    g_hookInstalled = true;
    return true;
}

void ShutdownD3D8Hook() {
    MH_DisableHook(MH_ALL_HOOKS);
    overlay::Shutdown();
    g_hookInstalled = false;
}

bool IsD3D8HookInstalled() { return g_hookInstalled.load(); }

} // namespace hook
