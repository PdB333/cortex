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

        // mode selection -- see agents.md / README for the full contract.
        //   render : hook Present in the render loop (default legacy path).
        //   window : GDI PrintWindow of the game's top-level HWND -- works
        //            with the game in the background as long as it isn't
        //            minimized.
        //   last   : return the most recent PNG ever produced, without
        //            blocking. Fails only if nothing has ever been captured.
        //   auto   : try render (short timeout), then window, then last.
        std::string mode = req.get_param_value("mode");
        if (mode.empty()) mode = "render";

        std::vector<uint8_t> png;
        std::string source;
        std::string error;

        auto tryRender = [&](int t) {
            if (capture::RequestCapture(png, t)) { source = "render"; return true; }
            return false;
        };
        auto tryWindow = [&]() {
            if (capture::PrintWindowFallback(png)) { source = "window"; return true; }
            return false;
        };
        auto tryLast = [&]() {
            unsigned long long age = 0;
            if (capture::GetLastPng(png, &age)) { source = "last"; return true; }
            return false;
        };

        bool ok = false;
        if (mode == "render") {
            ok = tryRender(timeoutMs);
            if (!ok) error = "capture_timeout";
        } else if (mode == "window") {
            ok = tryWindow();
            if (!ok) error = "printwindow_failed";
        } else if (mode == "last") {
            ok = tryLast();
            if (!ok) error = "no_frame_yet";
        } else if (mode == "auto") {
            // Short render attempt first: if the game is presenting, this
            // returns almost immediately. Otherwise fall back to window,
            // then to the last known frame.
            int quick = timeoutMs < 1500 ? timeoutMs : 1500;
            ok = tryRender(quick) || tryWindow() || tryLast();
            if (!ok) error = "all_backends_failed";
        } else {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", "invalid_mode"}}.dump(),
                            "application/json");
            return;
        }

        if (!ok) {
            HWND hwnd = overlay::GetHwnd();
            HWND root = hwnd ? GetAncestor(hwnd, GA_ROOT) : nullptr;
            bool minimized = root && IsIconic(root);
            res.status = 504;
            res.set_content(json{
                {"ok", false},
                {"error", minimized ? "window_minimized" : error},
                {"mode", mode},
                {"message", minimized
                     ? "The game window is minimized: no backend can capture a live frame. "
                       "Try mode=last to get the most recent cached frame."
                     : "Capture failed. Try mode=auto for automatic fallback."}
            }.dump(), "application/json");
            return;
        }

        std::string encoding = req.get_param_value("encoding");
        if (encoding == "base64") {
            json out;
            out["ok"] = true;
            out["source"] = source;
            out["image_base64"] = Base64Encode(png);
            res.set_content(out.dump(), "application/json");
        } else {
            // Also surface the source in a custom header so binary consumers
            // can tell which backend produced this PNG without decoding it.
            res.set_header("X-Cortex-Capture-Source", source);
            res.set_content(reinterpret_cast<const char*>(png.data()), png.size(), "image/png");
        }
        overlay::LogApiCall("GET /screenshot mode=" + mode + " src=" + source);
    });
}

} // namespace api
