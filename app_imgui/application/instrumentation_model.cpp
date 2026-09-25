#include "instrumentation_model.h"

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

void InstrumentationModel::Reset() {
    allocationWatchEnabled_ = false;
    allocationWatchMinSize_ = 0;
    pageAccessWatches_.clear();
    allocationEvents_.clear();
    pageAccessEvents_.clear();
}

bool InstrumentationModel::EnsureRuntime(bool allowInjection, bool mutationAllowed,
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

bool InstrumentationModel::Call(const std::string& tool, json arguments,
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

bool InstrumentationModel::RefreshState(std::string* error) {
    json allocationResult;
    if (!Call("watch_allocations_status", json::object(), allocationResult,
              false, false, error)) return false;
    if (!allocationResult.is_object()) {
        if (error) *error = "allocation_watch_status_invalid";
        return false;
    }

    json pageResult;
    if (!Call("watch_page_access_list", json::object(), pageResult,
              false, false, error)) return false;
    if (!pageResult.is_object()) {
        if (error) *error = "page_watch_list_invalid";
        return false;
    }

    allocationWatchEnabled_ = allocationResult.value("enabled", false);
    allocationWatchMinSize_ = allocationResult.value("min_size", uint64_t{0});

    pageAccessWatches_.clear();
    const json watches = pageResult.value("watches", json::array());
    if (watches.is_array()) {
        for (const auto& watch : watches) {
            if (!watch.is_object()) continue;
            PageAccessWatch row;
            row.id = watch.value("id", -1);
            row.address = watch.value("address", std::string());
            row.size = watch.value("size", uint64_t{0});
            row.label = watch.value("label", std::string());
            pageAccessWatches_.push_back(std::move(row));
        }
    }
    return true;
}

bool InstrumentationModel::RefreshEvents(std::string* error) {
    json allocationResult;
    if (!Call("watch_allocations_events_snapshot", json::object(), allocationResult,
              false, false, error)) return false;
    if (!allocationResult.is_object()) {
        if (error) *error = "allocation_events_snapshot_invalid";
        return false;
    }

    json pageResult;
    if (!Call("watch_page_access_events_snapshot", json::object(), pageResult,
              false, false, error)) return false;
    if (!pageResult.is_object()) {
        if (error) *error = "page_access_events_snapshot_invalid";
        return false;
    }

    allocationEvents_.clear();
    const json allocations = allocationResult.value("events", json::array());
    if (allocations.is_array()) {
        for (const auto& event : allocations) {
            if (!event.is_object()) continue;
            AllocationEvent row;
            row.timestampMs = event.value("timestamp_ms", int64_t{0});
            row.api = event.value("api", std::string());
            row.address = event.value("address", std::string());
            row.size = event.value("size", uint64_t{0});
            row.flags = event.value("protect_or_flags", uint64_t{0});
            allocationEvents_.push_back(std::move(row));
        }
    }

    pageAccessEvents_.clear();
    const json pageEvents = pageResult.value("events", json::array());
    if (pageEvents.is_array()) {
        for (const auto& event : pageEvents) {
            if (!event.is_object()) continue;
            PageAccessEvent row;
            row.timestampMs = event.value("timestamp_ms", int64_t{0});
            row.watchId = event.value("watch_id", -1);
            row.address = event.value("address", std::string());
            row.access = event.value("access", std::string());
            row.label = event.value("label", std::string());
            row.threadId = event.value("thread_id", uint64_t{0});
            row.instruction = event.value("instruction", std::string());
            row.size = event.value("access_size", uint64_t{0});
            row.before = event.value("before", std::string());
            row.after = event.value("after", std::string());
            if (event.contains("registers")) row.registersJson = event.at("registers").dump();
            if (event.contains("stack")) row.stackJson = event.at("stack").dump();
            pageAccessEvents_.push_back(std::move(row));
        }
    }
    return true;
}

bool InstrumentationModel::SetAllocationWatch(bool enabled, uint64_t minSize,
                                               bool mutationAllowed,
                                               std::string* error) {
    json result;
    if (!Call("watch_allocations",
              {{"enabled", enabled}, {"min_size", minSize}},
              result, true, mutationAllowed, error)) return false;
    return RefreshState(error);
}

bool InstrumentationModel::AddPageAccessWatch(const std::string& rawAddress, int size,
                                              const std::string& rawLabel,
                                              bool mutationAllowed,
                                              std::string* error) {
    const std::string address = Trim(rawAddress);
    if (address.empty() || size <= 0 || size > 64 * 1024 * 1024) {
        if (error) *error = "invalid_page_watch_configuration";
        return false;
    }

    json arguments{{"address", address}, {"size", size}};
    const std::string label = Trim(rawLabel);
    if (!label.empty()) arguments["label"] = label;

    json result;
    if (!Call("watch_page_access", std::move(arguments), result,
              true, mutationAllowed, error)) return false;
    return RefreshState(error);
}

bool InstrumentationModel::DeletePageAccessWatch(int id, bool mutationAllowed,
                                                 std::string* error) {
    if (id < 0) {
        if (error) *error = "invalid_page_watch_id";
        return false;
    }

    json result;
    if (!Call("watch_page_access_delete", {{"_path", {{"id", id}}}},
              result, true, mutationAllowed, error)) return false;
    return RefreshState(error);
}

} // namespace cortex::application
