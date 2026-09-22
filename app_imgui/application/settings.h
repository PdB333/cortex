#pragma once

#include <filesystem>
#include <string>

namespace cortex::application {

struct Settings {
    bool autoLoadRuntimeOnAttach = false;
    bool httpApiEnabled = false;
    bool diagnosticsEnabled = true;
    bool diagnosticsWriteMinidump = true;
    std::string diagnosticsCrashDirectory;
    std::string diagnosticsSymbolPath;
    int diagnosticsMaxStackFrames = 64;

    int memoryBytesPerRow = 16;
    int memoryReadSize = 256;
    std::string defaultScanType = "i32";
    int maxScanResults = 5000;

    std::string debuggerBackend = "windows";
    std::string breakpointDefaultAction = "log";
    bool hardwareBreakpointsGlobal = true;
    int traceMaxSteps = 10000;
    int traceEventLoadLimit = 250;

    std::string projectDirectory;
    std::string sessionDirectory;
    int sessionHistoryLimit = 25;

    std::string mcpToolProfile = "compact";
    int aiActivityHistoryLimit = 300;
    bool showAiActivityInTitleBar = true;

    int autoRefreshMs = 750;
};

class SettingsStore {
public:
    explicit SettingsStore(std::string applicationDirectory);

    Settings& Values() { return values_; }
    const Settings& Values() const { return values_; }

    bool Load(std::string* error = nullptr);
    bool Save(std::string* error = nullptr) const;
    bool SyncRuntimeConfig(std::string* error = nullptr) const;
    bool SaveAndSync(std::string* error = nullptr) const;
    void ResetDefaults();

    const std::filesystem::path& Path() const { return path_; }

private:
    std::filesystem::path root_;
    std::filesystem::path path_;
    Settings values_;
};

} // namespace cortex::application
