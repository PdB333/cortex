#include "routes.h"
#include "../ghidra/ghidra.h"
#include "../overlay/overlay.h"

#include <nlohmann/json.hpp>

using json=nlohmann::json;

namespace api {

void RegisterGhidraRoutes(httplib::Server& svr) {
    svr.Post("/ghidra/export", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body=json::parse(req.body); std::string output,script,error;
            const bool ok=ghidra::ExportRuntime(body.value("name",std::string()),output,script,error);
            res.status=ok?200:500; res.set_content(ok?json{{"ok",true},{"json_path",output},{"script_path",script}}.dump()
                :json{{"ok",false},{"error",error}}.dump(),"application/json");
            overlay::LogApiCall("POST /ghidra/export");
        } catch(const std::exception& e){res.status=400;res.set_content(json{{"ok",false},{"error",e.what()}}.dump(),"application/json");}
    });
    svr.Post("/ghidra/import", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body=json::parse(req.body); size_t imported=0; std::string error;
            const bool ok=ghidra::ImportAnnotations(body,imported,error);
            res.status=ok?200:400; res.set_content(ok?json{{"ok",true},{"imported",imported}}.dump()
                :json{{"ok",false},{"error",error}}.dump(),"application/json");
        } catch(const std::exception& e){res.status=400;res.set_content(json{{"ok",false},{"error",e.what()}}.dump(),"application/json");}
    });
}

} // namespace api
