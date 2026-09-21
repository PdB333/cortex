#include "kiero_hook.h"
#include "../overlay/overlay.h"
#include "../capture/capture.h"
#include "../log.h"

#include <windows.h>
#include <d3d9.h>
#include <d3d10.h>
#include <d3d11.h>
#ifdef CORTEX_D3D12
#include <d3d12.h>
#endif
#include <dxgi1_4.h>
#include <MinHook.h>
#include <kiero.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace hook {

namespace {
std::atomic<bool> g_hookInstalled{false};
std::atomic<int> g_backend{0}; // 0 none, 9/10/11/12 Direct3D, 20 OpenGL
std::atomic<bool> g_retryStop{false};
std::thread g_retryThread;

// The renderer hooks are instrumentation points only. Cortex no longer creates
// a graphics UI/context inside the target process. Each Present/SwapBuffers
// hook captures frames on demand and pumps work that must execute on the game
// render thread.

// ---------------------------------------------------------------- D3D9
using D3D9Present_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
D3D9Present_t oPresent9 = nullptr;
bool g_frameInit9 = false;

HRESULT STDMETHODCALLTYPE hkPresent9(IDirect3DDevice9* device, const RECT* srcRect, const RECT* destRect,
                                      HWND destWindowOverride, const RGNDATA* dirtyRegion) {
    if (!g_frameInit9) {
        D3DDEVICE_CREATION_PARAMETERS cp = {};
        if (SUCCEEDED(device->GetCreationParameters(&cp)) && cp.hFocusWindow) {
            overlay::InitD3D9(device, cp.hFocusWindow);
            g_frameInit9 = true;
        }
    }
    capture::OnPresentD3D9(device);
    if (g_frameInit9) overlay::OnFrameD3D9(device);
    return oPresent9(device, srcRect, destRect, destWindowOverride, dirtyRegion);
}

// --------------------------------------------------------------- D3D10
using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
Present_t oPresent10 = nullptr;
bool g_frameInit10 = false;
ID3D10Device* g_device10 = nullptr;

HRESULT STDMETHODCALLTYPE hkPresent10(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    if (!g_frameInit10) {
        if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D10Device), reinterpret_cast<void**>(&g_device10)))) {
            DXGI_SWAP_CHAIN_DESC desc = {};
            if (SUCCEEDED(swapChain->GetDesc(&desc)) && desc.OutputWindow) {
                overlay::InitD3D10(g_device10, desc.OutputWindow);
                g_frameInit10 = true;
            }
        }
    }

    ID3D10Texture2D* backBuffer = nullptr;
    if (SUCCEEDED(swapChain->GetBuffer(0, __uuidof(ID3D10Texture2D), reinterpret_cast<void**>(&backBuffer)))) {
        capture::OnPresentD3D10(backBuffer);
        backBuffer->Release();
    }
    if (g_frameInit10) overlay::OnFrameD3D10(g_device10);
    return oPresent10(swapChain, syncInterval, flags);
}

// --------------------------------------------------------------- D3D11
Present_t oPresent11 = nullptr;
bool g_frameInit11 = false;
ID3D11Device* g_device11 = nullptr;
ID3D11DeviceContext* g_context11 = nullptr;

HRESULT STDMETHODCALLTYPE hkPresent11(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    if (!g_frameInit11) {
        if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_device11)))) {
            g_device11->GetImmediateContext(&g_context11);
            DXGI_SWAP_CHAIN_DESC desc = {};
            if (SUCCEEDED(swapChain->GetDesc(&desc)) && desc.OutputWindow) {
                overlay::Init(g_device11, g_context11, desc.OutputWindow);
                g_frameInit11 = true;
            }
        }
    }

    ID3D11Texture2D* backBuffer = nullptr;
    if (SUCCEEDED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer)))) {
        capture::OnPresentD3D11(backBuffer);
        backBuffer->Release();
    }
    if (g_frameInit11) overlay::OnFrame(g_device11, g_context11, nullptr);
    return oPresent11(swapChain, syncInterval, flags);
}

