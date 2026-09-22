#pragma once

#include "services/payload_client.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cortex::application {

struct NetworkEvent {
    uint64_t id = 0;
    uint64_t tickMs = 0;
    std::string direction;
    uint64_t socket = 0;
    uint64_t size = 0;
    std::string previewHex;
};

class NetworkModel {
public:
    explicit NetworkModel(services::PayloadClient& payload) : payload_(payload) {}

    void Reset();
    bool Refresh(std::string* error = nullptr);
    bool SetCapture(bool enabled, bool mutationAllowed, std::string* error = nullptr);

    bool CaptureEnabled() const { return captureEnabled_; }
    const std::vector<NetworkEvent>& Events() const { return events_; }

private:
    bool EnsureRuntime(bool allowInjection, bool mutationAllowed, std::string* error);
    bool Call(const std::string& tool, nlohmann::json arguments,
              nlohmann::json& result, bool mutation, bool mutationAllowed,
              std::string* error);

    services::PayloadClient& payload_;
    bool captureEnabled_ = false;
    std::vector<NetworkEvent> events_;
};

} // namespace cortex::application
