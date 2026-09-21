#include "debugger_service.h"

namespace cortex::services {

std::vector<uint64_t> DebuggerService::Threads(std::string* error) const {
    if (error) error->clear();
    const auto session = sessions_.Active();
    if (!session || !session->Alive()) {
        if (error) *error = "no_active_session";
        return {};
    }
    return target::ListTargetThreads(session->Target(), error);
}

bool DebuggerService::Registers(uint64_t threadId,
                                target::ThreadRegisterSnapshot& snapshot,
                                std::string* error) const {
    if (error) error->clear();
    const auto session = sessions_.Active();
    if (!session || !session->Alive()) {
        if (error) *error = "no_active_session";
        return false;
    }
    return target::ReadTargetThreadRegisters(session->Target(), threadId, snapshot, error);
}

} // namespace cortex::services
