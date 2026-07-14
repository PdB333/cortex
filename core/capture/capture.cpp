#include "capture.h"
#ifdef CORTEX_KIERO
#include "capture_common.h"
#include "../log.h"
#include <cstring>
#include <utility>
#include <d3d9.h>
#include <d3d10.h>
#include <d3d11.h>
#ifdef CORTEX_D3D12
#include <d3d12.h>
#endif
#include <gl/GL.h>

namespace capture {

void OnPresentD3D9(IDirect3DDevice9* device) {
    std::lock_guard<std::mutex> lock(detail::g_mutex);
    if (!detail::g_requested || !device) return;
    dbglog::Line("capture: OnPresentD3D9 handling request");

    IDirect3DSurface9* backBuffer = nullptr;
    IDirect3DSurface9* sysSurface = nullptr;

    do {
        HRESULT hr = device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
        if (FAILED(hr)) { dbglog::Line("capture: GetBackBuffer failed hr=0x%08lX", (unsigned long)hr); break; }

        D3DSURFACE_DESC desc = {};
        hr = backBuffer->GetDesc(&desc);
        if (FAILED(hr)) { dbglog::Line("capture: GetDesc failed hr=0x%08lX", (unsigned long)hr); break; }

        hr = device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format,
                                                  D3DPOOL_SYSTEMMEM, &sysSurface, nullptr);
        if (FAILED(hr)) { dbglog::Line("capture: CreateOffscreenPlainSurface failed hr=0x%08lX", (unsigned long)hr); break; }

        hr = device->GetRenderTargetData(backBuffer, sysSurface);
        if (FAILED(hr)) { dbglog::Line("capture: GetRenderTargetData failed hr=0x%08lX", (unsigned long)hr); break; }

        D3DLOCKED_RECT locked = {};
        hr = sysSurface->LockRect(&locked, nullptr, D3DLOCK_READONLY);
        if (FAILED(hr)) { dbglog::Line("capture: LockRect failed hr=0x%08lX", (unsigned long)hr); break; }

        std::vector<uint8_t> pixels(static_cast<size_t>(desc.Width) * desc.Height * 4);
        const uint8_t* src = reinterpret_cast<const uint8_t*>(locked.pBits);
        for (UINT y = 0; y < desc.Height; ++y) {
            memcpy(pixels.data() + static_cast<size_t>(y) * desc.Width * 4,
                   src + static_cast<size_t>(y) * locked.Pitch,
                   static_cast<size_t>(desc.Width) * 4);
        }
        sysSurface->UnlockRect();

        detail::FinishCapture(pixels, desc.Width, desc.Height);
    } while (false);

    if (backBuffer) backBuffer->Release();
    if (sysSurface) sysSurface->Release();

    detail::g_requested = false;
    detail::g_cv.notify_all();
}

void OnPresentD3D10(ID3D10Texture2D* backBuffer) {
    std::lock_guard<std::mutex> lock(detail::g_mutex);
    if (!detail::g_requested || !backBuffer) return;
    dbglog::Line("capture: OnPresentD3D10 handling request");

    ID3D10Device* device = nullptr;
    ID3D10Texture2D* staging = nullptr;

    do {
        backBuffer->GetDevice(&device);
        if (!device) break;

        D3D10_TEXTURE2D_DESC desc = {};
        backBuffer->GetDesc(&desc);

        D3D10_TEXTURE2D_DESC stagingDesc = desc;
        stagingDesc.Usage = D3D10_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
        stagingDesc.MiscFlags = 0;
        stagingDesc.SampleDesc.Count = 1;
        stagingDesc.SampleDesc.Quality = 0;

        if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &staging))) {
            dbglog::Line("capture: CreateTexture2D (staging) failed");
            break;
        }

        if (desc.SampleDesc.Count > 1) {
            device->ResolveSubresource(staging, 0, backBuffer, 0, desc.Format);
        } else {
            device->CopyResource(staging, backBuffer);
        }

        D3D10_MAPPED_TEXTURE2D mapped = {};
        if (FAILED(staging->Map(0, D3D10_MAP_READ, 0, &mapped))) {
            dbglog::Line("capture: Map failed");
            break;
        }

        std::vector<uint8_t> pixels(static_cast<size_t>(desc.Width) * desc.Height * 4);
        const uint8_t* src = reinterpret_cast<const uint8_t*>(mapped.pData);
        for (UINT y = 0; y < desc.Height; ++y) {
            memcpy(pixels.data() + static_cast<size_t>(y) * desc.Width * 4,
                   src + static_cast<size_t>(y) * mapped.RowPitch,
                   static_cast<size_t>(desc.Width) * 4);
        }
        staging->Unmap(0);

        detail::FinishCapture(pixels, desc.Width, desc.Height);
    } while (false);

    if (staging) staging->Release();
    if (device) device->Release();

    detail::g_requested = false;
    detail::g_cv.notify_all();
}

