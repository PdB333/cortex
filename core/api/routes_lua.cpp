// Embedded Lua scripting routes.
//
// One-shot execution via POST /lua/exec, and a tiny file-backed catalog
// (list, get, save, run, delete) under <module_dir>/cortex_scripts/. Scripts
// share the same address grammar and cortex.* bindings the REST routes use,
// so a snippet developed interactively is a drop-in replacement for a chain
// of HTTP calls.

#include "routes.h"
#include "server.h"
#include "../scripting/lua_engine.h"
#include "../overlay/overlay.h"

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <windows.h>
#include <fstream>
#include <sstream>
#include <cctype>

using json = nlohmann::json;

namespace api {

namespace {

// Only allow a-z, A-Z, 0-9, underscore and hyphen -- keeps user-supplied
// names from escaping the scripts directory via traversal.
std::string SanitizeName(const std::string& n) {
    std::string s;
    for (char c : n) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') s.push_back(c);
    }
    return s;
}

json ExecResultToJson(const scripting::ExecResult& r) {
    return json{
        {"ok", r.ok},
        {"result", r.result},
        {"output", r.output},
        {"error", r.error}
    };
}

} // namespace

void RegisterLuaRoutes(httplib::Server& svr) {
    svr.Post("/lua/exec", [](const httplib::Request& req, httplib::Response& res) {
        json body;
        try { body = json::parse(req.body.empty() ? "{}" : req.body); }
        catch (...) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":\"invalid_json\"}", "application/json");
            return;
        }
        std::string code = body.value("code", std::string(""));
        int timeout = body.value("timeout_ms", 5000);
        auto r = scripting::Exec(code, timeout);
        res.set_content(ExecResultToJson(r).dump(), "application/json");
        overlay::LogApiCall("POST /lua/exec");
    });

    svr.Get("/lua/scripts", [](const httplib::Request&, httplib::Response& res) {
        json arr = json::array();
        const std::string dir = scripting::GetScriptsDir();
        WIN32_FIND_DATAA fd{};
        HANDLE h = FindFirstFileA((dir + "\\*.lua").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                std::string name = fd.cFileName;
                if (name.size() > 4 && name.substr(name.size() - 4) == ".lua") {
                    arr.push_back(name.substr(0, name.size() - 4));
                }
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        res.set_content(json{{"scripts", arr}}.dump(), "application/json");
        overlay::LogApiCall("GET /lua/scripts");
    });

    svr.Post("/lua/scripts", [](const httplib::Request& req, httplib::Response& res) {
        json body;
        try { body = json::parse(req.body); }
        catch (...) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":\"invalid_json\"}", "application/json");
            return;
        }
        const std::string rawName = body.value("name", std::string(""));
        const std::string name = SanitizeName(rawName);
        const std::string code = body.value("code", std::string(""));
        if (name.empty() || name != rawName) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":\"invalid_name\"}", "application/json");
            return;
        }
        const std::string path = scripting::GetScriptsDir() + "\\" + name + ".lua";
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) {
            res.status = 500;
            res.set_content(json{{"ok", false}, {"error", "write_failed"}}.dump(), "application/json");
            return;
        }
        f.write(code.data(), static_cast<std::streamsize>(code.size()));
        f.flush();
        if (!f.good()) {
            res.status = 500;
            res.set_content(json{{"ok", false}, {"error", "write_failed"}}.dump(), "application/json");
            return;
        }
        res.set_content(json{{"ok", true}, {"name", name}, {"bytes", code.size()}}.dump(),
                        "application/json");
        overlay::LogApiCall("POST /lua/scripts");
    });

    svr.Get(R"(/lua/scripts/([A-Za-z0-9_\-]+))",
            [](const httplib::Request& req, httplib::Response& res) {
        const std::string name = req.matches[1];
        const std::string path = scripting::GetScriptsDir() + "\\" + name + ".lua";
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            res.status = 404;
            res.set_content("{\"ok\":false,\"error\":\"not_found\"}", "application/json");
            return;
        }
        std::stringstream buf; buf << f.rdbuf();
        res.set_content(json{{"name", name}, {"code", buf.str()}}.dump(), "application/json");
        overlay::LogApiCall("GET /lua/scripts/" + name);
    });

    svr.Post(R"(/lua/scripts/([A-Za-z0-9_\-]+)/run)",
             [](const httplib::Request& req, httplib::Response& res) {
        const std::string name = req.matches[1];
        const std::string path = scripting::GetScriptsDir() + "\\" + name + ".lua";
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            res.status = 404;
            res.set_content("{\"ok\":false,\"error\":\"not_found\"}", "application/json");
            return;
        }
        std::stringstream buf; buf << f.rdbuf();
        int timeout = 5000;
        try {
            auto b = json::parse(req.body.empty() ? "{}" : req.body);
            timeout = b.value("timeout_ms", 5000);
        } catch (...) {}
        auto r = scripting::Exec(buf.str(), timeout);
        res.set_content(ExecResultToJson(r).dump(), "application/json");
        overlay::LogApiCall("POST /lua/scripts/" + name + "/run");
    });

    svr.Delete(R"(/lua/scripts/([A-Za-z0-9_\-]+))",
               [](const httplib::Request& req, httplib::Response& res) {
        const std::string name = req.matches[1];
        const std::string path = scripting::GetScriptsDir() + "\\" + name + ".lua";
        BOOL ok = DeleteFileA(path.c_str());
        if (!ok) {
            const DWORD error = ::GetLastError();
            res.status = error == ERROR_FILE_NOT_FOUND ? 404 : 500;
            res.set_content(json{{"ok", false}, {"error", error == ERROR_FILE_NOT_FOUND ? "not_found" : "delete_failed"}}.dump(),
                            "application/json");
            return;
        }
        res.set_content(json{{"ok", true}}.dump(), "application/json");
        overlay::LogApiCall("DELETE /lua/scripts/" + name);
    });
}

} // namespace api
