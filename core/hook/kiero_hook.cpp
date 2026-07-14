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
#include <utility>
#include <atomic>
#include <thread>
#include <chrono>
#include <mutex>

namespace hook {

namespace {
    std::atomic<bool> g_hookInstalled{false};
    std::atomic<int> g_backend{0}; // 0 none, 9/10/11/12 Direct3D, 20 OpenGL
    std::atomic<bool> g_retryStop{false};
    std::thread g_retryThread;

    // ---------------------------------------------------------------- D3D9
    typedef HRESULT(STDMETHODCALLTYPE* D3D9Present_t)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
    typedef HRESULT(STDMETHODCALLTYPE* D3D9Reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

    D3D9Present_t oPresent9 = nullptr;
    D3D9Reset_t oReset9 = nullptr;
    bool g_overlayInit9 = false;

    HRESULT STDMETHODCALLTYPE hkPresent9(IDirect3DDevice9* device, const RECT* srcRect, const RECT* destRect,
                                          HWND destWindowOverride, const RGNDATA* dirtyRegion) {
        if (!g_overlayInit9) {
            D3DDEVICE_CREATION_PARAMETERS cp = {};
            if (SUCCEEDED(device->GetCreationParameters(&cp)) && cp.hFocusWindow) {
                overlay::InitD3D9(device, cp.hFocusWindow);
                g_overlayInit9 = true;
            }
        }
        if (g_overlayInit9) {
            capture::OnPresentD3D9(device);
            overlay::OnFrameD3D9(device);
        }
        return oPresent9(device, srcRect, destRect, destWindowOverride, dirtyRegion);
    }

    HRESULT STDMETHODCALLTYPE hkReset9(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params) {
        if (g_overlayInit9) overlay::PreResetD3D9();
        HRESULT hr = oReset9(device, params);
        if (SUCCEEDED(hr) && g_overlayInit9) overlay::PostResetD3D9(device);
        return hr;
    }

    // --------------------------------------------------------------- D3D10
    typedef HRESULT(STDMETHODCALLTYPE* Present_t)(IDXGISwapChain*, UINT, UINT);
    typedef HRESULT(STDMETHODCALLTYPE* ResizeBuffers_t)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

    Present_t oPresent10 = nullptr;
    ResizeBuffers_t oResizeBuffers10 = nullptr;
    bool g_overlayInit10 = false;
    ID3D10Device* g_device10 = nullptr;

    HRESULT STDMETHODCALLTYPE hkPresent10(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
        if (!g_overlayInit10) {
            if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D10Device), reinterpret_cast<void**>(&g_device10)))) {
                DXGI_SWAP_CHAIN_DESC desc = {};
                if (SUCCEEDED(swapChain->GetDesc(&desc)) && desc.OutputWindow) {
                    overlay::InitD3D10(g_device10, desc.OutputWindow);
                    g_overlayInit10 = true;
                }
            }
        }

        if (g_overlayInit10) {
            ID3D10Texture2D* backBuffer = nullptr;
            if (SUCCEEDED(swapChain->GetBuffer(0, __uuidof(ID3D10Texture2D), reinterpret_cast<void**>(&backBuffer)))) {
                capture::OnPresentD3D10(backBuffer);

                ID3D10RenderTargetView* rtv = nullptr;
                g_device10->CreateRenderTargetView(backBuffer, nullptr, &rtv);
                if (rtv) {
                    g_device10->OMSetRenderTargets(1, &rtv, nullptr);
                    overlay::OnFrameD3D10(g_device10);
                    rtv->Release();
                }
                backBuffer->Release();
            }
        }

        return oPresent10(swapChain, syncInterval, flags);
    }

    HRESULT STDMETHODCALLTYPE hkResizeBuffers10(IDXGISwapChain* swapChain, UINT bufferCount, UINT width, UINT height,
                                                 DXGI_FORMAT newFormat, UINT swapChainFlags) {
        if (g_overlayInit10) overlay::PreResetD3D10();
        HRESULT hr = oResizeBuffers10(swapChain, bufferCount, width, height, newFormat, swapChainFlags);
        if (SUCCEEDED(hr) && g_overlayInit10 && g_device10) overlay::PostResetD3D10(g_device10);
        return hr;
    }

    // --------------------------------------------------------------- D3D11
    Present_t oPresent11 = nullptr;
    ResizeBuffers_t oResizeBuffers11 = nullptr;
    bool g_overlayInit11 = false;
    ID3D11Device* g_device11 = nullptr;
    ID3D11DeviceContext* g_context11 = nullptr;
    ID3D11RenderTargetView* g_rtv11 = nullptr;

    void ReleaseRenderTarget11() {
        if (g_rtv11) { g_rtv11->Release(); g_rtv11 = nullptr; }
    }

    void CreateRenderTarget11(IDXGISwapChain* swapChain) {
        ID3D11Texture2D* backBuffer = nullptr;
        if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer)))) return;
        g_device11->CreateRenderTargetView(backBuffer, nullptr, &g_rtv11);
        capture::OnPresentD3D11(backBuffer);
        backBuffer->Release();
    }

    HRESULT STDMETHODCALLTYPE hkPresent11(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
        if (!g_overlayInit11) {
            if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_device11)))) {
                g_device11->GetImmediateContext(&g_context11);
                DXGI_SWAP_CHAIN_DESC desc = {};
                if (SUCCEEDED(swapChain->GetDesc(&desc)) && desc.OutputWindow) {
                    CreateRenderTarget11(swapChain);
                    overlay::Init(g_device11, g_context11, desc.OutputWindow);
                    g_overlayInit11 = true;
                }
            }
        } else {
            CreateRenderTarget11(swapChain);
        }

        if (g_overlayInit11 && g_rtv11) {
            g_context11->OMSetRenderTargets(1, &g_rtv11, nullptr);
            overlay::OnFrame(g_device11, g_context11, g_rtv11);
        }
        ReleaseRenderTarget11();

        return oPresent11(swapChain, syncInterval, flags);
    }

    HRESULT STDMETHODCALLTYPE hkResizeBuffers11(IDXGISwapChain* swapChain, UINT bufferCount, UINT width, UINT height,
                                                 DXGI_FORMAT newFormat, UINT swapChainFlags) {
        if (g_overlayInit11) overlay::PreReset();
        ReleaseRenderTarget11();
        HRESULT hr = oResizeBuffers11(swapChain, bufferCount, width, height, newFormat, swapChainFlags);
        if (SUCCEEDED(hr) && g_overlayInit11) overlay::PostReset(g_device11, g_context11);
        return hr;
    }

    // --------------------------------------------------------------- D3D12