// --------------------------------------------------------------- D3D12
#ifdef CORTEX_D3D12
using ExecuteCommandLists_t = void (STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using Present12_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
ExecuteCommandLists_t oExecuteCommandLists = nullptr;
Present12_t oPresent12 = nullptr;

bool g_frameInit12 = false;
ID3D12Device* g_d3d12Device = nullptr;
ID3D12CommandQueue* g_d3d12Queue = nullptr;
std::mutex g_d3d12QueueMutex;

void STDMETHODCALLTYPE hkExecuteCommandLists(ID3D12CommandQueue* queue, UINT numLists, ID3D12CommandList* const* lists) {
    const D3D12_COMMAND_QUEUE_DESC desc = queue->GetDesc();
    if (desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        std::lock_guard<std::mutex> lock(g_d3d12QueueMutex);
        if (g_d3d12Queue != queue) {
            queue->AddRef();
            if (g_d3d12Queue) g_d3d12Queue->Release();
            g_d3d12Queue = queue;
        }
    }
    oExecuteCommandLists(queue, numLists, lists);
}

HRESULT STDMETHODCALLTYPE hkPresent12(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    ID3D12CommandQueue* queue = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_d3d12QueueMutex);
        queue = g_d3d12Queue;
        if (queue) queue->AddRef();
    }

    if (!g_frameInit12 && queue) {
        if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&g_d3d12Device)))) {
            DXGI_SWAP_CHAIN_DESC desc = {};
            if (SUCCEEDED(swapChain->GetDesc(&desc)) && desc.OutputWindow) {
                overlay::InitD3D12(g_d3d12Device, 0, DXGI_FORMAT_UNKNOWN, desc.OutputWindow);
                g_frameInit12 = true;
            }
        }
    }

    if (g_frameInit12 && queue && g_d3d12Device) {
        UINT backBufferIndex = 0;
        IDXGISwapChain3* sc3 = nullptr;
        if (SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&sc3)))) {
            backBufferIndex = sc3->GetCurrentBackBufferIndex();
            sc3->Release();
        }
        ID3D12Resource* backBuffer = nullptr;
        if (SUCCEEDED(swapChain->GetBuffer(backBufferIndex, IID_PPV_ARGS(&backBuffer)))) {
            capture::OnPresentD3D12(g_d3d12Device, queue, backBuffer);
            backBuffer->Release();
        }
        overlay::OnFrameD3D12(nullptr);
    }

    if (queue) queue->Release();
    return oPresent12(swapChain, syncInterval, flags);
}
#endif // CORTEX_D3D12

// -------------------------------------------------------------- OpenGL
using SwapBuffers_t = BOOL(WINAPI*)(HDC);
SwapBuffers_t oSwapBuffers = nullptr;
bool g_frameInitGL = false;

BOOL WINAPI hkSwapBuffers(HDC hdc) {
    HWND hwnd = WindowFromDC(hdc);
    if (!g_frameInitGL && hwnd) {
        overlay::InitOpenGL(hwnd);
        g_frameInitGL = true;
    }
    if (hwnd) capture::OnPresentOpenGL(hwnd);
    if (g_frameInitGL) overlay::OnFrameOpenGL();
    return oSwapBuffers(hdc);
}

} // namespace

