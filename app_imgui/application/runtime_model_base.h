#pragma once

#include "services/runtime_transport.h"

#include <nlohmann/json.hpp>

#include <string>
#include <utility>

namespace cortex::application {

// Shared runtime access for application models. It owns the one rule every
// model must follow: connect to an existing runtime when possible, and only
// load the runtime into the target (or send a mutating call) when the user
// has allowed writes.
class RuntimeModelBase {
protected:
    explicit RuntimeModelBase(services::RuntimeTransport& payload) : payload_(payload) {}

    // Connects to the runtime. When no runtime is loaded yet it is injected
    // only if `allowInjection` and `mutationAllowed` are both true.
    bool EnsureRuntime(bool allowInjection, bool mutationAllowed, std::string* error);

    // Connect-only variant for models that never load the runtime.
    bool EnsureRuntime(std::string* error) { return EnsureRuntime(false, false, error); }

    // Calls a runtime tool and unwraps its route result into `result`. A
    // mutating call requires `mutationAllowed`, may load the runtime, and is
    // sent with the explicit mutation_permission flag.
    bool Call(const std::string& tool, nlohmann::json arguments,
              nlohmann::json& result, bool mutation, bool mutationAllowed,
              std::string* error);

    // Read-only call on an already-loaded runtime.
    bool Call(const std::string& tool, nlohmann::json arguments,
              nlohmann::json& result, std::string* error) {
        return Call(tool, std::move(arguments), result, false, false, error);
    }

    // Tool calls return {status, result}; this returns the inner result.
    static nlohmann::json RouteResult(const nlohmann::json& output);

    services::RuntimeTransport& payload_;
};

}  // namespace cortex::application
