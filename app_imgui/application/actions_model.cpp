#include "actions_model.h"

#include <nlohmann/json.hpp>

#include <utility>

namespace cortex::application {
namespace {

using json = nlohmann::json;

json RouteResult(const json& output) {
    if (!output.is_object()) return output;
    const auto found = output.find("result");
    return found != output.end() ? *found : output;
}

} // namespace

void ActionsModel::Reset() {
    actions_.clear();
    checkpoint_ = 0;
}

bool ActionsModel::EnsureRuntime(bool allowInjection, bool mutationAllowed,
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

bool ActionsModel::Call(const std::string& tool, json arguments,
                        json& result, bool mutation, bool mutationAllowed,
                        std::string* error) {
    result = json::object();
    if (!EnsureRuntime(mutation, mutationAllowed, error)) return false;
    if (mutation) arguments["mutation_permission"] = true;

    json output;
    if (!payload_.CallTool(tool, arguments, output, error)) return false;
    result = RouteResult(output);
    return true;
}

bool ActionsModel::Refresh(std::string* error) {
    json result;
    if (!Call("actions_list", {{"_query", {{"offset", 0}, {"limit", 512}}}},
              result, false, false, error)) return false;

    actions_.clear();
    const json rows = result.value("actions", json::array());
    if (rows.is_array()) {
        for (const auto& entry : rows) {
            if (!entry.is_object()) continue;
            ActionEntry row;
            row.id = entry.value("id", uint64_t{0});
            row.timestampMs = entry.value("timestamp_ms", uint64_t{0});
            row.description = entry.value("description", std::string());
            actions_.push_back(std::move(row));
        }
    }
    checkpoint_ = result.value("checkpoint", uint64_t{0});
    return true;
}

bool ActionsModel::RollbackAll(bool mutationAllowed, std::string* error) {
    json result;
    if (!Call("actions_rollback", json::object(), result,
              true, mutationAllowed, error)) return false;
    return Refresh(error);
}

bool ActionsModel::RollbackTo(uint64_t checkpoint, bool mutationAllowed,
                              std::string* error) {
    json result;
    if (!Call("actions_rollback", {{"checkpoint", checkpoint}}, result,
              true, mutationAllowed, error)) return false;
    return Refresh(error);
}

bool ActionsModel::Clear(bool mutationAllowed, std::string* error) {
    json result;
    if (!Call("actions_clear", json::object(), result,
              true, mutationAllowed, error)) return false;
    return Refresh(error);
}

} // namespace cortex::application
