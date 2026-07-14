#include "routes.h"
#include "../prompt/prompt_queue.h"
#include "../overlay/overlay.h"

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace api {

namespace {
    json ToJson(const prompt::PromptRequest& r) {
        json j;
        j["id"] = r.id;
        j["kind"] = prompt::ToString(r.kind);
        j["status"] = prompt::ToString(r.status);
        if (r.status == prompt::Status::Answered) j["response"] = r.response_value;
        return j;
    }
}

void RegisterPromptRoutes(httplib::Server& svr) {
    svr.Post("/prompt/timed_test", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            std::string message = body.at("message").get<std::string>();
            double duration = body.at("duration_seconds").get<double>();
            if (duration <= 0.0) {
                res.status = 400;
                res.set_content(json{{"ok", false}, {"error", "duration_seconds_must_be_positive"}}.dump(),
                                 "application/json");
                return;
            }
            prompt::AnswerType type = prompt::ParseAnswerType(body.value("answer_type", std::string("number")));

            auto id = prompt::CreateTimedTest(message, duration, type);
            if (!id.has_value()) {
                res.status = 409;
                res.set_content(json{{"ok", false}, {"error", "a_prompt_is_already_pending"}}.dump(),
                                 "application/json");
                return;
            }
            res.set_content(json{{"ok", true}, {"id", *id}}.dump(), "application/json");
            overlay::LogApiCall("POST /prompt/timed_test \"" + message + "\"");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Post("/prompt/value_change", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            std::string label = body.at("label").get<std::string>();
            std::string target = body.at("target_value").get<std::string>();
            std::string current = body.value("current_value", std::string(""));

            auto id = prompt::CreateValueChange(label, target, current);
            if (!id.has_value()) {
                res.status = 409;
                res.set_content(json{{"ok", false}, {"error", "a_prompt_is_already_pending"}}.dump(),
                                 "application/json");
                return;
            }
            res.set_content(json{{"ok", true}, {"id", *id}}.dump(), "application/json");
            overlay::LogApiCall("POST /prompt/value_change \"" + label + " -> " + target + "\"");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Get(R"(/prompt/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
        int id = std::stoi(req.matches[1]);
        auto p = prompt::GetStatus(id);
        if (!p.has_value()) {
            res.status = 404;
            res.set_content(json{{"ok", false}, {"error", "not_found"}}.dump(), "application/json");
            return;
        }
        res.set_content(ToJson(*p).dump(), "application/json");
    });
}

} // namespace api
