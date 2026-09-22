#include "diagnostics_model.h"

#include <nlohmann/json.hpp>

namespace cortex::application {
namespace {
using json = nlohmann::json;

json RouteResult(const json& output) {
    if (!output.is_object()) return output;
    const auto found = output.find("result");
    return found != output.end() ? *found : output;
}
} // namespace

void DiagnosticsModel::Reset() {
    summary_.clear();
    statusJson_.clear();
    healthJson_.clear();
    hooks_.clear();
    toolStats_ = {};
}

bool DiagnosticsModel::EnsureRuntime(std::string* error) {
    if (error) error->clear();
    if (payload_.Ready()) return true;
    return payload_.TryConnectExisting(error);
}

bool DiagnosticsModel::Call(const std::string& tool, json arguments,
                            json& result, std::string* error) {
    result = json::object();
    if (!EnsureRuntime(error)) return false;
    json output;
    if (!payload_.CallTool(tool, arguments, output, error)) return false;
    result = RouteResult(output);
    return true;
}

bool DiagnosticsModel::Refresh(std::string* error) {
    json status;
    if (!Call("status", json::object(), status, error)) return false;
    json health;
    if (!Call("health", json::object(), health, error)) return false;
    json tools;
    if (!Call("tools", json::object(), tools, error)) return false;

    if (!status.is_object() || !health.is_object() || !tools.is_array()) {
        if (error) *error = "diagnostics_payload_invalid";
        return false;
    }

    statusJson_ = status.dump(2);
    healthJson_ = health.dump(2);

    hooks_.clear();
    const json hooks = health.value("hooks", json::object());
    if (hooks.is_object()) {
        for (auto it = hooks.begin(); it != hooks.end(); ++it) {
            if (!it.value().is_object()) continue;
            DiagnosticHook row;
            row.name = it.key();
            row.installed = it.value().value("installed", false);
            row.backend = it.value().value("backend", std::string());
            hooks_.push_back(std::move(row));
        }
    }

    toolStats_ = {};
    toolStats_.total = static_cast<int>(tools.size());
    for (const auto& entry : tools) {
        if (!entry.is_object()) continue;
        const std::string method = entry.value("method", std::string());
        if (method == "GET") ++toolStats_.get;
        else if (method == "POST") ++toolStats_.post;
        else if (method == "DELETE") ++toolStats_.remove;
        if (entry.value("public", false)) ++toolStats_.publicCount;
    }

    const json process = health.value("process", json::object());
    const int bitness = process.is_object() ? process.value("bitness", 0) : 0;
    summary_ = std::string("Runtime ") +
               (health.value("ok", false) ? "healthy" : "degraded") +
               " | " + std::to_string(bitness) + "-bit | " +
               std::to_string(toolStats_.total) + " tools | " +
               std::to_string(hooks_.size()) + " hook(s)";
    return true;
}

} // namespace cortex::application
