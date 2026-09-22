#include "re_model.h"

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

std::string DisplayJson(const json& value) {
    if (value.is_string()) return value.get<std::string>();
    return value.dump();
}

json AddressValue(const std::string& raw) {
    const std::string value = Trim(raw);
    try {
        size_t used = 0;
        const unsigned long long parsed = std::stoull(value, &used, 0);
        if (used == value.size()) return static_cast<uint64_t>(parsed);
    } catch (...) {}
    return value;
}

} // namespace

void ReModel::Reset() {
    tracks_.clear();
    checkpoints_.clear();
    sessions_.clear();
    selectedTrackId_ = -1;
    selectedTrackJson_.clear();
    trackEventsJson_.clear();
    sessionJson_.clear();
    resultJson_.clear();
}

bool ReModel::EnsureRuntime(bool allowInjection, bool mutationAllowed, std::string* error) {
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

bool ReModel::Call(const std::string& tool, json arguments,
                   json& result, bool mutation, bool mutationAllowed,
                   std::string* error) {
    result = json::object();
    if (!EnsureRuntime(mutation, mutationAllowed, error)) return false;
    if (mutation) arguments["mutation_permission"] = true;

    json output;
    if (!payload_.CallTool(tool, arguments, output, error)) return false;
    result = RouteResult(output);
    SetResult(result);

    if (result.is_object() && result.contains("ok") && !result.value("ok", true)) {
        if (error) *error = result.value("error", std::string("operation_failed"));
        return false;
    }
    return true;
}

void ReModel::SetResult(const json& value) {
    resultJson_ = value.dump(2);
}

bool ReModel::Refresh(std::string* error) {
    json result;
    if (!Call("re_object_tracks", json::object(), result, false, false, error))
        return false;

    tracks_.clear();
    const json rows = result.value("tracks", json::array());
    if (rows.is_array()) {
        for (const auto& item : rows) {
            if (!item.is_object()) continue;
            ReTrack row;
            row.id = item.value("id", -1);
            row.name = item.value("name", std::string());
            if (item.contains("address")) row.address = DisplayJson(item.at("address"));
            row.size = item.value("size", 0);
            row.alive = item.value("alive", false);
            row.pointerPath = item.value("pointer_path", std::string());
            row.structName = item.value("struct_name", std::string());
            tracks_.push_back(std::move(row));
        }
    }

    json session;
    if (Call("re_session", json::object(), session, false, false, nullptr))
        sessionJson_ = session.dump(2);

    if (selectedTrackId_ >= 0 &&
        std::none_of(tracks_.begin(), tracks_.end(),
                     [&](const ReTrack& row) { return row.id == selectedTrackId_; })) {
        selectedTrackId_ = -1;
        selectedTrackJson_.clear();
        trackEventsJson_.clear();
    }
    return true;
}

bool ReModel::RefreshSessions(std::string* error) {
    json result;
    if (!Call("session_list", json::object(), result, false, false, error))
        return false;

    sessions_.clear();
    const json rows = result.value("sessions", json::array());
    if (rows.is_array()) {
        for (const auto& item : rows) {
            if (!item.is_object()) continue;
            ReSessionSummary row;
            if (item.contains("id")) row.id = DisplayJson(item.at("id"));
            row.rawJson = item.dump();
            sessions_.push_back(std::move(row));
        }
    }
    return true;
}

bool ReModel::RefreshCheckpoints(std::string* error) {
    json result;
    if (!Call("re_checkpoint_list", json::object(), result, false, false, error))
        return false;

    checkpoints_.clear();
    const json rows = result.value("checkpoints", json::array());
    if (rows.is_array()) {
        for (const auto& item : rows) {
            if (!item.is_object()) continue;
            ReCheckpoint row;
            row.id = item.value("id", -1);
            row.label = item.value("label", std::string());
            if (row.label.empty()) row.label = "Checkpoint " + std::to_string(row.id);
            row.rawJson = item.dump();
            checkpoints_.push_back(std::move(row));
        }
    }
    return true;
}

bool ReModel::SelectTrack(int id, std::string* error) {
    if (id < 0) {
        if (error) *error = "invalid_track_id";
        return false;
    }

    json result;
    if (!Call("re_object_get", {{"_path", {{"id", id}}}},
              result, false, false, error)) return false;

    selectedTrackId_ = id;
    selectedTrackJson_ = result.dump(2);

    json events;
    if (Call("re_object_events", {{"_path", {{"id", id}}}},
             events, false, false, nullptr)) {
        const json rows = events.value("events", json::array());
        trackEventsJson_ = rows.dump(2);
    }
    SetResult(result);
    return true;
}

bool ReModel::TrackObject(const std::string& rawName, const std::string& rawAddress,
                          const std::string& rawPointerPath, int size, bool persist,
                          const std::string& rawStructName, bool mutationAllowed,
                          std::string* error) {
    const std::string address = Trim(rawAddress);
    const std::string pointerPath = Trim(rawPointerPath);
    if (address.empty() && pointerPath.empty()) {
        if (error) *error = "track_address_or_pointer_path_required";
        return false;
    }
    size = std::clamp(size, 1, 1024 * 1024);

    std::string name = Trim(rawName);
    if (name.empty()) name = address.empty() ? pointerPath : ("Object " + address);

    json arguments{{"name", name}, {"size", size}, {"persist", persist}};
    if (!pointerPath.empty()) arguments["pointer_path"] = pointerPath;
    else arguments["address"] = AddressValue(address);

    const std::string structName = Trim(rawStructName);
    if (!structName.empty()) arguments["struct_name"] = structName;

    json result;
    if (!Call("re_track_object", std::move(arguments), result,
              true, mutationAllowed, error)) return false;
    SetResult(result);
    return Refresh(error);
}

bool ReModel::DeleteTrack(int id, bool mutationAllowed, std::string* error) {
    if (id < 0) {
        if (error) *error = "invalid_track_id";
        return false;
    }
    json result;
    if (!Call("re_object_delete", {{"_path", {{"id", id}}}},
              result, true, mutationAllowed, error)) return false;

    if (selectedTrackId_ == id) {
        selectedTrackId_ = -1;
        selectedTrackJson_.clear();
        trackEventsJson_.clear();
    }
    SetResult(result);
    return Refresh(error);
}

bool ReModel::FindLastWriter(const std::string& address, int size, int timeoutMs,
                             bool mutationAllowed, std::string* error) {
    if (Trim(address).empty()) {
        if (error) *error = "address_required";
        return false;
    }
    size = std::clamp(size, 1, 4096);
    timeoutMs = std::clamp(timeoutMs, 100, 120000);

    json result;
    return Call("re_find_last_writer",
                {{"address", AddressValue(address)},
                 {"size", size}, {"timeout_ms", timeoutMs}},
                result, true, mutationAllowed, error);
}

bool ReModel::DetectSubobjects(const std::string& address, int size,
                               std::string* error) {
    if (Trim(address).empty()) {
        if (error) *error = "address_required";
        return false;
    }
    size = std::clamp(size, 1, 1024 * 1024);

    json result;
    return Call("re_cpp_subobjects",
                {{"address", AddressValue(address)}, {"size", size}},
                result, false, false, error);
}

bool ReModel::TraceTransition(const std::string& jsonText, bool mutationAllowed,
                              std::string* error) {
    json arguments;
    try {
        arguments = json::parse(jsonText);
    } catch (...) {
        if (error) *error = "transition_json_must_be_object";
        return false;
    }
    if (!arguments.is_object()) {
        if (error) *error = "transition_json_must_be_object";
        return false;
    }

    json result;
    return Call("re_trace_transition", std::move(arguments), result,
                true, mutationAllowed, error);
}

bool ReModel::RunTest(const std::string& jsonText, bool experiment,
                      bool mutationAllowed, std::string* error) {
    json arguments;
    try {
        arguments = json::parse(jsonText);
    } catch (...) {
        if (error) *error = "test_json_must_be_object";
        return false;
    }
    if (!arguments.is_object()) {
        if (error) *error = "test_json_must_be_object";
        return false;
    }

    json result;
    return Call(experiment ? "re_experiment_run" : "re_test_run",
                std::move(arguments), result, true, mutationAllowed, error);
}

bool ReModel::CreateCheckpoint(const std::string& rawLabel,
                               const std::string& rangesJson,
                               bool mutationAllowed, std::string* error) {
    json ranges;
    try {
        ranges = json::parse(Trim(rangesJson).empty() ? "[]" : rangesJson);
    } catch (...) {
        if (error) *error = "checkpoint_ranges_must_be_array";
        return false;
    }
    if (!ranges.is_array()) {
        if (error) *error = "checkpoint_ranges_must_be_array";
        return false;
    }

    json result;
    if (!Call("re_checkpoint_create",
              {{"label", Trim(rawLabel)}, {"ranges", std::move(ranges)}},
              result, true, mutationAllowed, error)) return false;
    SetResult(result);
    return RefreshCheckpoints(error);
}

bool ReModel::RollbackCheckpoint(int id, bool keep, bool mutationAllowed,
                                 std::string* error) {
    if (id <= 0) {
        if (error) *error = "invalid_checkpoint_id";
        return false;
    }
    json result;
    if (!Call("re_checkpoint_rollback",
              {{"_path", {{"id", id}}}, {"keep", keep}},
              result, true, mutationAllowed, error)) return false;
    SetResult(result);
    RefreshCheckpoints(nullptr);
    Refresh(nullptr);
    return true;
}

bool ReModel::DeleteCheckpoint(int id, bool mutationAllowed, std::string* error) {
    if (id <= 0) {
        if (error) *error = "invalid_checkpoint_id";
        return false;
    }
    json result;
    if (!Call("re_checkpoint_delete", {{"_path", {{"id", id}}}},
              result, true, mutationAllowed, error)) return false;
    SetResult(result);
    return RefreshCheckpoints(error);
}

bool ReModel::SaveFact(const std::string& rawKey, const std::string& valueText,
                       bool mutationAllowed, std::string* error) {
    const std::string key = Trim(rawKey);
    if (key.empty()) {
        if (error) *error = "fact_key_required";
        return false;
    }

    json value;
    try {
        value = json::parse(valueText);
    } catch (...) {
        value = valueText;
    }

    json result;
    if (!Call("re_session_fact_set", {{"key", key}, {"value", value}},
              result, true, mutationAllowed, error)) return false;
    SetResult(result);
    return Refresh(error);
}

bool ReModel::SaveBreakpointTemplates(const std::string& jsonText,
                                      bool mutationAllowed, std::string* error) {
    json templates;
    try {
        templates = json::parse(jsonText);
    } catch (...) {
        if (error) *error = "breakpoint_templates_must_be_array";
        return false;
    }
    if (!templates.is_array()) {
        if (error) *error = "breakpoint_templates_must_be_array";
        return false;
    }

    json result;
    if (!Call("re_session_breakpoints", {{"templates", std::move(templates)}},
              result, true, mutationAllowed, error)) return false;
    SetResult(result);
    return Refresh(error);
}

bool ReModel::ApplyBreakpointTemplates(bool mutationAllowed, std::string* error) {
    json result;
    return Call("re_session_apply_breakpoints", json::object(), result,
                true, mutationAllowed, error);
}

bool ReModel::ExportSession(std::string* error) {
    json result;
    if (!Call("session_export", json::object(), result, false, false, error))
        return false;
    SetResult(result);
    RefreshSessions(nullptr);
    return true;
}

bool ReModel::DiffSessions(const std::string& a, const std::string& b,
                           std::string* error) {
    if (a.empty() || b.empty() || a == b) {
        if (error) *error = "two_distinct_sessions_required";
        return false;
    }
    json result;
    return Call("session_diff", {{"a", a}, {"b", b}},
                result, false, false, error);
}

bool ReModel::GhidraExport(const std::string& name, std::string* error) {
    json result;
    return Call("ghidra_export", {{"name", Trim(name)}},
                result, false, false, error);
}

bool ReModel::GhidraImport(const std::string& jsonText, bool mutationAllowed,
                           std::string* error) {
    json document;
    try {
        document = json::parse(jsonText);
    } catch (...) {
        if (error) *error = "ghidra_import_must_be_object";
        return false;
    }
    if (!document.is_object()) {
        if (error) *error = "ghidra_import_must_be_object";
        return false;
    }

    json result;
    if (!Call("ghidra_import_symbols", std::move(document), result,
              true, mutationAllowed, error)) return false;
    SetResult(result);
    Refresh(nullptr);
    return true;
}

std::string ReModel::SelectedTrackAddress() const {
    const auto found = std::find_if(tracks_.begin(), tracks_.end(),
                                    [&](const ReTrack& row) {
                                        return row.id == selectedTrackId_;
                                    });
    return found == tracks_.end() ? std::string() : found->address;
}

} // namespace cortex::application
