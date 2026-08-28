#pragma once

#include "target/module_provider.h"
#include "target/session_manager.h"

#include <string>
#include <vector>

namespace cortex::services {

class ModuleService {
public:
    explicit ModuleService(target::SessionManager& sessions) : sessions_(sessions) {}

    std::vector<target::ModuleInfo> List(std::string* error = nullptr) const;

private:
    target::SessionManager& sessions_;
};

} // namespace cortex::services
