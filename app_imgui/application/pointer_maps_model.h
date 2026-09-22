#pragma once

#include "services/payload_client.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cortex::application {

struct PointerMapInfo {
    std::string name;
    std::string target;
    uint64_t createdMs = 0;
    uint64_t pathCount = 0;
    bool truncated = false;
};

struct PointerPathCandidate {
    std::string module;
    int64_t baseOffset = 0;
    std::string offsetsJson = "[]";
    int sessions = 0;
    double score = 0.0;
};

class PointerMapsModel {
public:
    explicit PointerMapsModel(services::PayloadClient& payload) : payload_(payload) {}

    void Reset();
    bool Refresh(std::string* error = nullptr);
    bool Capture(const std::string& name, const std::string& target,
                 int maxDepth, int maxOffset, bool mutationAllowed,
                 std::string* error = nullptr);
    bool Intersect(const std::vector<std::string>& names,
                   std::string* error = nullptr);
    bool Delete(const std::string& name, bool mutationAllowed,
                std::string* error = nullptr);

    const std::vector<PointerMapInfo>& Maps() const { return maps_; }
    const std::vector<PointerPathCandidate>& Paths() const { return paths_; }

private:
    bool EnsureRuntime(bool allowInjection, bool mutationAllowed, std::string* error);
    bool Call(const std::string& tool, nlohmann::json arguments,
              nlohmann::json& result, bool mutation, bool mutationAllowed,
              std::string* error);

    services::PayloadClient& payload_;
    std::vector<PointerMapInfo> maps_;
    std::vector<PointerPathCandidate> paths_;
};

} // namespace cortex::application
