#pragma once

#include "catalog.h"
#include "session.h"

#include <string>

namespace cortex::target {

class SessionManager {
public:
    explicit SessionManager(Catalog& catalog) : catalog_(catalog) {}

    bool Attach(const TargetDescriptor& target, std::string* error = nullptr);
    void Detach();

    bool HasActiveSession() const { return static_cast<bool>(active_); }
    SessionPtr Active() const { return active_; }
    const TargetDescriptor* ActiveTarget() const { return active_ ? &active_->Target() : nullptr; }
    const CapabilitySet* ActiveCapabilities() const { return active_ ? &active_->Capabilities() : nullptr; }

private:
    Catalog& catalog_;
    SessionPtr active_;
};

} // namespace cortex::target
