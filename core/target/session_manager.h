#pragma once

#include "catalog.h"
#include "session.h"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace cortex::target {

class SessionManager {
public:
    explicit SessionManager(Catalog& catalog) : catalog_(catalog) {}

    bool Attach(const TargetDescriptor& target, std::string* error = nullptr);
    bool Activate(const std::string& targetId);
    bool Detach(const std::string& targetId);
    void Detach();
    void DetachAll();
    void PruneDeadSessions();

    bool HasActiveSession() const { return static_cast<bool>(Active()); }
    bool HasSession(const std::string& targetId) const;
    std::size_t SessionCount() const;
    SessionPtr Active() const;
    SessionPtr Find(const std::string& targetId) const;
    std::vector<TargetDescriptor> AttachedTargets() const;
    const std::string& ActiveTargetId() const { return activeTargetId_; }
    const TargetDescriptor* ActiveTarget() const;
    const CapabilitySet* ActiveCapabilities() const;

private:
    Catalog& catalog_;
    std::unordered_map<std::string, SessionPtr> sessions_;
    std::string activeTargetId_;
};

} // namespace cortex::target