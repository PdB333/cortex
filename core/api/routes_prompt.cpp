#include "routes.h"
#include "../prompt/prompt_queue.h"
#include "../overlay/overlay.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <cmath>
#include <string>

using json = nlohmann::json;

namespace api {
namespace {

std::string Trim(const std::string& value) {
    size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first]))) ++first;
    size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1]))) --last;
    return value.substr(first, last - first);
}

json ToJson(const prompt::PromptRequest& r) {
    json j{{"id", r.id},
           {"kind", prompt::ToString(r.kind)},
           {"status", prompt::ToString(r.status)},
           {"remaining_ms", prompt::RemainingMs(r)},
           {"answer_ready", prompt::CanAnswer(r)}};

    if (r.kind == prompt::Kind::TimedTest) {
        j["message"] = r.message;
        j["duration_seconds"] = r.duration_seconds;
        j["answer_type"] = prompt::ToString(r.answer_type);
    } else {
        j["label"] = r.label;
        j["current_value"] = r.current_value;
        j["target_value"] = r.target_value;
        j["answer_type"] = "text";
    }

    if (r.status == prompt::Status::Answered) j["response"] = r.response_value;
    return j;
}

void JsonError(httplib::Response& res, int status, const std::string& error) {
    res.status = status;
    res.set_content(json{{"ok", false}, {"error", error}}.dump(), "application/json");
}

int AnswerStatus(const std::string& error) {
    if (error == "prompt_not_found") return 404;
    if (error == "prompt_timer_active" || error == "prompt_already_answered") return 409;
    return 400;
}

} // namespace

void RegisterPromptRoutes(httplib::Server& svr) {
    svr.Post("/prompt/timed_test", [](const httplib::Request& req, httplib::Response& res) {
        try {
            const json body = json::parse(req.body);
            const std::string message = body.at("message").get<std::string>();
            const double duration = body.at("duration_seconds").get<double>();
            const std::string answerType = body.value("answer_type", std::string("number"));
            if (Trim(message).empty()) {
                JsonError(res, 400, "message_required");
                return;
            }
            if (!std::isfinite(duration) || duration <= 0.0) {
                JsonError(res, 400, "duration_seconds_must_be_positive");
                return;
            }
            if (answerType != "text" && answerType != "number") {
                JsonError(res, 400, "answer_type_must_be_text_or_number");
                return;
            }

            auto id = prompt::CreateTimedTest(message, duration, prompt::ParseAnswerType(answerType));
            if (!id.has_value()) {
                JsonError(res, 409, "a_prompt_is_already_pending");
                return;
            }
            res.set_content(json{{"ok", true}, {"id", *id}}.dump(), "application/json");
            overlay::LogApiCall("POST /prompt/timed_test \"" + message + "\"");
        } catch (const std::exception& e) {
            JsonError(res, 400, e.what());
        }
    });

    svr.Post("/prompt/value_change", [](const httplib::Request& req, httplib::Response& res) {
        try {
            const json body = json::parse(req.body);
            const std::string label = body.at("label").get<std::string>();
            const std::string target = body.at("target_value").get<std::string>();
            const std::string current = body.value("current_value", std::string());
            if (Trim(label).empty() || Trim(target).empty()) {
                JsonError(res, 400, "label_and_target_value_required");
                return;
            }

            auto id = prompt::CreateValueChange(label, target, current);
            if (!id.has_value()) {
                JsonError(res, 409, "a_prompt_is_already_pending");
                return;
            }
            res.set_content(json{{"ok", true}, {"id", *id}}.dump(), "application/json");
            overlay::LogApiCall("POST /prompt/value_change \"" + label + " -> " + target + "\"");
        } catch (const std::exception& e) {
            JsonError(res, 400, e.what());
        }
    });

    // Desktop consumes these two routes through cortex/private/route on the
    // authenticated local Named Pipe. They are intentionally absent from the
    // public MCP tool manifest so an AI cannot answer its own human prompt.
    svr.Get("/prompt/active", [](const httplib::Request&, httplib::Response& res) {
        prompt::NoteExternalPresenter();
        const auto active = prompt::GetActive();
        res.set_content(json{{"ok", true},
                             {"prompt", active.has_value() ? ToJson(*active) : json(nullptr)}}.dump(),
                        "application/json");
    });

    svr.Post(R"(/prompt/(\d+)/answer)", [](const httplib::Request& req, httplib::Response& res) {
        try {
            const int id = std::stoi(req.matches[1]);
            const json body = json::parse(req.body);
            if (!body.contains("value") ||
                (!body["value"].is_string() && !body["value"].is_number())) {
                JsonError(res, 400, "prompt_answer_required");
                return;
            }
            const std::string value = body["value"].is_string()
                ? body["value"].get<std::string>()
                : body["value"].dump();

            std::string error;
            if (!prompt::Answer(id, value, &error)) {
                JsonError(res, AnswerStatus(error), error);
                return;
            }
            res.set_content(json{{"ok", true}, {"id", id}, {"status", "answered"}}.dump(),
                            "application/json");
            overlay::LogApiCall("POST /prompt/" + std::to_string(id) + "/answer");
        } catch (const std::exception& e) {
            JsonError(res, 400, e.what());
        }
    });

    svr.Get(R"(/prompt/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
        const int id = std::stoi(req.matches[1]);
        const auto p = prompt::GetStatus(id);
        if (!p.has_value()) {
            JsonError(res, 404, "not_found");
            return;
        }
        res.set_content(ToJson(*p).dump(), "application/json");
    });
}

} // namespace api
