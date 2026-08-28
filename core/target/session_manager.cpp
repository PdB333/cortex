#include "session_manager.h"

namespace cortex::target {

bool SessionManager::Attach(const TargetDescriptor& target, std::string* error) {
    if (error) error->clear();
    auto session = catalog_.Attach(target, error);
    if (!session) return false;
    active_ = std::move(session);
    return true;
}

void SessionManager::Detach() {
    active_.reset();
}

} // namespace cortex::target
