#include "routes.h"
#include "../capture/capture.h"
#include "../overlay/overlay.h"

#include <windows.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace api {

namespace {
    // Base64 encode (no external dependency needed for this small helper).
    std::string Base64Encode(const std::vector<uint8_t>& data) {
        static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve(((data.size() + 2) / 3) * 4);
        size_t i = 0;
        while (i + 2 < data.size()) {
            uint32_t n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
            out += table[(n >> 18) & 0x3F];
            out += table[(n >> 12) & 0x3F];
            out += table[(n >> 6) & 0x3F];
            out += table[n & 0x3F];
            i += 3;
        }
        size_t rem = data.size() - i;
        if (rem == 1) {
            uint32_t n = data[i] << 16;
            out += table[(n >> 18) & 0x3F];
            out += table[(n >> 12) & 0x3F];
            out += "==";
        } else if (rem == 2) {
            uint32_t n = (data[i] << 16) | (data[i + 1] << 8);
            out += table[(n >> 18) & 0x3F];
            out += table[(n >> 12) & 0x3F];
            out += table[(n >> 6) & 0x3F];
            out += "=";
        }
        return out;
    }
}

void RegisterScreenshotRoutes(httplib::Server& svr) {
    svr.Get("/screenshot", [](const httplib::Request& req, httplib::Response& res) {
        // Default raised from 2000ms: live-measured EndScene gaps (RDP-hosted
        // D3D8 present throttling, confirmed via timestamped cortex_debug.log)
        // reach 5+ seconds even in a healthy, unminimized game window, so
        // anything shorter surfaces a spurious capture_timeout even though
        // the render loop and capture logic are both working correctly.
        int timeoutMs = 8000;
        std::string timeoutParam = req.get_param_value("timeout_ms");
        if (!timeoutParam.empty()) {
            try {
                timeoutMs = std::stoi(timeoutParam);
            } catch (const std::exception&) {
                timeoutMs = 8000;
            }
        }
        if (timeoutMs < 100) timeoutMs = 100;
        if (timeoutMs > 20000) timeoutMs = 20000;

        std::vector<uint8_t> png;
        bool ok = capture::RequestCapture(png, timeoutMs);

        if (!ok) {
            HWND hwnd = overlay::GetHwnd();
            // The device's focus/render window may be a child of the actual
            // top-level window that gets minimized -- check the root ancestor.
            HWND root = hwnd ? GetAncestor(hwnd, GA_ROOT) : nullptr;
            bool minimized = root && IsIconic(root);
            res.status = 504;
            res.set_content(json{
                {"ok", false},
                {"error", minimized ? "window_minimized" : "capture_timeout"},
                {"message", minimized
                     ? "The game window is minimized: rendering (and therefore any capture) is "
                       "suspended until it is restored."
                     : "No frame received in time."}
            }.dump(), "application/json");
            return;
        }

        std::string encoding = req.get_param_value("encoding");
        if (encoding == "base64") {
            json out;
            out["ok"] = true;
            out["image_base64"] = Base64Encode(png);
            res.set_content(out.dump(), "application/json");
        } else {
            res.set_content(reinterpret_cast<const char*>(png.data()), png.size(), "image/png");
        }
        overlay::LogApiCall("GET /screenshot");
    });
}

} // namespace api
