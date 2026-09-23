#pragma once

#include "services/payload_client.h"

#include <string>
#include <vector>

namespace cortex::application {

struct PatchInfo {
    int id = -1;
    std::string address;
    std::string originalBytes;
    std::string currentBytes;
    std::string label;
    std::string gateway;
};

class PatchesModel {
public:
    explicit PatchesModel(services::PayloadClient& payload) : payload_(payload) {}

    void Reset();
    bool Refresh(std::string* error = nullptr);
    bool ApplyBytes(const std::string& address, const std::string& bytes,
                    const std::string& label, bool mutationAllowed,
                    std::string* error = nullptr);
    bool ApplyNop(const std::string& address, int size,
                  const std::string& label, bool mutationAllowed,
                  std::string* error = nullptr);
    bool ApplyAssembly(const std::string& address, const std::string& instructions,
                       const std::string& label, bool mutationAllowed,
                       std::string* error = nullptr);
    bool ApplyDetour(const std::string& address, const std::string& target,
                     int jumpSize, bool mutationAllowed,
                     std::string* error = nullptr);
    bool ApplyTrampoline(const std::string& address, const std::string& target,
                         int minimumOverwrite, bool mutationAllowed,
                         std::string* error = nullptr);
    bool AllocateCave(const std::string& nearAddress, int size,
                      bool mutationAllowed, std::string* error = nullptr);
    bool Revert(int patchId, bool mutationAllowed, std::string* error = nullptr);

    const std::vector<PatchInfo>& Patches() const { return patches_; }
    const std::string& OperationResult() const { return operationResult_; }

private:
    bool EnsureRuntime(bool allowInjection, bool mutationAllowed, std::string* error);
    bool Call(const std::string& tool, nlohmann::json arguments,
              nlohmann::json& result, bool mutation, bool mutationAllowed,
              std::string* error);
    static std::string Trim(std::string value);

    services::PayloadClient& payload_;
    std::vector<PatchInfo> patches_;
    std::string operationResult_;
};

} // namespace cortex::application
