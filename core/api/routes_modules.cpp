#include "routes.h"
#include "../process/modules.h"
#include "../overlay/overlay.h"

#include <nlohmann/json.hpp>
#include <sstream>
#include <iomanip>

using json = nlohmann::json;

namespace api {

void RegisterModulesRoutes(httplib::Server& svr) {
    svr.Get("/modules", [](const httplib::Request&, httplib::Response& res) {
        auto mods = process::ListModules();
        json j = json::array();
        for (const auto& m : mods) {
            std::ostringstream base;
            base << "0x" << std::hex << m.base;
            j.push_back({{"name", m.name}, {"base", base.str()}, {"size", m.size}});
        }
        res.set_content(j.dump(), "application/json");
        overlay::LogApiCall("GET /modules (" + std::to_string(mods.size()) + " modules)");
    });
}

} // namespace api
