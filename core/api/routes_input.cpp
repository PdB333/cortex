#include "routes.h"
#include "../hook/input_inject.h"
#include "../overlay/overlay.h"
#include "../action/action.h"

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace api {

void RegisterInputRoutes(httplib::Server& svr) {
    svr.Post("/input/key", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto mutation = action::LockMutations();
            json body = json::parse(req.body);
            int vk = body.at("vk").get<int>();
            bool down = body.at("down").get<bool>();
            bool ok = inputinject::KeyEvent(vk, down);
            res.set_content(json{{"ok", ok}}.dump(), "application/json");
            overlay::LogApiCall("POST /input/key vk=" + std::to_string(vk) + (down ? " down" : " up"));
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Post("/input/key_tap", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto mutation = action::LockMutations();
            json body = json::parse(req.body);
            int vk = body.at("vk").get<int>();
            int holdMs = body.value("hold_ms", 50);
            bool ok = inputinject::KeyTap(vk, holdMs);
            res.set_content(json{{"ok", ok}}.dump(), "application/json");
            overlay::LogApiCall("POST /input/key_tap vk=" + std::to_string(vk));
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Post("/input/mouse_button", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto mutation = action::LockMutations();
            json body = json::parse(req.body);
            int button = body.at("button").get<int>();
            bool down = body.at("down").get<bool>();
            bool ok = inputinject::MouseButtonEvent(button, down);
            json out;
            out["ok"] = ok;
            if (!ok) out["error"] = "invalid_button";
            res.set_content(out.dump(), "application/json");
            overlay::LogApiCall("POST /input/mouse_button " + std::to_string(button) + (down ? " down" : " up"));
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Post("/input/mouse_move", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto mutation = action::LockMutations();
            json body = json::parse(req.body);
            int dx = body.at("dx").get<int>();
            int dy = body.at("dy").get<int>();
            bool ok = inputinject::MouseMove(dx, dy);
            res.set_content(json{{"ok", ok}}.dump(), "application/json");
            overlay::LogApiCall("POST /input/mouse_move " + std::to_string(dx) + "," + std::to_string(dy));
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });
}

} // namespace api
