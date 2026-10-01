#pragma once

#include "runtime_model_base.h"

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

class NetworkModel : public RuntimeModelBase {
public:
    explicit NetworkModel(services::RuntimeTransport& payload) : RuntimeModelBase(payload) {}

    void Reset();
    bool Refresh(std::string* error = nullptr);
    bool SetCapture(bool enabled, bool mutationAllowed, std::string* error = nullptr);

    bool CaptureEnabled() const { return captureEnabled_; }
    const std::vector<NetworkEvent>& Events() const { return events_; }

private:

    bool captureEnabled_ = false;
    std::vector<NetworkEvent> events_;
};

} // namespace cortex::application
