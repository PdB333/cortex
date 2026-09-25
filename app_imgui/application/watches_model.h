#pragma once

#include "services/payload_client.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cortex::application {

struct RuntimeWatch {
    int id = -1;
    std::string address;
    std::string type;
    std::string label;
    std::string value;
    bool hasValue = false;
};

struct RuntimeFreeze {
    int id = -1;
    std::string address;
    std::string type;
    std::string valueBytes;
    std::string label;
    int64_t ttlMsRemaining = 0;
};

class WatchesModel {
public:
    explicit WatchesModel(services::PayloadClient& payload) : payload_(payload) {}

    void Reset();
    bool Refresh(std::string* error = nullptr);
    bool AddWatch(const std::string& address, const std::string& type,
                  const std::string& label, bool mutationAllowed,
                  std::string* error = nullptr);
    bool DeleteWatch(int id, bool mutationAllowed, std::string* error = nullptr);
    bool AddFreeze(const std::string& address, const std::string& type,
                   const std::string& valueText, const std::string& label,
                   int ttlMs, bool mutationAllowed, std::string* error = nullptr);
    bool DeleteFreeze(int id, bool mutationAllowed, std::string* error = nullptr);

    const std::vector<RuntimeWatch>& Watches() const { return watches_; }
    const std::vector<RuntimeFreeze>& Freezes() const { return freezes_; }

private:
    bool EnsureRuntime(bool allowInjection, bool mutationAllowed, std::string* error);
    bool Call(const std::string& tool, nlohmann::json arguments,
              nlohmann::json& result, bool mutation, bool mutationAllowed,
              std::string* error);
    bool ParseValue(const std::string& type, const std::string& text,
                    nlohmann::json& value) const;

    services::PayloadClient& payload_;
    std::vector<RuntimeWatch> watches_;
    std::vector<RuntimeFreeze> freezes_;
};

} // namespace cortex::application
