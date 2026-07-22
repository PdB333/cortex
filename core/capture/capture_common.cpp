#include "capture_common.h"
#include "capture.h"
#include "../log.h"
#include "../overlay/overlay.h"
#include <windows.h>
#include <chrono>
#include <cstring>
#include <utility>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace capture::detail {

std::mutex g_mutex;
std::condition_variable g_cv;
bool g_requested = false;
bool g_ready = false;
std::vector<uint8_t> g_png;
std::vector<uint8_t> g_lastPng;
unsigned long long g_lastPngTickMs = 0;

namespace {
    // BGRA (D3D memory layout) -> RGBA (stb expects) in place.
    void BgraToRgba(uint8_t* data, size_t pixelCount) {
        for (size_t i = 0; i < pixelCount; ++i) {
            uint8_t* px = data + i * 4;
            std::swap(px[0], px[2]); // B <-> R, G stays, A stays
        }
    }
}

void FinishCapture(std::vector<uint8_t>& pixels, uint32_t width, uint32_t height, bool swapRB) {
    if (swapRB) {
        BgraToRgba(pixels.data(), static_cast<size_t>(width) * height);
    }

    int outLen = 0;
    unsigned char* png = stbi_write_png_to_mem(
        pixels.data(), static_cast<int>(width) * 4,
        static_cast<int>(width), static_cast<int>(height), 4, &outLen);

    if (png) {
        g_png.assign(png, png + outLen);
        g_lastPng.assign(png, png + outLen);
        g_lastPngTickMs = GetTickCount64();
        STBIW_FREE(png);
        g_ready = true;
    }
}

} // namespace capture::detail

namespace capture {

bool GetLastPng(std::vector<uint8_t>& out, unsigned long long* outAgeMs) {
    std::lock_guard<std::mutex> lock(detail::g_mutex);
    if (detail::g_lastPng.empty()) return false;
    out = detail::g_lastPng;
    if (outAgeMs) *outAgeMs = GetTickCount64() - detail::g_lastPngTickMs;
    return true;
}

// GDI fallback: use PrintWindow to snapshot whichever top-level window the
// overlay is attached to. Works even when the window is not focused and even
// when the game's Present hook is not firing, as long as the window is not
// iconified. PrintWindow with PW_RENDERFULLCONTENT lets DWM composite the
// current visible frame for us -- for windowed / borderless-windowed games
// this is the single best "background" capture path with no extra deps.
bool PrintWindowFallback(std::vector<uint8_t>& out_png) {
    HWND hwnd = overlay::GetHwnd();
    HWND root = hwnd ? GetAncestor(hwnd, GA_ROOT) : nullptr;
    if (!root || IsIconic(root)) return false;

    RECT rc{};
    if (!GetClientRect(root, &rc)) return false;
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0) return false;

    HDC screenDc = GetDC(nullptr);
    if (!screenDc) return false;
    HDC memDc = CreateCompatibleDC(screenDc);
    ReleaseDC(nullptr, screenDc);
    if (!memDc) return false;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(memDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) { DeleteDC(memDc); return false; }

    HGDIOBJ prev = SelectObject(memDc, dib);
    // PW_RENDERFULLCONTENT (0x2) required for D3D/DWM-rendered surfaces --
    // without it PrintWindow captures only the GDI-drawn parts (usually just
    // a black rectangle for hardware-accelerated content).
    BOOL ok = PrintWindow(root, memDc, 0x2 /* PW_RENDERFULLCONTENT */ | 0x1 /* PW_CLIENTONLY */);
    SelectObject(memDc, prev);

    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    std::memcpy(pixels.data(), bits, pixels.size());
    DeleteObject(dib);
    DeleteDC(memDc);

    if (!ok) return false;

    // GDI DIB is BGRA with A=0 (no alpha channel). Force alpha=255 so the
    // resulting PNG isn't fully transparent, and swap BGRA -> RGBA for stb.
    for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
        std::swap(pixels[i], pixels[i + 2]);
        pixels[i + 3] = 0xFF;
    }

    int outLen = 0;
    unsigned char* png = stbi_write_png_to_mem(
        pixels.data(), width * 4, width, height, 4, &outLen);
    if (!png) return false;
    out_png.assign(png, png + outLen);
    STBIW_FREE(png);

    // Also refresh the last-frame cache so subsequent /screenshot?mode=last
    // returns this GDI grab if nothing newer arrives from the render loop.
    {
        std::lock_guard<std::mutex> lock(detail::g_mutex);
        detail::g_lastPng = out_png;
        detail::g_lastPngTickMs = GetTickCount64();
    }
    return true;
}

bool RequestCapture(std::vector<uint8_t>& out_png, int timeout_ms) {
    dbglog::Line("capture: RequestCapture enter");
    std::unique_lock<std::mutex> lock(detail::g_mutex);
    detail::g_ready = false;
    detail::g_requested = true;

    bool got = detail::g_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                      [] { return detail::g_ready; });
    detail::g_requested = false;
    dbglog::Line("capture: RequestCapture wait done, got=%d ready=%d", (int)got, (int)detail::g_ready);

    if (got && detail::g_ready) {
        out_png = detail::g_png;
        return true;
    }
    return false;
}

} // namespace capture
