#include "pointer_maps_model.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <utility>

namespace cortex::application {
namespace {

using json = nlohmann::json;

json RouteResult(const json& output) {
    if (!output.is_object()) return output;
    const auto found = output.find("result");
    return found != output.end() ? *found : output;
}

bool StableName(const std::string& value) {
    return !value.empty() &&
           std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return std::isalnum(ch) || ch == '_' || ch == '-';
           });
}

} // namespace

void PointerMapsModel::Reset() {
    maps_.clear();
    paths_.clear();
}

bool PointerMapsModel::EnsureRuntime(bool allowInjection, bool mutationAllowed, std::string* error) {
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

bool PointerMapsModel::Call(const std::string& tool, json arguments,
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

bool PointerMapsModel::Refresh(std::string* error) {
    json result;
    if (!Call("pointermap_list", json::object(), result, false, false, error))
        return false;

    maps_.clear();
    const json rows = result.value("pointermaps", json::array());
    if (rows.is_array()) {
        for (const auto& entry : rows) {
            if (!entry.is_object()) continue;
            PointerMapInfo row;
            row.name = entry.value("name", std::string());
            row.target = entry.value("target", std::string());
            row.createdMs = entry.value("created_ms", uint64_t{0});
            row.pathCount = entry.value("path_count", uint64_t{0});
            row.truncated = entry.value("truncated", false);
            maps_.push_back(std::move(row));
        }
    }
    std::sort(maps_.begin(), maps_.end(),
              [](const auto& a, const auto& b) { return a.name < b.name; });
    return true;
}

bool PointerMapsModel::Capture(const std::string& name, const std::string& target,
                               int maxDepth, int maxOffset, bool mutationAllowed,
                               std::string* error) {
    if (!StableName(name) || target.empty() || maxDepth <= 0 || maxOffset <= 0) {
        if (error) *error = "invalid_pointermap_configuration";
        return false;
    }

    json result;
    if (!Call("pointermap_capture",
              {{"name", name}, {"target", target},
               {"max_depth", maxDepth}, {"max_offset", maxOffset}},
              result, true, mutationAllowed, error)) return false;

    paths_.clear();
    return Refresh(error);
}

bool PointerMapsModel::Intersect(const std::vector<std::string>& names,
                                 std::string* error) {
    if (names.size() < 2) {
        if (error) *error = "pointermap_intersection_requires_two_maps";
        return false;
    }

    json result;
    if (!Call("pointermap_intersect", {{"names", names}}, result,
              false, false, error)) return false;

    paths_.clear();
    const json rows = result.value("paths", json::array());
    if (rows.is_array()) {
        for (const auto& entry : rows) {
            if (!entry.is_object()) continue;
            PointerPathCandidate row;
            row.module = entry.value("module", std::string());
            row.baseOffset = entry.value("base_offset", int64_t{0});
            if (entry.contains("offsets")) row.offsetsJson = entry.at("offsets").dump();
            row.sessions = entry.value("sessions", 0);
            row.score = entry.value("score", 0.0);
            paths_.push_back(std::move(row));
        }
    }
    std::sort(paths_.begin(), paths_.end(),
              [](const auto& a, const auto& b) { return a.score > b.score; });
    return true;
}

bool PointerMapsModel::Delete(const std::string& name, bool mutationAllowed,
                              std::string* error) {
    if (name.empty()) {
        if (error) *error = "pointermap_name_required";
        return false;
    }

    json result;
    if (!Call("pointermap_delete", {{"_path", {{"name", name}}}}, result,
              true, mutationAllowed, error)) return false;

    paths_.clear();
    return Refresh(error);
}

} // namespace cortex::application
