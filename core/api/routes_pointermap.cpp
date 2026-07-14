#include "routes.h"
#include "../pointermap/pointermap.h"
#include "../overlay/overlay.h"

#include <nlohmann/json.hpp>
#include <sstream>

using json = nlohmann::json;

namespace api {
namespace {

uintptr_t Address(const json& value) {
    if (value.is_string()) return static_cast<uintptr_t>(std::stoull(value.get<std::string>(), nullptr, 0));
    return static_cast<uintptr_t>(value.get<uint64_t>());
}

std::string Hex(uintptr_t value) { std::ostringstream out; out << "0x" << std::hex << value; return out.str(); }

json PathJson(const pointermap::Path& path) {
    return {{"module", path.module}, {"base_offset", path.baseOffset}, {"offsets", path.offsets},
            {"sessions", path.sessions}, {"score", path.score}};
}

json InfoJson(const pointermap::MapInfo& info) {
    return {{"name", info.name}, {"target", Hex(info.target)}, {"created_ms", info.createdMs},
            {"path_count", info.pathCount}, {"truncated", info.truncated}};
}

} // namespace

void RegisterPointerMapRoutes(httplib::Server& svr) {
    svr.Post("/pointermap/capture", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            pointermap::MapInfo info{};
            std::string error;
            const bool ok = pointermap::Capture(body.at("name").get<std::string>(), Address(body.at("target")),
                                                body.value("max_depth", 5), body.value("max_offset", 4096u),
                                                info, error);
            res.status = ok ? 200 : 400;
            res.set_content(ok ? json{{"ok", true}, {"pointermap", InfoJson(info)}}.dump()
                               : json{{"ok", false}, {"error", error}}.dump(), "application/json");
            overlay::LogApiCall("POST /pointermap/capture " + body.at("name").get<std::string>());
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Get("/pointermap/list", [](const httplib::Request&, httplib::Response& res) {
        json maps = json::array();
        for (const auto& info : pointermap::List()) maps.push_back(InfoJson(info));
        res.set_content(json{{"ok", true}, {"pointermaps", maps}}.dump(), "application/json");
    });

    svr.Post("/pointermap/intersect", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            std::vector<std::string> names = body.at("names").get<std::vector<std::string>>();
            std::vector<pointermap::Path> found;
            std::string error;
            if (!pointermap::Intersect(names, found, error)) {
                res.status = 400;
                res.set_content(json{{"ok", false}, {"error", error}}.dump(), "application/json");
                return;
            }
            json paths = json::array();
            for (const auto& path : found) paths.push_back(PathJson(path));
            res.set_content(json{{"ok", true}, {"paths", paths}, {"count", paths.size()}}.dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Delete(R"(/pointermap/([A-Za-z0-9_-]+))", [](const httplib::Request& req, httplib::Response& res) {
        const bool ok = pointermap::Remove(req.matches[1]);
        res.status = ok ? 200 : 404;
        res.set_content(json{{"ok", ok}}.dump(), "application/json");
    });
}

} // namespace api
