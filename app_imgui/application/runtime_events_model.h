#pragma once

#include "services/runtime_transport.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cortex::application {

struct RuntimeEvent {
    uint64_t id = 0;
    uint64_t timestampMs = 0;
    std::string type;
    std::string dataJson = "{}";
};

class RuntimeEventsModel {
public:
    explicit RuntimeEventsModel(services::RuntimeTransport& payload) : payload_(payload) {}

    void Reset();
    bool RefreshEvents(std::string* error = nullptr);
    bool RefreshApiLog(std::string* error = nullptr);

    const std::vector<RuntimeEvent>& Events() const { return events_; }
    const std::vector<std::string>& ApiLog() const { return apiLog_; }
    uint64_t LastEventId() const { return lastEventId_; }

private:
    services::RuntimeTransport& payload_;
    std::vector<RuntimeEvent> events_;
    std::vector<std::string> apiLog_;
    uint64_t lastEventId_ = 0;
};

} // namespace cortex::application
