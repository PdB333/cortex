#pragma once

// The per-user stores the desktop shares with `cortex.exe mcp` (whose
// defaults are the same folders): annotations, evidence and investigations.
// In the desktop the actor is the person ("desktop").

#include "../../core/artifacts/annotations.h"
#include "../../core/evidence/evidence.h"
#include "../../core/investigation/investigation.h"
#include "../../core/providers/analysis_provider.h"
#include "../../core/live/cdp.h"
#include "../../core/services/user_data.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>

namespace cortex::desktop {
std::string ExecutableDirectory();
}

namespace cortex::ui {

inline std::filesystem::path DesktopDataDirectory() {
    return cortex::services::UserDataDirectory(std::filesystem::u8path(cortex::desktop::ExecutableDirectory()));
}

inline cortex::artifacts::AnnotationStore& DesktopAnnotations() {
    static cortex::artifacts::AnnotationStore store(DesktopDataDirectory() / "annotations");
    return store;
}

inline cortex::evidence::Store& DesktopEvidence() {
    static cortex::evidence::Store store(DesktopDataDirectory() / "evidence");
    return store;
}

inline cortex::investigation::Store& DesktopInvestigations() {
    static cortex::investigation::Store store(DesktopDataDirectory() / "investigations");
    return store;
}

// Live CDP sessions (Node.js/Electron inspectors). In the desktop the person
// is the operator: any port on 127.0.0.1. Breakpoints still need the
// desktop's write permission.
inline cortex::live::LiveService& DesktopLive() {
    static cortex::live::LiveService service({}, true);
    return service;
}

// External engines (Ghidra, ILSpy). The person is the operator here: the
// engine paths they set are kept in analysis-engines.json; empty fields fall
// back to GHIDRA_INSTALL_DIR / CORTEX_ILSPYCMD / PATH. The result cache is
// the folder `cortex.exe mcp` uses by default.
inline cortex::providers::Settings DesktopEngineSettings() {
    cortex::providers::Settings settings;
    settings.cacheDir = DesktopDataDirectory() / "analysis-cache";
    settings.tempDir = std::filesystem::temp_directory_path();
    try {
        std::ifstream file(DesktopDataDirectory() / "analysis-engines.json");
        if (file) {
            const auto saved = nlohmann::json::parse(file);
            settings.ghidraDir = saved.value("ghidra_dir", std::string());
            settings.ilspycmd = saved.value("ilspycmd", std::string());
            settings.dotnet = saved.value("dotnet", std::string());
            settings.vineflowerJar = saved.value("vineflower", std::string());
        }
    } catch (...) {
    }
    return settings;
}

struct DesktopAnalysisSlot {
    std::mutex mutex;
    std::shared_ptr<cortex::providers::Service> service;
};

inline DesktopAnalysisSlot& DesktopAnalysisState() {
    static DesktopAnalysisSlot slot;
    return slot;
}

inline std::shared_ptr<cortex::providers::Service> DesktopAnalysis() {
    auto& slot = DesktopAnalysisState();
    std::lock_guard<std::mutex> lock(slot.mutex);
    if (!slot.service) slot.service = std::make_shared<cortex::providers::Service>(DesktopEngineSettings());
    return slot.service;
}

// Saves the engine paths and starts a new service (running jobs of the old
// one are cancelled when it is released).
inline bool ConfigureDesktopAnalysis(const std::string& ghidraDir, const std::string& ilspycmd, const std::string& dotnet,
                                     const std::string& vineflower, std::string& error) {
    const nlohmann::json saved{{"ghidra_dir", ghidraDir}, {"ilspycmd", ilspycmd}, {"dotnet", dotnet}, {"vineflower", vineflower}};
    std::error_code ec;
    std::filesystem::create_directories(DesktopDataDirectory(), ec);
    if (!cortex::services::WriteFileAtomically(DesktopDataDirectory() / "analysis-engines.json", saved.dump(2), &error)) return false;
    auto& slot = DesktopAnalysisState();
    std::shared_ptr<cortex::providers::Service> old;
    {
        std::lock_guard<std::mutex> lock(slot.mutex);
        old = std::move(slot.service);
        slot.service = std::make_shared<cortex::providers::Service>(DesktopEngineSettings());
    }
    return true;
}

} // namespace cortex::ui
