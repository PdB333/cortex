#pragma once

#include "services/payload_client.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cortex::application {

struct AllocationEvent {
    int64_t timestampMs = 0;
    std::string api;
    std::string address;
    uint64_t size = 0;
    uint64_t flags = 0;
};

struct PageAccessWatch {
    int id = -1;
    std::string address;
    uint64_t size = 0;
    std::string label;
};

struct PageAccessEvent {
    int64_t timestampMs = 0;
    int watchId = -1;
    std::string address;
    std::string access;
    std::string label;
    uint64_t threadId = 0;
    std::string instruction;
    uint64_t size = 0;
    std::string before;
    std::string after;
    std::string registersJson = "{}";
    std::string stackJson = "[]";
};

class InstrumentationModel {
public:
    explicit InstrumentationModel(services::PayloadClient& payload) : payload_(payload) {}

    void Reset();
    bool RefreshState(std::string* error = nullptr);
    bool RefreshEvents(std::string* error = nullptr);

    bool SetAllocationWatch(bool enabled, uint64_t minSize,
                            bool mutationAllowed, std::string* error = nullptr);
    bool AddPageAccessWatch(const std::string& address, int size,
                            const std::string& label, bool mutationAllowed,
                            std::string* error = nullptr);
    bool DeletePageAccessWatch(int id, bool mutationAllowed,
                               std::string* error = nullptr);

    bool AllocationWatchEnabled() const { return allocationWatchEnabled_; }
    uint64_t AllocationWatchMinSize() const { return allocationWatchMinSize_; }
    const std::vector<PageAccessWatch>& PageAccessWatches() const { return pageAccessWatches_; }
    const std::vector<AllocationEvent>& AllocationEvents() const { return allocationEvents_; }
    const std::vector<PageAccessEvent>& PageAccessEvents() const { return pageAccessEvents_; }

private:
    bool EnsureRuntime(bool allowInjection, bool mutationAllowed, std::string* error);
    bool Call(const std::string& tool, nlohmann::json arguments,
              nlohmann::json& result, bool mutation, bool mutationAllowed,
              std::string* error);

    services::PayloadClient& payload_;
    bool allocationWatchEnabled_ = false;
    uint64_t allocationWatchMinSize_ = 0;
    std::vector<PageAccessWatch> pageAccessWatches_;
    std::vector<AllocationEvent> allocationEvents_;
    std::vector<PageAccessEvent> pageAccessEvents_;
};

} // namespace cortex::application