#ifdef CORTEX_D3D12
    typedef void (STDMETHODCALLTYPE* ExecuteCommandLists_t)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
    typedef HRESULT(STDMETHODCALLTYPE* Present12_t)(IDXGISwapChain*, UINT, UINT);
    typedef HRESULT(STDMETHODCALLTYPE* ResizeBuffers12_t)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

    ExecuteCommandLists_t oExecuteCommandLists = nullptr;
    Present12_t oPresent12 = nullptr;
    ResizeBuffers12_t oResizeBuffers12 = nullptr;

    struct FrameContext {
        ID3D12CommandAllocator* allocator = nullptr;
        UINT64 fenceValue = 0;
    };
    constexpr UINT kMaxD3D12Frames = 4;

    bool g_overlayInit12 = false;
    ID3D12Device* g_d3d12Device = nullptr;
    ID3D12CommandQueue* g_d3d12Queue = nullptr;
    std::mutex g_d3d12QueueMutex;
    UINT g_d3d12BufferCount = 0;
    FrameContext g_d3d12FrameCtx[kMaxD3D12Frames];
    ID3D12Resource* g_d3d12BackBuffers[kMaxD3D12Frames] = {};
    ID3D12GraphicsCommandList* g_d3d12CmdList = nullptr;
    ID3D12DescriptorHeap* g_d3d12RtvHeap = nullptr;
    UINT g_d3d12RtvDescSize = 0;
    ID3D12Fence* g_d3d12Fence = nullptr;
    HANDLE g_d3d12FenceEvent = nullptr;
    UINT64 g_d3d12FenceLastSignaled = 0;

    void STDMETHODCALLTYPE hkExecuteCommandLists(ID3D12CommandQueue* queue, UINT numLists, ID3D12CommandList* const* lists) {
        D3D12_COMMAND_QUEUE_DESC desc = queue->GetDesc();
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

    void ReleaseD3D12BackBuffers() {
        for (UINT i = 0; i < kMaxD3D12Frames; ++i) {
            if (g_d3d12BackBuffers[i]) { g_d3d12BackBuffers[i]->Release(); g_d3d12BackBuffers[i] = nullptr; }
        }
    }

    void ReleaseD3D12Resources() {
        ReleaseD3D12BackBuffers();
        for (UINT i = 0; i < kMaxD3D12Frames; ++i) {
            if (g_d3d12FrameCtx[i].allocator) { g_d3d12FrameCtx[i].allocator->Release(); g_d3d12FrameCtx[i].allocator = nullptr; }
        }
        if (g_d3d12RtvHeap) { g_d3d12RtvHeap->Release(); g_d3d12RtvHeap = nullptr; }
        if (g_d3d12CmdList) { g_d3d12CmdList->Release(); g_d3d12CmdList = nullptr; }
        if (g_d3d12Fence) { g_d3d12Fence->Release(); g_d3d12Fence = nullptr; }
        if (g_d3d12FenceEvent) { CloseHandle(g_d3d12FenceEvent); g_d3d12FenceEvent = nullptr; }
    }

    bool CreateD3D12Resources(IDXGISwapChain* swapChain) {
        DXGI_SWAP_CHAIN_DESC scDesc = {};
        if (FAILED(swapChain->GetDesc(&scDesc))) return false;

        g_d3d12BufferCount = scDesc.BufferCount;
        if (g_d3d12BufferCount == 0) g_d3d12BufferCount = 1;
        if (g_d3d12BufferCount > kMaxD3D12Frames) g_d3d12BufferCount = kMaxD3D12Frames;

        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.NumDescriptors = kMaxD3D12Frames;
        rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(g_d3d12Device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_d3d12RtvHeap)))) {
            dbglog::Line("kiero_hook: CreateDescriptorHeap (RTV) failed");
            return false;
        }
        g_d3d12RtvDescSize = g_d3d12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_d3d12RtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < g_d3d12BufferCount; ++i) {
            if (FAILED(swapChain->GetBuffer(i, IID_PPV_ARGS(&g_d3d12BackBuffers[i])))) {
                dbglog::Line("kiero_hook: GetBuffer(%u) failed", i);
                return false;
            }
            g_d3d12Device->CreateRenderTargetView(g_d3d12BackBuffers[i], nullptr, rtvHandle);
            rtvHandle.ptr += g_d3d12RtvDescSize;

            if (FAILED(g_d3d12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_d3d12FrameCtx[i].allocator)))) {
                dbglog::Line("kiero_hook: CreateCommandAllocator(%u) failed", i);
                return false;
            }
            g_d3d12FrameCtx[i].fenceValue = 0;
        }

        if (FAILED(g_d3d12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_d3d12FrameCtx[0].allocator,
                                                      nullptr, IID_PPV_ARGS(&g_d3d12CmdList)))) {
            dbglog::Line("kiero_hook: CreateCommandList failed");
            return false;
        }
        g_d3d12CmdList->Close();

        if (FAILED(g_d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_d3d12Fence)))) {
            dbglog::Line("kiero_hook: CreateFence failed");
            return false;
        }
        g_d3d12FenceEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (!g_d3d12FenceEvent) return false;

        return true;
    }

    HRESULT STDMETHODCALLTYPE hkPresent12(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
        ID3D12CommandQueue* queue = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_d3d12QueueMutex);
            queue = g_d3d12Queue;
            if (queue) queue->AddRef();
        }

        if (!g_overlayInit12 && queue) {
            if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&g_d3d12Device)))) {
                DXGI_SWAP_CHAIN_DESC scDesc = {};
                if (SUCCEEDED(swapChain->GetDesc(&scDesc)) && scDesc.OutputWindow && CreateD3D12Resources(swapChain)) {
                    overlay::InitD3D12(g_d3d12Device, static_cast<int>(g_d3d12BufferCount),
                                        DXGI_FORMAT_R8G8B8A8_UNORM, scDesc.OutputWindow);
                    g_overlayInit12 = true;
                }
            }
        }

        if (g_overlayInit12) {
            UINT backBufferIndex = 0;
            IDXGISwapChain3* sc3 = nullptr;
            if (SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&sc3)))) {
                backBufferIndex = sc3->GetCurrentBackBufferIndex();
                sc3->Release();
            }
            if (backBufferIndex < g_d3d12BufferCount) {
                FrameContext& frame = g_d3d12FrameCtx[backBufferIndex];

                // Don't touch this frame's allocator while the GPU might
                // still be executing the previous command list that used it.
                if (frame.fenceValue != 0 && g_d3d12Fence->GetCompletedValue() < frame.fenceValue) {
                    g_d3d12Fence->SetEventOnCompletion(frame.fenceValue, g_d3d12FenceEvent);
                    const DWORD wait = WaitForSingleObject(g_d3d12FenceEvent, 2000);
                    if (wait != WAIT_OBJECT_0) {
                        dbglog::Line("kiero_hook: D3D12 frame fence timeout/error (%lu); skipping overlay frame", wait);
                        queue->Release();
                        return oPresent12(swapChain, syncInterval, flags);
                    }
                }

                ID3D12Resource* backBuffer = g_d3d12BackBuffers[backBufferIndex];

                // Capture must run first, while the backbuffer is still in
                // PRESENT state -- it does its own resource-state dance.
                capture::OnPresentD3D12(g_d3d12Device, queue, backBuffer);

                frame.allocator->Reset();
                g_d3d12CmdList->Reset(frame.allocator, nullptr);

                D3D12_RESOURCE_BARRIER toRt = {};
                toRt.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                toRt.Transition.pResource = backBuffer;
                toRt.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                toRt.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                toRt.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                g_d3d12CmdList->ResourceBarrier(1, &toRt);

                D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_d3d12RtvHeap->GetCPUDescriptorHandleForHeapStart();
                rtvHandle.ptr += static_cast<SIZE_T>(backBufferIndex) * g_d3d12RtvDescSize;
                g_d3d12CmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

                overlay::OnFrameD3D12(g_d3d12CmdList);

                D3D12_RESOURCE_BARRIER toPresent = toRt;
                std::swap(toPresent.Transition.StateBefore, toPresent.Transition.StateAfter);
                g_d3d12CmdList->ResourceBarrier(1, &toPresent);

                g_d3d12CmdList->Close();

                ID3D12CommandList* lists[] = { g_d3d12CmdList };
                queue->ExecuteCommandLists(1, lists);

                ++g_d3d12FenceLastSignaled;
                queue->Signal(g_d3d12Fence, g_d3d12FenceLastSignaled);
                frame.fenceValue = g_d3d12FenceLastSignaled;
            }
        }

        if (queue) queue->Release();
        return oPresent12(swapChain, syncInterval, flags);
    }

    HRESULT STDMETHODCALLTYPE hkResizeBuffers12(IDXGISwapChain* swapChain, UINT bufferCount, UINT width, UINT height,
                                                 DXGI_FORMAT newFormat, UINT swapChainFlags) {
        ID3D12CommandQueue* queue = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_d3d12QueueMutex);
            queue = g_d3d12Queue;
            if (queue) queue->AddRef();
        }
        if (g_overlayInit12) {
            // Flush the GPU so no in-flight work still references the
            // backbuffers/allocators we're about to release.
            if (queue && g_d3d12Fence) {
                ++g_d3d12FenceLastSignaled;
                queue->Signal(g_d3d12Fence, g_d3d12FenceLastSignaled);
                if (g_d3d12Fence->GetCompletedValue() < g_d3d12FenceLastSignaled) {
                    g_d3d12Fence->SetEventOnCompletion(g_d3d12FenceLastSignaled, g_d3d12FenceEvent);
                    const DWORD wait = WaitForSingleObject(g_d3d12FenceEvent, 5000);
                    if (wait != WAIT_OBJECT_0) {
                        dbglog::Line("kiero_hook: D3D12 resize fence timeout/error (%lu); resize deferred", wait);
                        queue->Release();
                        return DXGI_ERROR_WAS_STILL_DRAWING;
                    }
                }
            }
            overlay::PreResetD3D12();
            ReleaseD3D12BackBuffers();
        }

        HRESULT hr = oResizeBuffers12(swapChain, bufferCount, width, height, newFormat, swapChainFlags);

        if (SUCCEEDED(hr) && g_overlayInit12) {
            DXGI_SWAP_CHAIN_DESC scDesc = {};
            swapChain->GetDesc(&scDesc);
            g_d3d12BufferCount = scDesc.BufferCount;
            if (g_d3d12BufferCount > kMaxD3D12Frames) g_d3d12BufferCount = kMaxD3D12Frames;

            D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_d3d12RtvHeap->GetCPUDescriptorHandleForHeapStart();
            for (UINT i = 0; i < g_d3d12BufferCount; ++i) {
                swapChain->GetBuffer(i, IID_PPV_ARGS(&g_d3d12BackBuffers[i]));
                g_d3d12Device->CreateRenderTargetView(g_d3d12BackBuffers[i], nullptr, rtvHandle);
                rtvHandle.ptr += g_d3d12RtvDescSize;
            }
            overlay::PostResetD3D12();
        }

        if (queue) queue->Release();
        return hr;
    }
