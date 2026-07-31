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

json IdentityJson(const symbols::ModuleIdentity& identity) {
    return json{
        {"valid_pe", identity.validPe},
        {"build_id", identity.buildId},
        {"pe_timestamp", identity.timeDateStamp},
        {"image_size", identity.imageSize},
        {"preferred_image_base", HexAddr(static_cast<uintptr_t>(identity.preferredImageBase))},
        {"has_codeview", identity.hasCodeView},
        {"pdb_guid", identity.pdbGuid},
        {"pdb_age", identity.pdbAge},
        {"pdb_path_hint", identity.pdbPathHint}
    };
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
            auto location = symbols::ResolveDetailed(address);
            json out;
            out["ok"] = location.has_value() && location->hasSymbol;
            out["address"] = HexAddr(address);
            if (!location) {
                out["error"] = "symbol_session_unavailable";
            } else {
                out["module"] = location->moduleName;
                out["module_path"] = location->modulePath;
                out["module_base"] = HexAddr(location->moduleBase);
                out["rva"] = HexAddr(location->moduleRva);
                out["build_id"] = location->buildId;
                out["has_symbol"] = location->hasSymbol;
                out["symbol"] = location->symbolName;
                out["symbol_address"] = HexAddr(location->symbolAddress);
                out["displacement"] = location->displacement;
                out["has_line"] = location->hasLine;
                out["file"] = location->file;
                out["line"] = location->line;
                out["loaded_pdb"] = location->loadedPdb;
                out["symbol_type"] = location->symbolType;
                out["exact_symbols"] = location->exactSymbols;
                out["verification"] = location->verification;
                if (!location->hasSymbol) out["error"] = "no_symbol_available";
            }
            res.set_content(out.dump(), "application/json");
            overlay::LogApiCall("GET /symbols/resolve @ " + req.get_param_value("address"));
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Get("/symbols/module", [](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_param("address")) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", "missing_address"}}.dump(), "application/json");
            return;
        }
        try {
            uintptr_t address = static_cast<uintptr_t>(std::stoull(req.get_param_value("address"), nullptr, 0));
            auto location = symbols::ResolveDetailed(address);
            if (!location || !location->moduleBase) {
                res.status = 404;
                res.set_content(json{{"ok", false}, {"error", "module_not_found"}}.dump(), "application/json");
                return;
            }
            auto module = symbols::GetModuleInfo(location->moduleBase);
            json out{
                {"ok", module.has_value()},
                {"module", location->moduleName},
                {"module_path", location->modulePath},
                {"module_base", HexAddr(location->moduleBase)},
                {"rva", HexAddr(location->moduleRva)}
            };
            if (module) {
                out["loaded"] = module->loaded;
                out["has_symbols"] = module->hasSymbols;
                out["exact_match"] = module->exactMatch;
                out["loaded_pdb"] = module->loadedPdb;
                out["symbol_type"] = module->symbolType;
                out["verification"] = module->verification;
                out["error_code"] = module->error;
                out["identity"] = IdentityJson(module->identity);
            }
            res.set_content(out.dump(), "application/json");
            overlay::LogApiCall("GET /symbols/module @ " + req.get_param_value("address"));
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
