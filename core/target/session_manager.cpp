#include "session_manager.h"

namespace cortex::target {

namespace {

// Generation 0 means the backend could not read the process lifetime; it is
// accepted as the same process rather than refusing the attach.
bool SameGeneration(uint64_t existing, uint64_t requested) {
    return existing == 0 || requested == 0 || existing == requested;
}

} // namespace

bool SessionManager::Attach(const TargetDescriptor& target, std::string* error) {
    if (error) error->clear();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto existing = FindLocked(target.id)) {
            if (SameGeneration(existing->Target().generation, target.generation)) {
                activeTargetId_ = existing->Target().id;
                return true;
            }
            DetachLocked(target.id);
        }
    }

    // Opening a process can block; the catalog is not called under the lock.
    auto session = catalog_.Attach(target, error);
    if (!session) return false;

    const std::string key = session->Target().id.empty() ? target.id : session->Target().id;
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_[key] = std::move(session);
    activeTargetId_ = key;
    return true;
}

bool SessionManager::Activate(const std::string& targetId) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!FindLocked(targetId)) return false;
    activeTargetId_ = targetId;
    return true;
}

bool SessionManager::Detach(const std::string& targetId) {
    std::lock_guard<std::mutex> lock(mutex_);
    return DetachLocked(targetId);
}

void SessionManager::Detach() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (activeTargetId_.empty()) return;
    DetachLocked(activeTargetId_);
}

void SessionManager::DetachAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.clear();
    activeTargetId_.clear();
}

void SessionManager::PruneDeadSessions() {
    std::lock_guard<std::mutex> lock(mutex_);
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
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t count = 0;
    for (const auto& [id, session] : sessions_) { (void)id; if (session && session->Alive()) ++count; }
    return count;
}

SessionPtr SessionManager::Active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (activeTargetId_.empty()) return {};
    return FindLocked(activeTargetId_);
}

SessionPtr SessionManager::Find(const std::string& targetId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return FindLocked(targetId);
}

std::vector<TargetDescriptor> SessionManager::AttachedTargets() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<TargetDescriptor> result;
    result.reserve(sessions_.size());
    for (const auto& [id, session] : sessions_) { (void)id; if (session && session->Alive()) result.push_back(session->Target()); }
    return result;
}

std::string SessionManager::ActiveTargetId() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return activeTargetId_;
}

std::optional<TargetDescriptor> SessionManager::ActiveTarget() const {
    auto session = Active();
    if (!session) return std::nullopt;
    return session->Target();
}

std::optional<CapabilitySet> SessionManager::ActiveCapabilities() const {
    auto session = Active();
    if (!session) return std::nullopt;
    return session->Capabilities();
}

SessionPtr SessionManager::FindLocked(const std::string& targetId) const {
    const auto found = sessions_.find(targetId);
    if (found == sessions_.end() || !found->second || !found->second->Alive()) return {};
    return found->second;
}

bool SessionManager::DetachLocked(const std::string& targetId) {
    const auto found = sessions_.find(targetId);
    if (found == sessions_.end()) return false;
    sessions_.erase(found);
    if (activeTargetId_ == targetId) activeTargetId_.clear();
    return true;
}

} // namespace cortex::target
