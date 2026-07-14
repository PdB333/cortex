#include "routes.h"
#include "../struct/structs.h"
#include "../overlay/overlay.h"
#include "../action/action.h"
#include "../struct/infer.h"

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace api {

namespace {

uintptr_t ParseAddress(const json& jaddr) {
    if (jaddr.is_string()) {
        return static_cast<uintptr_t>(std::stoull(jaddr.get<std::string>(), nullptr, 0));
    }
    return static_cast<uintptr_t>(jaddr.get<uint64_t>());
}

} // namespace

void RegisterStructRoutes(httplib::Server& svr) {
    svr.Post("/struct/define", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto mutation = action::LockMutations();
            json body = json::parse(req.body);
            std::string name = body.at("name").get<std::string>();

            std::vector<structs::Field> fields;
            for (const auto& jf : body.at("fields")) {
                structs::Field f;
                f.name = jf.at("name").get<std::string>();
                f.offset = jf.at("offset").get<int64_t>();
                f.type = jf.at("type").get<std::string>();
                f.count = jf.value("count", 0);
                fields.push_back(f);
            }

            std::vector<structs::Field> previous;
            bool existed = false;
            for (const auto& def : structs::List()) {
                if (def.name == name) { previous = def.fields; existed = true; break; }
            }
            structs::Define(name, fields);
            action::Record("struct/define " + name, [name, previous, existed] {
                return existed ? structs::Define(name, previous) : structs::Remove(name);
            });
            res.set_content(json{{"ok", true}}.dump(), "application/json");
            overlay::LogApiCall("POST /struct/define " + name);
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Delete(R"(/struct/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
        auto mutation = action::LockMutations();
        std::string name = req.matches[1];
        std::vector<structs::Field> previous;
        for (const auto& def : structs::List()) if (def.name == name) { previous = def.fields; break; }
        bool ok = structs::Remove(name);
        if (ok) action::Record("struct/remove " + name, [name, previous] { return structs::Define(name, previous); });
        res.status = ok ? 200 : 404;
        res.set_content(json{{"ok", ok}}.dump(), "application/json");
        overlay::LogApiCall("DELETE /struct/" + name);
    });

    svr.Get("/struct/list", [](const httplib::Request&, httplib::Response& res) {
        json arr = json::array();
        for (const auto& s : structs::List()) {
            json fields = json::array();
            for (const auto& f : s.fields) {
                fields.push_back({{"name", f.name}, {"offset", f.offset}, {"type", f.type}, {"count", f.count}});
            }
            arr.push_back({{"name", s.name}, {"fields", fields}});
        }
        res.set_content(json{{"ok", true}, {"structs", arr}}.dump(), "application/json");
        overlay::LogApiCall("GET /struct/list");
    });

    svr.Post("/struct/read", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            std::string name = body.at("name").get<std::string>();
            uintptr_t address = ParseAddress(body.at("address"));

            json fields, errors;
            bool ok = structs::Read(name, address, fields, errors);
            json out;
            out["ok"] = ok;
            if (ok) { out["fields"] = fields; out["errors"] = errors; }
            else out["error"] = "unknown_struct";
            res.set_content(out.dump(), "application/json");
            overlay::LogApiCall("POST /struct/read " + name);
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Post("/struct/write", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto mutation = action::LockMutations();
            json body = json::parse(req.body);
            std::string name = body.at("name").get<std::string>();
            uintptr_t address = ParseAddress(body.at("address"));
            json values = body.at("values");

            json originalFields, readErrors;
            structs::Read(name, address, originalFields, readErrors);
            json originalSubset = json::object();
            for (auto it = values.begin(); it != values.end(); ++it)
                if (originalFields.contains(it.key())) originalSubset[it.key()] = originalFields.at(it.key());

            json errors;
            bool ok = structs::Write(name, address, values, errors);
            if (ok && !originalSubset.empty()) {
                action::Record("struct/write " + name, [name, address, originalSubset] {
                    json undoErrors;
                    return structs::Write(name, address, originalSubset, undoErrors) && undoErrors.empty();
                });
            }
            json out;
            out["ok"] = ok;
            if (ok) out["errors"] = errors;
            else out["error"] = "unknown_struct";
            res.set_content(out.dump(), "application/json");
            overlay::LogApiCall("POST /struct/write " + name);
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Post("/struct/infer", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            std::vector<uintptr_t> instances;
            for (const auto& address : body.at("instances")) instances.push_back(ParseAddress(address));
            std::vector<structinfer::FieldGuess> guesses;
            std::string error;
            if (!structinfer::Infer(instances, body.at("size").get<size_t>(), guesses, error)) {
                res.status = 400; res.set_content(json{{"ok",false},{"error",error}}.dump(), "application/json"); return;
            }
            json fields = json::array();
            std::vector<structs::Field> definition;
            for (const auto& guess : guesses) {
                fields.push_back({{"name",guess.name},{"offset",guess.offset},{"size",guess.size},{"type",guess.type},
                    {"confidence",guess.confidence},{"constant",guess.constant},{"distinct_values",guess.distinctValues},
                    {"reasons",guess.reasons},{"values",guess.values}});
                std::string storedType = guess.type;
                definition.push_back({guess.name, static_cast<int64_t>(guess.offset), storedType, 0});
            }
            bool defined = false;
            if (body.value("define", false)) {
                const std::string name = body.at("name").get<std::string>();
                defined = structs::Define(name, definition);
            }
            res.set_content(json{{"ok",true},{"fields",fields},{"defined",defined}}.dump(), "application/json");
            overlay::LogApiCall("POST /struct/infer (" + std::to_string(instances.size()) + " instances)");
        } catch (const std::exception& e) {
            res.status = 400; res.set_content(json{{"ok",false},{"error",e.what()}}.dump(), "application/json");
        }
    });
}

} // namespace api
