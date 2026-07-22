#include "routes.h"
#include "../overlay/overlay.h"

#include <windows.h>
#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::json;

namespace api {

namespace {

HWND TopLevelHwnd() {
    HWND h = overlay::GetHwnd();
    return h ? GetAncestor(h, GA_ROOT) : nullptr;
}

std::string HwndTitle(HWND h) {
    // GetWindowTextLengthA returns the ANSI count -- use the W variants
    // throughout to avoid mangling non-ASCII titles (Steam overlays, some
    // localizations, ...).
    int len = GetWindowTextLengthW(h);
    if (len <= 0) return {};
    std::wstring w(len, L'\0');
    GetWindowTextW(h, w.data(), len + 1);
    int u8 = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), len, nullptr, 0, nullptr, nullptr);
    std::string out(u8, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), len, out.data(), u8, nullptr, nullptr);
    return out;
}

std::string HwndClass(HWND h) {
    char buf[256] = {};
    GetClassNameA(h, buf, sizeof(buf));
    return buf;
}

json Describe(HWND h) {
    if (!h) return json{{"ok", false}, {"error", "no_window"}};
    RECT rc{}, client{};
    GetWindowRect(h, &rc);
    GetClientRect(h, &client);
    return json{
        {"ok", true},
        {"hwnd", reinterpret_cast<uintptr_t>(h)},
        {"title", HwndTitle(h)},
        {"class", HwndClass(h)},
        {"pid", []{ DWORD pid = GetCurrentProcessId(); return (uint32_t)pid; }()},
        {"rect", {{"left", rc.left}, {"top", rc.top}, {"right", rc.right}, {"bottom", rc.bottom}}},
        {"client", {{"width", client.right}, {"height", client.bottom}}},
        {"visible", (bool)IsWindowVisible(h)},
        {"minimized", (bool)IsIconic(h)},
        {"maximized", (bool)IsZoomed(h)},
        {"focused", GetForegroundWindow() == h}
    };
}

} // namespace

void RegisterWindowRoutes(httplib::Server& svr) {
    svr.Get("/window", [](const httplib::Request&, httplib::Response& res) {
        HWND h = TopLevelHwnd();
        res.set_content(Describe(h).dump(), "application/json");
    });

    svr.Post("/window/focus", [](const httplib::Request&, httplib::Response& res) {
        HWND h = TopLevelHwnd();
        if (!h) { res.status = 404; res.set_content(json{{"ok", false}}.dump(), "application/json"); return; }
        // AllowSetForegroundWindow lets us bypass Windows' focus-stealing
        // prevention when the caller is a well-known automation surface.
        AllowSetForegroundWindow(ASFW_ANY);
        if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
        SetForegroundWindow(h);
        BringWindowToTop(h);
        SetActiveWindow(h);
        res.set_content(json{{"ok", true}, {"focused", GetForegroundWindow() == h}}.dump(),
                        "application/json");
        overlay::LogApiCall("POST /window/focus");
    });

    svr.Post("/window/restore", [](const httplib::Request&, httplib::Response& res) {
        HWND h = TopLevelHwnd();
        if (!h) { res.status = 404; res.set_content(json{{"ok", false}}.dump(), "application/json"); return; }
        ShowWindow(h, SW_RESTORE);
        res.set_content(json{{"ok", true}}.dump(), "application/json");
        overlay::LogApiCall("POST /window/restore");
    });

    svr.Post("/window/minimize", [](const httplib::Request&, httplib::Response& res) {
        HWND h = TopLevelHwnd();
        if (!h) { res.status = 404; res.set_content(json{{"ok", false}}.dump(), "application/json"); return; }
        ShowWindow(h, SW_MINIMIZE);
        res.set_content(json{{"ok", true}}.dump(), "application/json");
        overlay::LogApiCall("POST /window/minimize");
    });

    svr.Post("/window/move", [](const httplib::Request& req, httplib::Response& res) {
        try {
            HWND h = TopLevelHwnd();
            if (!h) { res.status = 404; res.set_content(json{{"ok", false}}.dump(), "application/json"); return; }
            json body = json::parse(req.body);
            int x = body.at("x").get<int>();
            int y = body.at("y").get<int>();
            int w = body.value("width", 0);
            int hgt = body.value("height", 0);
            UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
            if (w == 0 || hgt == 0) flags |= SWP_NOSIZE;
            SetWindowPos(h, nullptr, x, y, w, hgt, flags);
            res.set_content(Describe(h).dump(), "application/json");
            overlay::LogApiCall("POST /window/move");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });
}

} // namespace api