bool InitKieroHook() {
    if (g_hookInstalled.load()) return true;
    const kiero::Status::Enum status = kiero::init(kiero::RenderType::Auto);
    if (status != kiero::Status::Success) {
        dbglog::Line("kiero::init failed, status=%d", static_cast<int>(status));
        return false;
    }

    const kiero::RenderType::Enum type = kiero::getRenderType();
    dbglog::Line("kiero detected render type=%d", static_cast<int>(type));

    switch (type) {
        case kiero::RenderType::D3D9:
            if (kiero::bind(17, reinterpret_cast<void**>(&oPresent9), reinterpret_cast<void*>(&hkPresent9)) != kiero::Status::Success) return false;
            g_backend = 9; g_hookInstalled = true; return true;

        case kiero::RenderType::D3D10:
            if (kiero::bind(8, reinterpret_cast<void**>(&oPresent10), reinterpret_cast<void*>(&hkPresent10)) != kiero::Status::Success) return false;
            g_backend = 10; g_hookInstalled = true; return true;

        case kiero::RenderType::D3D11:
            if (kiero::bind(8, reinterpret_cast<void**>(&oPresent11), reinterpret_cast<void*>(&hkPresent11)) != kiero::Status::Success) return false;
            g_backend = 11; g_hookInstalled = true; return true;

#ifdef CORTEX_D3D12
        case kiero::RenderType::D3D12:
            if (kiero::bind(54, reinterpret_cast<void**>(&oExecuteCommandLists), reinterpret_cast<void*>(&hkExecuteCommandLists)) != kiero::Status::Success) return false;
            if (kiero::bind(140, reinterpret_cast<void**>(&oPresent12), reinterpret_cast<void*>(&hkPresent12)) != kiero::Status::Success) return false;
            g_backend = 12; g_hookInstalled = true; return true;
#endif

        case kiero::RenderType::OpenGL:
            if (kiero::bind(336, reinterpret_cast<void**>(&oSwapBuffers), reinterpret_cast<void*>(&hkSwapBuffers)) != kiero::Status::Success) return false;
            g_backend = 20; g_hookInstalled = true; return true;

        default:
            dbglog::Line("kiero: unsupported/undetected render type %d (Vulkan is not supported by this hook)", static_cast<int>(type));
            return false;
    }
}

bool StartKieroHookWithRetry() {
    if (InitKieroHook()) return true;
    if (g_retryThread.joinable()) return false;
    g_retryStop = false;
    g_retryThread = std::thread([] {
        constexpr int kMaxAttempts = 120; // one minute at 500 ms
        for (int attempt = 1; attempt <= kMaxAttempts && !g_retryStop.load(); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (g_retryStop.load()) break;
            if (InitKieroHook()) {
                dbglog::Line("kiero hook installed after delayed retry #%d", attempt);
                return;
            }
        }
        if (!g_retryStop.load()) dbglog::Line("kiero delayed hook retry window expired");
    });
    return false;
}

void StopKieroHookRetry() {
    g_retryStop = true;
    if (g_retryThread.joinable()) g_retryThread.join();
}

void ShutdownKieroHook() {
    StopKieroHookRetry();
    MH_DisableHook(MH_ALL_HOOKS);
    overlay::Shutdown();

#ifdef CORTEX_D3D12
    if (g_d3d12Device) { g_d3d12Device->Release(); g_d3d12Device = nullptr; }
    {
        std::lock_guard<std::mutex> lock(g_d3d12QueueMutex);
        if (g_d3d12Queue) { g_d3d12Queue->Release(); g_d3d12Queue = nullptr; }
    }
#endif
    if (g_context11) { g_context11->Release(); g_context11 = nullptr; }
    if (g_device11) { g_device11->Release(); g_device11 = nullptr; }
    if (g_device10) { g_device10->Release(); g_device10 = nullptr; }

    kiero::shutdown();
    g_frameInit9 = false;
    g_frameInit10 = false;
    g_frameInit11 = false;
#ifdef CORTEX_D3D12
    g_frameInit12 = false;
#endif
    g_frameInitGL = false;
    g_hookInstalled = false;
    g_backend = 0;
}

bool IsKieroHookInstalled() { return g_hookInstalled.load(); }

std::string GetKieroRenderBackend() {
    switch (g_backend.load()) {
        case 9: return "d3d9";
        case 10: return "d3d10";
        case 11: return "d3d11";
        case 12: return "d3d12";
        case 20: return "opengl";
        default: return "none";
    }
}

} // namespace hook
