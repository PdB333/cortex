#include "snapshots_model.h"

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

std::string Trim(std::string value) {
    auto nonSpace = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), nonSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), nonSpace).base(), value.end());
    return value;
}

} // namespace

void SnapshotsModel::Reset() {
    snapshots_.clear();
    resultJson_.clear();
}

bool SnapshotsModel::EnsureRuntime(bool allowInjection, bool mutationAllowed, std::string* error) {
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

bool SnapshotsModel::Call(const std::string& tool, json arguments,
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

bool SnapshotsModel::Refresh(std::string* error) {
    json result;
    if (!Call("snapshot_list", json::object(), result, false, false, error))
        return false;

    snapshots_.clear();
    const json rows = result.value("snapshots", json::array());
    if (rows.is_array()) {
        for (const auto& entry : rows) {
            if (!entry.is_object()) continue;
            SnapshotInfo row;
            row.id = entry.value("id", -1);
            row.timestampMs = entry.value("timestamp_ms", uint64_t{0});
            row.label = entry.value("label", std::string());
            row.rangeCount = entry.value("range_count", uint64_t{0});
            row.totalBytes = entry.value("total_bytes", uint64_t{0});
            snapshots_.push_back(std::move(row));
        }
    }
    std::sort(snapshots_.begin(), snapshots_.end(),
              [](const auto& a, const auto& b) { return a.id > b.id; });
    return true;
}

bool SnapshotsModel::Create(const std::string& rangesJson, const std::string& rawLabel,
                            std::string* error) {
    json ranges;
    try {
        ranges = json::parse(Trim(rangesJson).empty() ? "[]" : rangesJson);
    } catch (...) {
        if (error) *error = "snapshot_ranges_invalid_json";
        return false;
    }
    if (!ranges.is_array() || ranges.empty()) {
        if (error) *error = "snapshot_ranges_required";
        return false;
    }

    json arguments{{"ranges", std::move(ranges)}};
    const std::string label = Trim(rawLabel);
    if (!label.empty()) arguments["label"] = label;

    json result;
    if (!Call("snapshot_create", std::move(arguments), result,
              false, false, error)) return false;

    resultJson_ = result.dump(2);
    return Refresh(error);
}

bool SnapshotsModel::Diff(int fromId, int toId, std::string* error) {
    if (fromId < 0 || toId < 0 || fromId == toId) {
        if (error) *error = "snapshot_diff_requires_two_ids";
        return false;
    }

    json result;
    if (!Call("snapshot_diff", {{"from", fromId}, {"to", toId}},
              result, false, false, error)) return false;
    resultJson_ = result.dump(2);
    return true;
}

bool SnapshotsModel::Rewind(int id, bool mutationAllowed, std::string* error) {
    if (id < 0) {
        if (error) *error = "invalid_snapshot_id";
        return false;
    }

    json result;
    if (!Call("snapshot_rewind", {{"_path", {{"id", id}}}},
              result, true, mutationAllowed, error)) return false;
    resultJson_ = result.dump(2);
    return Refresh(error);
}

bool SnapshotsModel::Delete(int id, bool mutationAllowed, std::string* error) {
    if (id < 0) {
        if (error) *error = "invalid_snapshot_id";
        return false;
    }

    json result;
    if (!Call("snapshot_delete", {{"_path", {{"id", id}}}},
              result, true, mutationAllowed, error)) return false;
    resultJson_ = result.dump(2);
    return Refresh(error);
}

bool SnapshotsModel::LastChange(const std::string& rawAddress, int size,
                                std::string* error) {
    const std::string address = Trim(rawAddress);
    if (address.empty() || size <= 0 || size > 4096) {
        if (error) *error = "invalid_snapshot_last_change_request";
        return false;
    }

    json result;
    if (!Call("snapshot_last_change",
              {{"address", address}, {"size", size}},
              result, false, false, error)) return false;
    resultJson_ = result.dump(2);
    return true;
}

} // namespace cortex::application
