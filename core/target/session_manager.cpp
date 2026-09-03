#include "session_manager.h"

namespace cortex::target {

bool SessionManager::Attach(const TargetDescriptor& target, std::string* error) {
    if (error) error->clear();

    if (auto existing = Find(target.id)) {
        const uint64_t existingGeneration = existing->Target().generation;
        const bool sameGeneration = existingGeneration == 0 || target.generation == 0 ||
                                    existingGeneration == target.generation;
        if (sameGeneration) {
            activeTargetId_ = existing->Target().id;
            return true;
        }
        sessions_.erase(target.id);
        if (activeTargetId_ == target.id) activeTargetId_.clear();
    }

    auto session = catalog_.Attach(target, error);
    if (!session) return false;

    const std::string key = session->Target().id.empty() ? target.id : session->Target().id;
    sessions_[key] = std::move(session);
    activeTargetId_ = key;
    return true;
}

bool SessionManager::Activate(const std::string& targetId) {
    auto session = Find(targetId);
    if (!session) return false;
    activeTargetId_ = targetId;
    return true;
}

bool SessionManager::Detach(const std::string& targetId) {
    const auto found = sessions_.find(targetId);
    if (found == sessions_.end()) return false;
    sessions_.erase(found);
    if (activeTargetId_ == targetId) activeTargetId_.clear();
    return true;
}

void SessionManager::Detach() {
    if (activeTargetId_.empty()) return;
    Detach(activeTargetId_);
}

void SessionManager::DetachAll() {
    sessions_.clear();
    activeTargetId_.clear();
}

void SessionManager::PruneDeadSessions() {
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (!it->second || !it->second->Alive()) {
            if (activeTargetId_ == it->first) activeTargetId_.clear();
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }
}

bool SessionManager::HasSession(const std::string& targetId) const { return static_cast<bool>(Find(targetId)); }
std::size_t SessionManager::SessionCount() const {
    std::size_t count = 0;
    for (const auto& [id, session] : sessions_) { (void)id; if (session && session->Alive()) ++count; }
    return count;
}
SessionPtr SessionManager::Active() const { if (activeTargetId_.empty()) return {}; return Find(activeTargetId_); }
SessionPtr SessionManager::Find(const std::string& targetId) const {
    const auto found = sessions_.find(targetId);
    if (found == sessions_.end() || !found->second || !found->second->Alive()) return {};
    return found->second;
}
std::vector<TargetDescriptor> SessionManager::AttachedTargets() const {
    std::vector<TargetDescriptor> result;
    result.reserve(sessions_.size());
    for (const auto& [id, session] : sessions_) { (void)id; if (session && session->Alive()) result.push_back(session->Target()); }
    return result;
}
const TargetDescriptor* SessionManager::ActiveTarget() const { auto session = Active(); return session ? &session->Target() : nullptr; }
const CapabilitySet* SessionManager::ActiveCapabilities() const { auto session = Active(); return session ? &session->Capabilities() : nullptr; }

} // namespace cortex::target
