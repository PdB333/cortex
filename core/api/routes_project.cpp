#include "routes.h"
#include "../process/address.h"
#include "../project/project.h"
#include "../overlay/overlay.h"
#include "../memory/memory.h"
#include <algorithm>
#include <cctype>

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

    // Investigation knowledge. Assertions are always client claims. Only the
    // verification route can attach a server-observed byte invariant result.
    svr.Get("/project/knowledge", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(json{{"ok", true}, {"records", project::GetKnowledge()}}.dump(),
                        "application/json");
    });

    svr.Get(R"(/project/knowledge/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
        const std::string id = DecodePathSegment(req.matches[1]);
        const json record = project::FindKnowledge(id);
        res.status = record.is_null() ? 404 : 200;
        res.set_content(json{{"ok", !record.is_null()}, {"record", record}}.dump(),
                        "application/json");
    });

    svr.Post("/project/knowledge", [](const httplib::Request& req, httplib::Response& res) {
        try {
            const json input = json::parse(req.body);
            json stored;
            std::string error;
            if (!project::PutKnowledge(input, stored, error)) {
                res.status = error == "revision_conflict" ? 409 :
                             error == "persistence_failed" ? 500 : 400;
                res.set_content(json{{"ok", false}, {"error", error}}.dump(), "application/json");
                return;
            }
            res.set_content(json{{"ok", true}, {"record", stored}}.dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Post(R"(/project/knowledge/([^/]+)/verify)", [](const httplib::Request& req, httplib::Response& res) {
        try {
            const std::string id = DecodePathSegment(req.matches[1]);
            const json before = project::FindKnowledge(id);
            if (before.is_null()) {
                res.status = 404;
                res.set_content(json{{"ok", false}, {"error", "knowledge_not_found"}}.dump(),
                                "application/json");
                return;
            }
            const int revision = before.value("revision", 0);
            const json checks = before.value("checks", json::array());
            json results = json::array();
            // This verifier deliberately supports only byte invariants. A
            // passing invariant does not prove the human-language hypothesis.
            bool passed = !checks.empty();
            for (const auto& check : checks) {
                const std::string addressText = check.at("address").get<std::string>();
                const std::string hex = check.at("expected_hex").get<std::string>();
                const size_t length = hex.size() / 2;
                uintptr_t address = 0;
                bool readOk = false;
                std::string actual;
                try {
                    address = process::ResolveAddress(addressText);
                    std::vector<uint8_t> bytes;
                    // ResolveAddress may yield zero for an unknown module; a
                    // missing region is recorded as a failure, not a match.
                    if (address && memory::ReadBytes(address, length, bytes) && bytes.size() == length) {
                        readOk = true;
                        const char digits[] = "0123456789abcdef";
                        for (const uint8_t byte : bytes) {
                            actual += digits[byte >> 4];
                            actual += digits[byte & 15];
                        }
                    }
                } catch (const std::exception&) {}
                std::string expected = hex;
                std::transform(expected.begin(), expected.end(), expected.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                const bool match = readOk && actual == expected;
                passed = passed && match;
                results.push_back({{"address", addressText}, {"passed", match},
                                   {"readable", readOk}});
            }
            const json verification = {
                {"status", checks.empty() ? "not_applicable" : passed ? "passed" : "failed"},
                {"scope", "byte_invariant"},
                {"results", results}
            };
            json updated;
            std::string error;
            if (!project::RecordKnowledgeVerification(id, revision, verification, updated, error)) {
                res.status = error == "revision_conflict" ? 409 : 500;
                res.set_content(json{{"ok", false}, {"error", error}}.dump(), "application/json");
                return;
            }
            res.set_content(json{{"ok", true}, {"record", updated}}.dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

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