void OnPresentD3D11(ID3D11Texture2D* backBuffer) {
    std::lock_guard<std::mutex> lock(detail::g_mutex);
    if (!detail::g_requested || !backBuffer) return;
    dbglog::Line("capture: OnPresentD3D11 handling request");

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11Texture2D* staging = nullptr;

    do {
        backBuffer->GetDevice(&device);
        if (!device) break;
        device->GetImmediateContext(&ctx);
        if (!ctx) break;

        D3D11_TEXTURE2D_DESC desc = {};
        backBuffer->GetDesc(&desc);

        D3D11_TEXTURE2D_DESC stagingDesc = desc;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDesc.MiscFlags = 0;
        stagingDesc.SampleDesc.Count = 1;
        stagingDesc.SampleDesc.Quality = 0;

        if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &staging))) {
            dbglog::Line("capture: CreateTexture2D (staging) failed");
            break;
        }

        if (desc.SampleDesc.Count > 1) {
            ctx->ResolveSubresource(staging, 0, backBuffer, 0, desc.Format);
        } else {
            ctx->CopyResource(staging, backBuffer);
        }

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
            dbglog::Line("capture: Map failed");
            break;
        }

        std::vector<uint8_t> pixels(static_cast<size_t>(desc.Width) * desc.Height * 4);
        const uint8_t* src = reinterpret_cast<const uint8_t*>(mapped.pData);
        for (UINT y = 0; y < desc.Height; ++y) {
            memcpy(pixels.data() + static_cast<size_t>(y) * desc.Width * 4,
                   src + static_cast<size_t>(y) * mapped.RowPitch,
                   static_cast<size_t>(desc.Width) * 4);
        }
        ctx->Unmap(staging, 0);

        detail::FinishCapture(pixels, desc.Width, desc.Height);
    } while (false);

    if (staging) staging->Release();
    if (ctx) ctx->Release();
    if (device) device->Release();

    detail::g_requested = false;
    detail::g_cv.notify_all();
}

