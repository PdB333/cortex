#include "network_model.h"

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

void NetworkModel::Reset() {
    captureEnabled_ = false;
    events_.clear();
}

bool NetworkModel::EnsureRuntime(bool allowInjection, bool mutationAllowed,
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

bool NetworkModel::Call(const std::string& tool, json arguments,
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

bool NetworkModel::Refresh(std::string* error) {
    json result;
    if (!Call("network_events", {{"_query", {{"limit", 500}}}},
              result, false, false, error)) return false;

    captureEnabled_ = result.value("enabled", false);
    events_.clear();
    const json rows = result.value("events", json::array());
    if (rows.is_array()) {
        for (const auto& event : rows) {
            if (!event.is_object()) continue;
            NetworkEvent row;
            row.id = event.value("id", uint64_t{0});
            row.tickMs = event.value("tick_ms", uint64_t{0});
            row.direction = event.value("dir", std::string());
            row.socket = event.value("socket", uint64_t{0});
            row.size = event.value("size", uint64_t{0});
            row.previewHex = event.value("preview_hex", std::string());
            events_.push_back(std::move(row));
        }
    }
    return true;
}

bool NetworkModel::SetCapture(bool enabled, bool mutationAllowed,
                              std::string* error) {
    json result;
    if (!Call("network_capture", {{"enabled", enabled}},
              result, true, mutationAllowed, error)) return false;
    captureEnabled_ = result.value("enabled", enabled);
    return Refresh(error);
}

} // namespace cortex::application
