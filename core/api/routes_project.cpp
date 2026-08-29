#include "routes.h"
#include "../process/address.h"
#include "../project/project.h"
#include "../overlay/overlay.h"

#include <nlohmann/json.hpp>
#include <sstream>

using json = nlohmann::json;

namespace api {

namespace {

uintptr_t ParseAddress(const json& jaddr) { return process::ResolveAddress(jaddr); }

std::string DecodePathSegment(const std::string& encoded) {
    std::string decoded;
    decoded.reserve(encoded.size());
    auto hex = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return 10 + value - 'a';
        if (value >= 'A' && value <= 'F') return 10 + value - 'A';
        return -1;
    };
    for (size_t i = 0; i < encoded.size(); ++i) {
        if (encoded[i] == '%' && i + 2 < encoded.size()) {
            const int hi = hex(encoded[i + 1]);
            const int lo = hex(encoded[i + 2]);
            if (hi >= 0 && lo >= 0) {
                decoded.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        decoded.push_back(encoded[i]);
    }
    return decoded;
}

// Same hex-string-or-number flexibility as ParseAddress, but signed --
// pointer path offsets are routinely negative (Cheat Engine et al. show
// them as e.g. "-0x4").
int64_t ParseSignedOffset(const json& j) {
    if (j.is_string()) return std::stoll(j.get<std::string>(), nullptr, 0);
    return j.get<int64_t>();
}

} // namespace

void RegisterProjectRoutes(httplib::Server& svr) {
    svr.Get("/project", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(project::GetAll().dump(2), "application/json");
        overlay::LogApiCall("GET /project");
    });

    svr.Post("/project/address", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            std::string name = body.at("name").get<std::string>();
            uintptr_t address = ParseAddress(body.at("address"));
            std::string type = body.value("type", "");
            std::string notes = body.value("notes", "");
            project::SetAddress(name, address, type, notes);
            res.set_content(json{{"ok", true}}.dump(), "application/json");
            overlay::LogApiCall("POST /project/address " + name);
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Delete(R"(/project/address/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
        std::string name = DecodePathSegment(req.matches[1]);
        bool ok = project::RemoveAddress(name);
        res.set_content(json{{"ok", ok}}.dump(), "application/json");
        overlay::LogApiCall("DELETE /project/address/" + name);
    });

    svr.Post("/project/pointer_path", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            std::string name = body.at("name").get<std::string>();
            std::string moduleName = body.value("module", "");
            int64_t baseOffset = ParseSignedOffset(body.at("base_offset"));
            std::vector<int64_t> offsets;
            for (const auto& o : body.value("offsets", json::array())) offsets.push_back(ParseSignedOffset(o));
            std::string finalType = body.value("final_type", "");
            std::string notes = body.value("notes", "");
            project::SetPointerPath(name, moduleName, baseOffset, offsets, finalType, notes);
            res.set_content(json{{"ok", true}}.dump(), "application/json");
            overlay::LogApiCall("POST /project/pointer_path " + name);
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Delete(R"(/project/pointer_path/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
        std::string name = DecodePathSegment(req.matches[1]);
        bool ok = project::RemovePointerPath(name);
        res.set_content(json{{"ok", ok}}.dump(), "application/json");
        overlay::LogApiCall("DELETE /project/pointer_path/" + name);
    });

    svr.Get(R"(/project/resolve/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
        std::string name = DecodePathSegment(req.matches[1]);
        auto addr = project::ResolvePointerPath(name);
        json out;
        out["ok"] = addr.has_value();
        if (addr) {
            std::ostringstream s;
            s << "0x" << std::hex << *addr;
            out["address"] = s.str();
        } else {
            out["error"] = "unresolvable_pointer_path";
        }
        res.set_content(out.dump(), "application/json");
        overlay::LogApiCall("GET /project/resolve/" + name);
    });

    svr.Post("/project/note", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            std::string text = body.at("text").get<std::string>();
            std::vector<std::string> tags = body.value("tags", std::vector<std::string>{});
            int id = project::AddNote(text, tags);
            res.set_content(json{{"ok", true}, {"id", id}}.dump(), "application/json");
            overlay::LogApiCall("POST /project/note");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Delete(R"(/project/note/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
        int id = std::stoi(req.matches[1]);
        bool ok = project::RemoveNote(id);
        res.set_content(json{{"ok", ok}}.dump(), "application/json");
        overlay::LogApiCall("DELETE /project/note/" + std::to_string(id));
    });
}

} // namespace api
