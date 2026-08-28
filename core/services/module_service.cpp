#include "module_service.h"

namespace cortex::services {

std::vector<target::ModuleInfo> ModuleService::List(std::string* error) const {
    if (error) error->clear();
    auto session = sessions_.Active();
    if (!session || !session->Alive()) {
        if (error) *error = "no_active_session";
        return {};
    }
    return target::ListTargetModules(session->Target(), error);
}

} // namespace cortex::services
