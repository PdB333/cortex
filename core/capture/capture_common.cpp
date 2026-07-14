#include "capture_common.h"
#include "capture.h"
#include "../log.h"
#include <chrono>
#include <utility>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace capture::detail {

std::mutex g_mutex;
std::condition_variable g_cv;
bool g_requested = false;
bool g_ready = false;
std::vector<uint8_t> g_png;

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
        STBIW_FREE(png);
        g_ready = true;
    }
}

} // namespace capture::detail

namespace capture {

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