#ifdef CORTEX_D3D12
// D3D12's Present hook doesn't get a ready-to-record command list from the
// game (its own lists are already closed/executed by then), so capture
// records and submits its own throwaway command list on the same queue the
// game uses, syncing via a dedicated fence -- acceptable here since capture
// only ever happens in response to a rare HTTP request, not every frame.
void OnPresentD3D12(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* backBuffer) {
    std::lock_guard<std::mutex> lock(detail::g_mutex);
    if (!detail::g_requested || !device || !queue || !backBuffer) return;
    dbglog::Line("capture: OnPresentD3D12 handling request");

    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* cmdList = nullptr;
    ID3D12Resource* readback = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE fenceEvent = nullptr;

    bool gpuTimedOut = false;
    do {
        D3D12_RESOURCE_DESC desc = backBuffer->GetDesc();

        UINT64 totalBytes = 0;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &totalBytes);

        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))) {
            dbglog::Line("capture: CreateCommandAllocator failed");
            break;
        }
        if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
                                              IID_PPV_ARGS(&cmdList)))) {
            dbglog::Line("capture: CreateCommandList failed");
            break;
        }

        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_READBACK;

        D3D12_RESOURCE_DESC bufDesc = {};
        bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufDesc.Width = totalBytes;
        bufDesc.Height = 1;
        bufDesc.DepthOrArraySize = 1;
        bufDesc.MipLevels = 1;
        bufDesc.Format = DXGI_FORMAT_UNKNOWN;
        bufDesc.SampleDesc.Count = 1;
        bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        if (FAILED(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&readback)))) {
            dbglog::Line("capture: CreateCommittedResource (readback) failed");
            break;
        }

        D3D12_RESOURCE_BARRIER toSrc = {};
        toSrc.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toSrc.Transition.pResource = backBuffer;
        toSrc.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        toSrc.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        toSrc.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &toSrc);

        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = readback;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = footprint;

        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = backBuffer;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;

        cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        D3D12_RESOURCE_BARRIER toPresent = toSrc;
        std::swap(toPresent.Transition.StateBefore, toPresent.Transition.StateAfter);
        cmdList->ResourceBarrier(1, &toPresent);

        if (FAILED(cmdList->Close())) { dbglog::Line("capture: cmdList->Close failed"); break; }

        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
            dbglog::Line("capture: CreateFence failed");
            break;
        }
        fenceEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (!fenceEvent) break;

        ID3D12CommandList* lists[] = { cmdList };
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence, 1);
        if (fence->GetCompletedValue() < 1) {
            fence->SetEventOnCompletion(1, fenceEvent);
            const DWORD wait = WaitForSingleObject(fenceEvent, 5000);
            if (wait != WAIT_OBJECT_0) {
                dbglog::Line("capture: D3D12 fence timeout/error (%lu); abandoning in-flight resources safely", wait);
                gpuTimedOut = true;
                break;
            }
        }

        D3D12_RANGE readRange = { 0, static_cast<SIZE_T>(totalBytes) };
        void* mappedPtr = nullptr;
        if (FAILED(readback->Map(0, &readRange, &mappedPtr))) {
            dbglog::Line("capture: readback->Map failed");
            break;
        }

        UINT width = static_cast<UINT>(desc.Width);
        UINT height = desc.Height;
        std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
        const uint8_t* srcPtr = reinterpret_cast<const uint8_t*>(mappedPtr);
        for (UINT y = 0; y < height; ++y) {
            memcpy(pixels.data() + static_cast<size_t>(y) * width * 4,
                   srcPtr + static_cast<size_t>(y) * footprint.Footprint.RowPitch,
                   static_cast<size_t>(width) * 4);
        }
        D3D12_RANGE writtenRange = { 0, 0 };
        readback->Unmap(0, &writtenRange);

        bool swapRB = (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                       desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
        detail::FinishCapture(pixels, width, height, swapRB);
    } while (false);

    // If the GPU did not acknowledge the fence, these objects may still be in
    // use. Deliberately leak this one capture set instead of risking a UAF in
    // the graphics driver. The request is still cleared so this cannot loop.
    if (!gpuTimedOut) {
        if (fenceEvent) CloseHandle(fenceEvent);
        if (fence) fence->Release();
        if (readback) readback->Release();
        if (cmdList) cmdList->Release();
        if (allocator) allocator->Release();
    }

    detail::g_requested = false;
    detail::g_cv.notify_all();
}
#endif // CORTEX_D3D12

void OnPresentOpenGL(HWND hwnd) {
    std::lock_guard<std::mutex> lock(detail::g_mutex);
    if (!detail::g_requested) return;
    dbglog::Line("capture: OnPresentOpenGL handling request");

    RECT rect = {};
    if (!hwnd || !GetClientRect(hwnd, &rect)) {
        detail::g_requested = false;
        detail::g_cv.notify_all();
        return;
    }
    UINT width = static_cast<UINT>(rect.right - rect.left);
    UINT height = static_cast<UINT>(rect.bottom - rect.top);
    if (width == 0 || height == 0) {
        detail::g_requested = false;
        detail::g_cv.notify_all();
        return;
    }

    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    glReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height),
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    // glReadPixels's origin is bottom-left; PNG (and stb_image_write) expect
    // top-left, so flip rows before encoding.
    std::vector<uint8_t> rowBuf(static_cast<size_t>(width) * 4);
    for (UINT y = 0; y < height / 2; ++y) {
        uint8_t* top = pixels.data() + static_cast<size_t>(y) * width * 4;
        uint8_t* bottom = pixels.data() + static_cast<size_t>(height - 1 - y) * width * 4;
        memcpy(rowBuf.data(), top, static_cast<size_t>(width) * 4);
        memcpy(top, bottom, static_cast<size_t>(width) * 4);
        memcpy(bottom, rowBuf.data(), static_cast<size_t>(width) * 4);
    }

    detail::FinishCapture(pixels, width, height, /*swapRB=*/false);

    detail::g_requested = false;
    detail::g_cv.notify_all();
}

} // namespace capture
#endif // CORTEX_KIERO
