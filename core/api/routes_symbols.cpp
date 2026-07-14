#include "routes.h"
#include "../symbols/symbols.h"
#include "../overlay/overlay.h"

#include <nlohmann/json.hpp>
#include <sstream>

using json = nlohmann::json;

namespace api {

namespace {

std::string HexAddr(uintptr_t a) {
    std::ostringstream s;
    s << "0x" << std::hex << a;
    return s.str();
}

} // namespace

void RegisterSymbolsRoutes(httplib::Server& svr) {
    svr.Get("/symbols/resolve", [](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_param("address")) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", "missing_address"}}.dump(), "application/json");
            return;
        }
        try {
            uintptr_t address = static_cast<uintptr_t>(std::stoull(req.get_param_value("address"), nullptr, 0));

            auto sym = symbols::Resolve(address);
            auto line = symbols::ResolveLine(address);

            json out;
            out["ok"] = sym.has_value();
            if (sym) {
                out["symbol"] = sym->name;
                out["symbol_address"] = HexAddr(sym->symbolAddress);
                out["displacement"] = sym->displacement;
            } else {
                out["error"] = "no_symbol_available";
            }
            if (line) {
                out["file"] = line->file;
                out["line"] = line->line;
            }
            res.set_content(out.dump(), "application/json");
            overlay::LogApiCall("GET /symbols/resolve @ " + req.get_param_value("address"));
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Get("/symbols/lookup", [](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_param("name")) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", "missing_name"}}.dump(), "application/json");
            return;
        }
        std::string name = req.get_param_value("name");
        auto addr = symbols::Lookup(name);

        json out;
        out["ok"] = addr.has_value();
        if (addr) out["address"] = HexAddr(*addr); else out["error"] = "symbol_not_found";
        res.set_content(out.dump(), "application/json");
        overlay::LogApiCall("GET /symbols/lookup " + name);
    });
}

} // namespace api
