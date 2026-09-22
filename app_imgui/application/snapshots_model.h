#pragma once

#include "services/payload_client.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cortex::application {

struct SnapshotInfo {
    int id = -1;
    uint64_t timestampMs = 0;
    std::string label;
    uint64_t rangeCount = 0;
    uint64_t totalBytes = 0;
};

class SnapshotsModel {
public:
    explicit SnapshotsModel(services::PayloadClient& payload) : payload_(payload) {}

    void Reset();
    bool Refresh(std::string* error = nullptr);
    bool Create(const std::string& rangesJson, const std::string& label,
                std::string* error = nullptr);
    bool Diff(int fromId, int toId, std::string* error = nullptr);
    bool Rewind(int id, bool mutationAllowed, std::string* error = nullptr);
    bool Delete(int id, bool mutationAllowed, std::string* error = nullptr);
    bool LastChange(const std::string& address, int size,
                    std::string* error = nullptr);

    const std::vector<SnapshotInfo>& Snapshots() const { return snapshots_; }
    const std::string& ResultJson() const { return resultJson_; }

private:
    bool EnsureRuntime(bool allowInjection, bool mutationAllowed, std::string* error);
    bool Call(const std::string& tool, nlohmann::json arguments,
              nlohmann::json& result, bool mutation, bool mutationAllowed,
              std::string* error);

    services::PayloadClient& payload_;
    std::vector<SnapshotInfo> snapshots_;
    std::string resultJson_;
};

} // namespace cortex::application
