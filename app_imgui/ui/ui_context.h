#pragma once

#include "services/memory_service.h"
#include "target/session_manager.h"

#include <string>

namespace cortex::ui {

struct UiContext {
    target::SessionManager* sessions = nullptr;
    services::MemoryService* memory = nullptr;

    bool mutationAllowed = false;
    bool requestProcessPicker = false;
    std::string status = "Select a process to begin";
};

} // namespace cortex::ui
