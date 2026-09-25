#include "watches_model.h"

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

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

} // namespace

void WatchesModel::Reset() {
    watches_.clear();
    freezes_.clear();
}

bool WatchesModel::EnsureRuntime(bool allowInjection, bool mutationAllowed,
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

bool WatchesModel::Call(const std::string& tool, json arguments,
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

bool WatchesModel::ParseValue(const std::string& rawType, const std::string& rawText,
                              json& value) const {
    const std::string type = Lower(Trim(rawType));
    const std::string text = Trim(rawText);
    if (type.empty() || text.empty()) return false;

    try {
        if (type == "float" || type == "double") {
            size_t used = 0;
            const double parsed = std::stod(text, &used);
            if (used != text.size()) return false;
            value = parsed;
            return true;
        }
        if (type == "bytes") {
            value = text;
            return true;
        }
        if (!type.empty() && type.front() == 'u') {
            size_t used = 0;
            const uint64_t parsed = std::stoull(text, &used, 0);
            if (used != text.size()) return false;
            value = parsed;
            return true;
        }
        size_t used = 0;
        const int64_t parsed = std::stoll(text, &used, 0);
        if (used != text.size()) return false;
        value = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

bool WatchesModel::Refresh(std::string* error) {
    json freezeResult;
    if (!Call("freeze_list", json::object(), freezeResult, false, false, error))
        return false;

    json watchResult;
    if (!Call("watch_list", json::object(), watchResult, false, false, error))
        return false;

    freezes_.clear();
    const json freezeEntries = freezeResult.value("freezes", json::array());
    if (freezeEntries.is_array()) {
        for (const auto& entry : freezeEntries) {
            if (!entry.is_object()) continue;
            RuntimeFreeze row;
            row.id = entry.value("id", -1);
            row.address = entry.value("address", std::string());
            row.type = entry.value("type", std::string());
            row.valueBytes = entry.value("value_bytes", std::string());
            row.label = entry.value("label", std::string());
            row.ttlMsRemaining = entry.value("ttl_ms_remaining", int64_t{0});
            freezes_.push_back(std::move(row));
        }
    }

    watches_.clear();
    const json watchEntries = watchResult.value("watches", json::array());
    if (watchEntries.is_array()) {
        for (const auto& entry : watchEntries) {
            if (!entry.is_object()) continue;
            RuntimeWatch row;
            row.id = entry.value("id", -1);
            row.address = entry.value("address", std::string());
            row.type = entry.value("type", std::string());
            row.label = entry.value("label", std::string());
            row.value = entry.value("value", std::string());
            row.hasValue = entry.value("has_value", false);
            watches_.push_back(std::move(row));
        }
    }
    return true;
}

bool WatchesModel::AddWatch(const std::string& rawAddress, const std::string& rawType,
                            const std::string& rawLabel, bool mutationAllowed,
                            std::string* error) {
    const std::string address = Trim(rawAddress);
    const std::string type = Lower(Trim(rawType));
    if (address.empty() || type.empty()) {
        if (error) *error = "invalid_watch_configuration";
        return false;
    }

    json arguments{{"address", address}, {"type", type}};
    const std::string label = Trim(rawLabel);
    if (!label.empty()) arguments["label"] = label;

    json result;
    if (!Call("watch_add", std::move(arguments), result,
              true, mutationAllowed, error)) return false;
    return Refresh(error);
}

bool WatchesModel::DeleteWatch(int id, bool mutationAllowed, std::string* error) {
    if (id < 0) {
        if (error) *error = "invalid_watch_id";
        return false;
    }
    json result;
    if (!Call("watch_delete", {{"_path", {{"id", id}}}},
              result, true, mutationAllowed, error)) return false;
    return Refresh(error);
}

bool WatchesModel::AddFreeze(const std::string& rawAddress, const std::string& rawType,
                             const std::string& valueText, const std::string& rawLabel,
                             int ttlMs, bool mutationAllowed, std::string* error) {
    const std::string address = Trim(rawAddress);
    const std::string type = Lower(Trim(rawType));
    if (address.empty() || type.empty() || ttlMs < 0) {
        if (error) *error = "invalid_freeze_configuration";
        return false;
    }

    json value;
    if (!ParseValue(type, valueText, value)) {
        if (error) *error = "invalid_freeze_value";
        return false;
    }

    json arguments{{"address", address}, {"type", type}, {"value", std::move(value)}};
    const std::string label = Trim(rawLabel);
    if (!label.empty()) arguments["label"] = label;
    if (ttlMs > 0) arguments["ttl_ms"] = ttlMs;

    json result;
    if (!Call("freeze_add", std::move(arguments), result,
              true, mutationAllowed, error)) return false;
    return Refresh(error);
}

bool WatchesModel::DeleteFreeze(int id, bool mutationAllowed, std::string* error) {
    if (id < 0) {
        if (error) *error = "invalid_freeze_id";
        return false;
    }
    json result;
    if (!Call("freeze_delete", {{"_path", {{"id", id}}}},
              result, true, mutationAllowed, error)) return false;
    return Refresh(error);
}

} // namespace cortex::application
