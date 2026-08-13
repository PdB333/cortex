#include "routes.h"
#include "pagination.h"
#include "response_contract.h"
#include "../action/action.h"
#include "../overlay/overlay.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <limits>

using json = nlohmann::json;

namespace api {
namespace {

bool ParseSizeParam(const httplib::Request& req,
                    const char* name,
                    size_t fallback,
                    size_t& value) {
    if (!req.has_param(name)) {
        value = fallback;
        return true;
    }
    const std::string raw = req.get_param_value(name);
    try {
        size_t consumed = 0;
        const unsigned long long parsed = std::stoull(raw, &consumed, 10);
        if (consumed != raw.size() || parsed > (std::numeric_limits<size_t>::max)()) return false;
        value = static_cast<size_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

void RegisterActionRoutes(httplib::Server& svr) {
    svr.Get("/actions", [](const httplib::Request& req, httplib::Response& res) {
        size_t offset = 0;
        size_t requestedLimit = 0;
        if (!ParseSizeParam(req, "offset", 0, offset) ||
            !ParseSizeParam(req, "limit", 0, requestedLimit)) {
            res.status = 400;
            res.set_content(response::Error("invalid_pagination", "offset and limit must be unsigned integers").dump(),
                            "application/json");
            return;
        }

        const auto page = pagination::Normalize(offset, requestedLimit, 100, 512);
        if (!page) {
            res.status = 400;
            const char* message = page.error == pagination::Error::RangeOverflow
                ? "offset plus limit overflows"
                : "limit exceeds the action journal maximum";
            res.set_content(response::Error("invalid_pagination", message).dump(), "application/json");
            return;
        }

        const auto all = action::List();
        json entries = json::array();
        const size_t begin = (std::min)(page.page.offset, all.size());
        const size_t end = (std::min)(page.page.end, all.size());
        for (size_t i = begin; i < end; ++i) {
            const auto& entry = all[i];
            entries.push_back({{"id", entry.id}, {"timestamp_ms", entry.timestampMs},
                               {"description", entry.description}});
        }

        json paginationInfo = {
            {"offset", page.page.offset},
            {"limit", page.page.limit},
            {"returned", entries.size()},
            {"total", all.size()},
            {"has_more", end < all.size()}
        };
        if (end < all.size()) paginationInfo["next_offset"] = end;
        else paginationInfo["next_offset"] = nullptr;

        res.set_content(json{{"ok", true}, {"actions", entries},
                             {"checkpoint", action::Checkpoint()},
                             {"pagination", paginationInfo}}.dump(), "application/json");
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
