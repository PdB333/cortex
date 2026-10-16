#include "runtime_model_base.h"

namespace cortex::application {

using json = nlohmann::json;

json RuntimeModelBase::RouteResult(const json& output) {
    if (!output.is_object()) return output;
    const auto found = output.find("result");
    return found != output.end() ? *found : output;
}

bool RuntimeModelBase::EnsureRuntime(bool allowInjection, bool mutationAllowed,
                                     std::string* error) {
    if (error) error->clear();
    if (payload_.Ready()) return true;

    std::string connectError;
    if (payload_.TryConnectExisting(&connectError)) return true;

    if (!allowInjection) {
        if (error) *error = connectError.empty() ? "runtime_not_connected" : connectError;
        return false;
    }
    if (!mutationAllowed) {
        if (error) *error = "mutation_permission_required";
        return false;
    }
    return payload_.EnsureReady(error);
}

bool RuntimeModelBase::Call(const std::string& tool, json arguments, json& result,
                            bool mutation, bool mutationAllowed, std::string* error) {
    result = json::object();
    // The write gate applies to every mutating call, not only to loading the
    // runtime: an already-connected runtime must not receive
    // mutation_permission unless the user allowed writes.
    if (mutation && !mutationAllowed) {
        if (error) *error = "mutation_permission_required";
        return false;
    }
    // The runtime enforces the same switch on its side. The person's Writes
    // allowed state reaches the payload once per frame (desktop_ui.cpp), and
    // that is the only place that lowers it: a read-only call passes
    // mutationAllowed = false whatever the switch says, so it must not touch the
    // shared authority, or it would revoke it for the debugger's direct calls
    // made later in the same frame. A mutating call got here with the switch on,
    // so raising is consistent; `mutation_permission` below only declares intent.
    if (mutation) payload_.SetWritesAuthority(true);
    if (!EnsureRuntime(mutation, mutationAllowed, error)) return false;
    if (mutation) arguments["mutation_permission"] = true;

    json output;
    if (!payload_.CallTool(tool, arguments, output, error)) return false;
    result = RouteResult(output);
    return true;
}

}  // namespace cortex::application