#endif // CORTEX_D3D12

    // -------------------------------------------------------------- OpenGL
    typedef BOOL(WINAPI* SwapBuffers_t)(HDC);
    SwapBuffers_t oSwapBuffers = nullptr;
    bool g_overlayInitGL = false;

    BOOL WINAPI hkSwapBuffers(HDC hdc) {
        HWND hwnd = WindowFromDC(hdc);
        if (!g_overlayInitGL && hwnd) {
            overlay::InitOpenGL(hwnd);
            g_overlayInitGL = true;
        }
        if (g_overlayInitGL) {
            capture::OnPresentOpenGL(hwnd);
            overlay::OnFrameOpenGL();
        }
        return oSwapBuffers(hdc);
    }

} // namespace

bool InitKieroHook() {
    if (g_hookInstalled.load()) return true;
    kiero::Status::Enum status = kiero::init(kiero::RenderType::Auto);
    if (status != kiero::Status::Success) {
        dbglog::Line("kiero::init failed, status=%d", (int)status);
        return false;
    }

    kiero::RenderType::Enum type = kiero::getRenderType();
    dbglog::Line("kiero detected render type=%d", (int)type);

    switch (type) {
        case kiero::RenderType::D3D9:
            if (kiero::bind(17, reinterpret_cast<void**>(&oPresent9), reinterpret_cast<void*>(&hkPresent9)) != kiero::Status::Success) return false;
            if (kiero::bind(16, reinterpret_cast<void**>(&oReset9), reinterpret_cast<void*>(&hkReset9)) != kiero::Status::Success) return false;
            g_backend = 9; g_hookInstalled = true; return true;

        case kiero::RenderType::D3D10:
            if (kiero::bind(8, reinterpret_cast<void**>(&oPresent10), reinterpret_cast<void*>(&hkPresent10)) != kiero::Status::Success) return false;
            if (kiero::bind(13, reinterpret_cast<void**>(&oResizeBuffers10), reinterpret_cast<void*>(&hkResizeBuffers10)) != kiero::Status::Success) return false;
            g_backend = 10; g_hookInstalled = true; return true;

        case kiero::RenderType::D3D11:
            if (kiero::bind(8, reinterpret_cast<void**>(&oPresent11), reinterpret_cast<void*>(&hkPresent11)) != kiero::Status::Success) return false;
            if (kiero::bind(13, reinterpret_cast<void**>(&oResizeBuffers11), reinterpret_cast<void*>(&hkResizeBuffers11)) != kiero::Status::Success) return false;
            g_backend = 11; g_hookInstalled = true; return true;

#ifdef CORTEX_D3D12
        case kiero::RenderType::D3D12:
            if (kiero::bind(54, reinterpret_cast<void**>(&oExecuteCommandLists), reinterpret_cast<void*>(&hkExecuteCommandLists)) != kiero::Status::Success) return false;
            if (kiero::bind(140, reinterpret_cast<void**>(&oPresent12), reinterpret_cast<void*>(&hkPresent12)) != kiero::Status::Success) return false;
            if (kiero::bind(145, reinterpret_cast<void**>(&oResizeBuffers12), reinterpret_cast<void*>(&hkResizeBuffers12)) != kiero::Status::Success) return false;
            g_backend = 12; g_hookInstalled = true; return true;
#endif

        case kiero::RenderType::OpenGL:
            if (kiero::bind(336, reinterpret_cast<void**>(&oSwapBuffers), reinterpret_cast<void*>(&hkSwapBuffers)) != kiero::Status::Success) return false;
            g_backend = 20; g_hookInstalled = true; return true;

        default:
            dbglog::Line("kiero: unsupported/undetected render type %d (Vulkan is not supported by this hook)", (int)type);
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
    ReleaseD3D12Resources();
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
