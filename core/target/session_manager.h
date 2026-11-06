#pragma once

#include "catalog.h"
#include "session.h"

#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace cortex::target {

// The attached sessions of one client (desktop, one MCP target runtime, ...).
// Every member is safe to call from several threads: the desktop frame loop,
// its background workers and the debugger backend all reach the same
// instance. Accessors return values or shared pointers, never references into
// the manager, so a concurrent Detach cannot leave a caller holding freed
// state.
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
    std::string ActiveTargetId() const;
    std::optional<TargetDescriptor> ActiveTarget() const;
    std::optional<CapabilitySet> ActiveCapabilities() const;

private:
    SessionPtr FindLocked(const std::string& targetId) const;
    bool DetachLocked(const std::string& targetId);

    Catalog& catalog_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, SessionPtr> sessions_;
    std::string activeTargetId_;
};

} // namespace cortex::target
