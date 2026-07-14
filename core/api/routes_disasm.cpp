#include "routes.h"
#include "../disasm/disasm.h"
#include "../overlay/overlay.h"

#include <nlohmann/json.hpp>
#include <sstream>

using json = nlohmann::json;

namespace api {

void RegisterDisasmRoutes(httplib::Server& svr) {
    svr.Get("/disasm", [](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_param("address")) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", "missing_address"}}.dump(), "application/json");
            return;
        }

        try {
            uintptr_t address = static_cast<uintptr_t>(
                std::stoull(req.get_param_value("address"), nullptr, 0));
            int count = req.has_param("count") ? std::stoi(req.get_param_value("count")) : 20;

            bool ok = false;
            auto instructions = disasm::Disassemble(address, count, ok);

            if (!ok) {
                res.status = 400;
                res.set_content(json{{"ok", false}, {"error", "address_not_readable"}}.dump(), "application/json");
                return;
            }

            json arr = json::array();
            for (const auto& ins : instructions) {
                std::ostringstream addr;
                addr << "0x" << std::hex << ins.address;
                arr.push_back({{"address", addr.str()}, {"size", ins.size},
                                {"bytes", ins.bytes_hex}, {"mnemonic", ins.mnemonic}, {"text", ins.text}});
            }
            res.set_content(json{{"ok", true}, {"instructions", arr}}.dump(), "application/json");
            overlay::LogApiCall("GET /disasm @ " + req.get_param_value("address"));
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });
}

} // namespace api
