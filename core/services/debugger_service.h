#pragma once

#include "target/session_manager.h"
#include "target/thread_provider.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cortex::services {

class DebuggerService {
public:
    explicit DebuggerService(target::SessionManager& sessions) : sessions_(sessions) {}

    std::vector<uint64_t> Threads(std::string* error = nullptr) const;
    bool Registers(uint64_t threadId,
                   target::ThreadRegisterSnapshot& snapshot,
                   std::string* error = nullptr) const;

private:
    target::SessionManager& sessions_;
};

} // namespace cortex::services
