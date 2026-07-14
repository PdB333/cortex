#include "routes.h"
#include "../action/action.h"
#include "../overlay/overlay.h"

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace api {

void RegisterActionRoutes(httplib::Server& svr) {
    svr.Get("/actions", [](const httplib::Request&, httplib::Response& res) {
        json entries = json::array();
        for (const auto& entry : action::List()) {
            entries.push_back({{"id", entry.id}, {"timestamp_ms", entry.timestampMs},
                               {"description", entry.description}});
        }
        res.set_content(json{{"ok", true}, {"actions", entries},
                             {"checkpoint", action::Checkpoint()}}.dump(), "application/json");
    });

    svr.Post("/actions/rollback", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = req.body.empty() ? json::object() : json::parse(req.body);
            auto results = body.contains("checkpoint")
                ? action::RollbackTo(body.at("checkpoint").get<uint64_t>())
                : action::RollbackAll();
            json rolledBack = json::array();
            bool ok = true;
            for (const auto& item : results) {
                rolledBack.push_back({{"id", item.id}, {"ok", item.ok}});
                ok = ok && item.ok;
            }
            res.set_content(json{{"ok", ok}, {"rolled_back", rolledBack}}.dump(), "application/json");
            overlay::LogApiCall("POST /actions/rollback");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Post("/actions/clear", [](const httplib::Request&, httplib::Response& res) {
        action::Clear();
        res.set_content("{\"ok\":true}", "application/json");
        overlay::LogApiCall("POST /actions/clear");
    });
}

} // namespace api
