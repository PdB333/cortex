#include "routes.h"
#include "../hook/net_hook.h"
#include "../overlay/overlay.h"
#include "../process/address.h"

#include <nlohmann/json.hpp>
#include <sstream>

using json = nlohmann::json;

namespace api {
namespace { std::string HexAddr(uintptr_t a){std::ostringstream out;out<<"0x"<<std::hex<<a;return out.str();} }


void RegisterNetRoutes(httplib::Server& svr) {
    svr.Post("/network/capture", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            bool on = body.at("enabled").get<bool>();
            nethook::SetCaptureEnabled(on);
            res.set_content(json{{"ok", true}, {"enabled", on}}.dump(), "application/json");
            overlay::LogApiCall(std::string("POST /network/capture ") + (on ? "on" : "off"));
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Get("/network/events", [](const httplib::Request& req, httplib::Response& res) {
        size_t max = 200;
        auto lim = req.get_param_value("limit");
        if (!lim.empty()) try { max = (size_t)std::stoi(lim); } catch (...) {}
        auto ev = nethook::Snapshot(max);
        json arr = json::array();
        for (const auto& e : ev) {
            json stack=json::array(), stackNamed=json::array();
            for(uintptr_t frame:e.stack){stack.push_back(HexAddr(frame));stackNamed.push_back(process::DescribeAddress(frame));}
            json row{{"id",e.id},{"tick_ms",e.tickMs},{"dir",e.direction==0?"recv":"send"},
                     {"socket",e.socket},{"size",e.size},{"preview_hex",e.previewHex},{"thread_id",e.threadId},
                     {"stack",stack},{"stack_named",stackNamed}};
            if(!e.stack.empty()){
                row["generated_by"]=HexAddr(e.stack[0]);row["generated_by_named"]=process::DescribeAddress(e.stack[0]);
                if(e.stack.size()>1){row["caller"]=HexAddr(e.stack[1]);row["caller_named"]=process::DescribeAddress(e.stack[1]);}
            }
            arr.push_back(std::move(row));
        }
        res.set_content(json{{"ok", true}, {"enabled", nethook::IsCaptureEnabled()},
                             {"events", arr}}.dump(), "application/json");
    });
}

} // namespace api

