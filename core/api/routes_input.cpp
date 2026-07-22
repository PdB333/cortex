#include "routes.h"
#include "../hook/input_inject.h"
#include "../overlay/overlay.h"
#include "../action/action.h"

#include <windows.h>
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

    // Execute a scripted sequence of key/mouse/delay steps on a worker thread.
    //
    // Body:
    //   { "mode": "os" | "game",             (default "os")
    //     "steps": [
    //       { "vk": 0x57, "down": true },                // key down
    //       { "vk": 0x57, "down": false },               // key up
    //       { "vk": 0x20, "tap_ms": 80 },                // key down/hold/up
    //       { "mouse_button": 0, "down": true },
    //       { "mouse_move": { "dx": 120, "dy": 0 } },    // relative
    //       { "mouse_move_abs": { "x": 400, "y": 300 } },// client-absolute
    //       { "delay_ms": 250 }
    //     ] }
    //
    // Returns { ok, job_id, steps }. Poll GET /input/sequence/{id} for
    // status. DELETE /input/sequence/{id} cancels it.
    //
    // mode=os   : SendInput. Foreground required. Reaches DirectInput /
    //             RawInput consumers because it goes through the OS HID
    //             pipeline.
    // mode=game : PostMessage on the game's top-level HWND. Background-safe
    //             but only reaches games that read WM_KEY* / WM_MOUSE* /
    //             WM_CHAR. See input_inject.h for the honest caveats.
    svr.Post("/input/text", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto mutation = action::LockMutations();
            json body = json::parse(req.body);
            std::string s = body.at("text").get<std::string>();
            bool bg = body.value("background", false);
            int perChar = body.value("per_char_ms", 0);

            // UTF-8 -> UTF-16 via MultiByteToWideChar (Windows-native, no ICU).
            int wlen = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
            std::wstring w(wlen, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), wlen);

            int n = inputinject::TypeString(w, bg, perChar);
            res.set_content(json{{"ok", true}, {"delivered", n}, {"length", (int)w.size()}}.dump(),
                            "application/json");
            overlay::LogApiCall("POST /input/text n=" + std::to_string(n) +
                                (bg ? " bg" : " os"));
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Post("/input/sequence", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto mutation = action::LockMutations();
            json body = json::parse(req.body);

            std::string modeStr = body.value("mode", std::string("os"));
            inputinject::SequenceMode mode = inputinject::SequenceMode::Os;
            if (modeStr == "game")   mode = inputinject::SequenceMode::Game;
            else if (modeStr == "dinput") mode = inputinject::SequenceMode::DInput;
            else if (modeStr != "os") {
                res.status = 400;
                res.set_content(json{{"ok", false}, {"error", "invalid_mode"}}.dump(),
                                "application/json");
                return;
            }

            std::vector<inputinject::SequenceStep> steps;
            if (!body.contains("steps") || !body["steps"].is_array() || body["steps"].empty()) {
                res.status = 400;
                res.set_content(json{{"ok", false}, {"error", "steps_required"}}.dump(),
                                "application/json");
                return;
            }
            for (const auto& s : body["steps"]) {
                inputinject::SequenceStep st;
                if (s.contains("delay_ms")) st.delayMs = s["delay_ms"].get<int>();
                if (s.contains("vk")) {
                    st.vk = s["vk"].get<int>();
                    if (s.contains("tap_ms")) {
                        st.keyTapHoldMs = s["tap_ms"].get<int>();
                    } else {
                        st.keyDown = s.value("down", true);
                    }
                }
                if (s.contains("mouse_button")) {
                    st.mouseButton = s["mouse_button"].get<int>();
                    st.mouseButtonDown = s.value("down", true);
                }
                if (s.contains("mouse_move") && s["mouse_move"].is_object()) {
                    st.mouseMoveRel = true;
                    st.dx = s["mouse_move"].value("dx", 0);
                    st.dy = s["mouse_move"].value("dy", 0);
                }
                if (s.contains("mouse_move_abs") && s["mouse_move_abs"].is_object()) {
                    st.mouseMoveAbs = true;
                    st.ax = s["mouse_move_abs"].value("x", 0);
                    st.ay = s["mouse_move_abs"].value("y", 0);
                }
                steps.push_back(st);
            }

            int id = inputinject::SequenceStart(mode, std::move(steps));
            json out;
            out["ok"] = true;
            out["job_id"] = id;
            out["mode"] = modeStr;
            res.set_content(out.dump(), "application/json");
            overlay::LogApiCall("POST /input/sequence mode=" + modeStr +
                                " job=" + std::to_string(id));
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Get(R"(/input/sequence/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
        int id = std::stoi(req.matches[1].str());
        std::string status;
        int idx = 0, count = 0;
        if (!inputinject::SequenceStatus(id, status, idx, count)) {
            res.status = 404;
            res.set_content(json{{"ok", false}, {"error", "unknown_job"}}.dump(),
                            "application/json");
            return;
        }
        res.set_content(json{
            {"ok", true},
            {"job_id", id},
            {"status", status},
            {"step_index", idx},
            {"step_count", count}
        }.dump(), "application/json");
    });

    svr.Post("/input/record/start", [](const httplib::Request&, httplib::Response& res) {
        bool ok = inputinject::RecordStart();
        res.set_content(json{{"ok", ok}, {"recording", inputinject::IsRecording()}}.dump(),
                        "application/json");
        overlay::LogApiCall("POST /input/record/start");
    });

    svr.Post("/input/record/stop", [](const httplib::Request&, httplib::Response& res) {
        auto steps = inputinject::RecordStop();
        json arr = json::array();
        for (const auto& s : steps) {
            json j;
            if (s.delayMs > 0) j["delay_ms"] = s.delayMs;
            if (s.vk) { j["vk"] = s.vk; j["down"] = s.keyDown; }
            if (s.mouseButton >= 0) { j["mouse_button"] = s.mouseButton; j["down"] = s.mouseButtonDown; }
            if (s.mouseMoveAbs) j["mouse_move_abs"] = {{"x", s.ax}, {"y", s.ay}};
            arr.push_back(j);
        }
        res.set_content(json{{"ok", true}, {"steps", arr}, {"count", (int)steps.size()}}.dump(),
                        "application/json");
        overlay::LogApiCall("POST /input/record/stop n=" + std::to_string(steps.size()));
    });

    svr.Delete(R"(/input/sequence/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
        int id = std::stoi(req.matches[1].str());
        bool ok = inputinject::SequenceCancel(id);
        if (!ok) res.status = 404;
        res.set_content(json{{"ok", ok}}.dump(), "application/json");
    });
}

} // namespace api
