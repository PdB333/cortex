#include "capture.h"
#ifdef CORTEX_D3D8
#include "capture_common.h"
#include "../log.h"
#include <cstring>
#include <d3d8.h>

namespace capture {

void OnEndScene(IDirect3DDevice8* device) {
    std::lock_guard<std::mutex> lock(detail::g_mutex);
    if (!detail::g_requested || !device) return;
    dbglog::Line("capture: OnEndScene handling request");

    IDirect3DSurface8* backBuffer = nullptr;
    IDirect3DSurface8* sysSurface = nullptr;

    do {
        HRESULT hr;
        hr = device->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
        if (FAILED(hr)) { dbglog::Line("capture: GetBackBuffer failed hr=0x%08lX", (unsigned long)hr); break; }

        D3DSURFACE_DESC desc = {};
        hr = backBuffer->GetDesc(&desc);
        if (FAILED(hr)) { dbglog::Line("capture: GetDesc failed hr=0x%08lX", (unsigned long)hr); break; }
        dbglog::Line("capture: backbuffer %ux%u format=%d", desc.Width, desc.Height, (int)desc.Format);

        hr = device->CreateImageSurface(desc.Width, desc.Height, desc.Format, &sysSurface);
        if (FAILED(hr)) { dbglog::Line("capture: CreateImageSurface failed hr=0x%08lX", (unsigned long)hr); break; }

        hr = device->CopyRects(backBuffer, nullptr, 0, sysSurface, nullptr);
        if (FAILED(hr)) { dbglog::Line("capture: CopyRects failed hr=0x%08lX", (unsigned long)hr); break; }

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

} // namespace capture
#endif // CORTEX_D3D8
