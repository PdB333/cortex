#pragma once

#include "services/payload_client.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cortex::application {

struct DiagnosticHook {
    std::string name;
    bool installed = false;
    std::string backend;
};

struct DiagnosticToolStats {
    int total = 0;
    int get = 0;
    int post = 0;
    int remove = 0;
    int publicCount = 0;
};

class DiagnosticsModel {
public:
    explicit DiagnosticsModel(services::PayloadClient& payload) : payload_(payload) {}

    void Reset();
    bool Refresh(std::string* error = nullptr);

    const std::string& Summary() const { return summary_; }
    const std::string& StatusJson() const { return statusJson_; }
    const std::string& HealthJson() const { return healthJson_; }
    const std::vector<DiagnosticHook>& Hooks() const { return hooks_; }
    const DiagnosticToolStats& ToolStats() const { return toolStats_; }

private:
    bool EnsureRuntime(std::string* error);
    bool Call(const std::string& tool, nlohmann::json arguments,
              nlohmann::json& result, std::string* error);

    services::PayloadClient& payload_;
    std::string summary_;
    std::string statusJson_;
    std::string healthJson_;
    std::vector<DiagnosticHook> hooks_;
    DiagnosticToolStats toolStats_;
};

} // namespace cortex::application
