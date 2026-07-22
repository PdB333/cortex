#include "routes.h"
#include "../hook/net_hook.h"
#include "../overlay/overlay.h"

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace api {

void RegisterNetRoutes(httplib::Server& svr) {
    svr.Post("/network/capture", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            bool on = body.at("enabled").get<bool>();
            nethook::SetCaptureEnabled(on);
            res.set_content(json{{"ok", true}, {"enabled", on}}.dump(), "application/json");
            overlay::LogApiCall(std::string("POST /network/capture ") + (on ? "on" : "off"));
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Get("/network/events", [](const httplib::Request& req, httplib::Response& res) {
        size_t max = 200;
        auto lim = req.get_param_value("limit");
        if (!lim.empty()) try { max = (size_t)std::stoi(lim); } catch (...) {}
        auto ev = nethook::Snapshot(max);
        json arr = json::array();
        for (const auto& e : ev) {
            arr.push_back({
                {"id", e.id}, {"tick_ms", e.tickMs},
                {"dir", e.direction == 0 ? "recv" : "send"},
                {"socket", e.socket}, {"size", e.size},
                {"preview_hex", e.previewHex}
            });
        }
        res.set_content(json{{"ok", true}, {"enabled", nethook::IsCaptureEnabled()},
                             {"events", arr}}.dump(), "application/json");
    });
}

} // namespace api
