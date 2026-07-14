#include "routes.h"
#include "../call/call.h"
#include "../overlay/overlay.h"
#include "../action/action.h"

#include <nlohmann/json.hpp>
#include <sstream>

using json = nlohmann::json;

namespace api {

namespace {

uintptr_t ParseAddress(const json& jaddr) {
    if (jaddr.is_string()) {
        return static_cast<uintptr_t>(std::stoull(jaddr.get<std::string>(), nullptr, 0));
    }
    return static_cast<uintptr_t>(jaddr.get<uint64_t>());
}

std::string HexAddr(uintptr_t a) {
    std::ostringstream s;
    s << "0x" << std::hex << a;
    return s.str();
}

} // namespace

void RegisterCallRoutes(httplib::Server& svr) {
    svr.Post("/call/function", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto mutation = action::LockMutations();
            json body = json::parse(req.body);
            uintptr_t address = ParseAddress(body.at("address"));

            std::vector<uintptr_t> args;
            if (body.contains("args")) {
                for (const auto& a : body.at("args")) args.push_back(ParseAddress(a));
            }

            std::string convStr = body.value("convention", std::string("cdecl"));
            remotecall::Convention conv;
            if (convStr == "cdecl") conv = remotecall::Convention::Cdecl;
            else if (convStr == "stdcall") conv = remotecall::Convention::Stdcall;
            else if (convStr == "thiscall") conv = remotecall::Convention::Thiscall;
            else {
                res.status = 400;
                res.set_content(json{{"ok", false}, {"error", "invalid_convention"}}.dump(), "application/json");
                return;
            }

            remotecall::CallResult result = remotecall::Invoke(address, args, conv);
            json out;
            out["ok"] = result.ok;
            if (result.ok) out["return_value"] = HexAddr(static_cast<uintptr_t>(result.returnValue));
            else out["error"] = result.error;
            res.set_content(out.dump(), "application/json");
            overlay::LogApiCall("POST /call/function @ " + HexAddr(address));
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });
}

} // namespace api
