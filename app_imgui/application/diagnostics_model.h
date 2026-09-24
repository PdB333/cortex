#pragma once

#include "services/crash_report_service.h"
#include "services/payload_client.h"

#include <cstdint>
#include <string>
#include <utility>
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
    DiagnosticsModel(services::PayloadClient& payload, std::string runtimeDirectory)
        : payload_(payload), runtimeDirectory_(std::move(runtimeDirectory)) {}

    void Reset();
    bool Refresh(std::string* error = nullptr);
    bool LoadLatestCrash(const target::TargetDescriptor& target,
                         const std::string& configuredDirectory,
                         std::string* error = nullptr);

    const std::string& Summary() const { return summary_; }
    const std::string& StatusJson() const { return statusJson_; }
    const std::string& HealthJson() const { return healthJson_; }
    const std::vector<DiagnosticHook>& Hooks() const { return hooks_; }
    const DiagnosticToolStats& ToolStats() const { return toolStats_; }
    bool CrashFound() const { return crashFound_; }
    const std::string& CrashDirectory() const { return crashDirectory_; }
    const std::string& CrashReportJson() const { return crashReportJson_; }
    const std::string& CrashSymbolizedJson() const { return crashSymbolizedJson_; }
    const std::string& CrashHooksJson() const { return crashHooksJson_; }
    const std::string& CrashBreadcrumbsJson() const { return crashBreadcrumbsJson_; }

private:
    bool EnsureRuntime(std::string* error);
    bool Call(const std::string& tool, nlohmann::json arguments,
              nlohmann::json& result, std::string* error);

    services::PayloadClient& payload_;
    std::string runtimeDirectory_;
    std::string summary_;
    std::string statusJson_;
    std::string healthJson_;
    std::vector<DiagnosticHook> hooks_;
    DiagnosticToolStats toolStats_;
    bool crashFound_ = false;
    std::string crashDirectory_;
    std::string crashReportJson_;
    std::string crashSymbolizedJson_;
    std::string crashHooksJson_;
    std::string crashBreadcrumbsJson_;
};

} // namespace cortex::application
