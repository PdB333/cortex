#include "settings_workspace.h"

#include <imgui.h>

#include <algorithm>
#include <cstring>

namespace cortex::ui {
namespace {

template <size_t N>
void Copy(std::array<char, N>& destination, const std::string& source) {
    std::fill(destination.begin(), destination.end(), '\0');
    const size_t count = std::min(source.size(), destination.size() - 1);
    std::memcpy(destination.data(), source.data(), count);
}

int IndexOf(int value, const int* values, int count) {
    for (int i = 0; i < count; ++i) if (values[i] == value) return i;
    return 0;
}

int IndexOf(const std::string& value, const char* const* values, int count) {
    for (int i = 0; i < count; ++i) if (value == values[i]) return i;
    return 0;
}

} // namespace

void SettingsWorkspace::SyncBuffers(UiContext& context) {
    if (!context.settings) return;
    const auto& value = context.settings->Values();
    Copy(crashDirectory_, value.diagnosticsCrashDirectory);
    Copy(symbolPath_, value.diagnosticsSymbolPath);
    Copy(projectDirectory_, value.projectDirectory);
    Copy(sessionDirectory_, value.sessionDirectory);
    buffersInitialized_ = true;
}

void SettingsWorkspace::Save(UiContext& context) {
    if (!context.settings) return;
    std::string error;
    if (context.settings->SaveAndSync(&error))
        context.status = "Settings saved";
    else
        context.status = "Settings save failed: " + error;
}

void SettingsWorkspace::Draw(UiContext& context) {
    if (!context.settings) {
        ImGui::TextDisabled("Settings store unavailable.");
        return;
    }
    if (!buffersInitialized_) SyncBuffers(context);

    auto& value = context.settings->Values();
    bool changed = false;

    ImGui::TextUnformatted("Cortex settings");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", context.settings->Path().u8string().c_str());
    ImGui::Spacing();

    if (ImGui::CollapsingHeader("Runtime & diagnostics", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= ImGui::Checkbox("Load runtime automatically after attach", &value.autoLoadRuntimeOnAttach);
        changed |= ImGui::Checkbox("Legacy HTTP compatibility API", &value.httpApiEnabled);
        changed |= ImGui::Checkbox("Runtime diagnostics", &value.diagnosticsEnabled);
        changed |= ImGui::Checkbox("Write minidumps", &value.diagnosticsWriteMinidump);

        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##CrashDirectory", "Crash / dump directory (empty = runtime default)",
                                 crashDirectory_.data(), crashDirectory_.size());
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            value.diagnosticsCrashDirectory = crashDirectory_.data();
            changed = true;
        }

        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##SymbolPath", "Symbol search path",
                                 symbolPath_.data(), symbolPath_.size());
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            value.diagnosticsSymbolPath = symbolPath_.data();
            changed = true;
        }

        int stackFrames = value.diagnosticsMaxStackFrames;
        ImGui::SetNextItemWidth(160);
        if (ImGui::InputInt("Maximum diagnostic stack frames", &stackFrames)) {
            value.diagnosticsMaxStackFrames = std::clamp(stackFrames, 16, 256);
            changed = true;
        }
    }

    if (ImGui::CollapsingHeader("Memory & scanner", ImGuiTreeNodeFlags_DefaultOpen)) {
        static const int rowWidths[] = {8, 16, 32};
        static const char* rowWidthNames[] = {"8", "16", "32"};
        int rowIndex = IndexOf(value.memoryBytesPerRow, rowWidths, IM_ARRAYSIZE(rowWidths));
        if (ImGui::Combo("Memory bytes per row", &rowIndex, rowWidthNames, IM_ARRAYSIZE(rowWidthNames))) {
            value.memoryBytesPerRow = rowWidths[rowIndex];
            changed = true;
        }

        static const int readSizes[] = {128, 256, 512, 1024, 2048, 4096};
        static const char* readSizeNames[] = {"128", "256", "512", "1024", "2048", "4096"};
        int readIndex = IndexOf(value.memoryReadSize, readSizes, IM_ARRAYSIZE(readSizes));
        if (ImGui::Combo("Default memory read size", &readIndex, readSizeNames, IM_ARRAYSIZE(readSizeNames))) {
            value.memoryReadSize = readSizes[readIndex];
            changed = true;
        }

        static const char* scanTypes[] = {"i32", "i64", "f32", "f64", "string", "bytes"};
        int scanIndex = IndexOf(value.defaultScanType, scanTypes, IM_ARRAYSIZE(scanTypes));
        if (ImGui::Combo("Default scan type", &scanIndex, scanTypes, IM_ARRAYSIZE(scanTypes))) {
            value.defaultScanType = scanTypes[scanIndex];
            changed = true;
        }

        int maxResults = value.maxScanResults;
        if (ImGui::InputInt("Maximum scan results", &maxResults)) {
            value.maxScanResults = std::clamp(maxResults, 100, 50000);
            changed = true;
        }
    }

    if (ImGui::CollapsingHeader("Debugger & trace", ImGuiTreeNodeFlags_DefaultOpen)) {
        static const char* backends[] = {"windows", "veh"};
        int backend = IndexOf(value.debuggerBackend, backends, IM_ARRAYSIZE(backends));
        if (ImGui::Combo("Debugger backend", &backend, backends, IM_ARRAYSIZE(backends))) {
            value.debuggerBackend = backends[backend];
            changed = true;
        }

        static const char* actions[] = {"log", "pause"};
        int action = IndexOf(value.breakpointDefaultAction, actions, IM_ARRAYSIZE(actions));
        if (ImGui::Combo("Default breakpoint action", &action, actions, IM_ARRAYSIZE(actions))) {
            value.breakpointDefaultAction = actions[action];
            changed = true;
        }

        changed |= ImGui::Checkbox("Hardware breakpoints process-global", &value.hardwareBreakpointsGlobal);

        int traceSteps = value.traceMaxSteps;
        if (ImGui::InputInt("Default trace step budget", &traceSteps)) {
            value.traceMaxSteps = std::clamp(traceSteps, 100, 1000000);
            changed = true;
        }

        int eventLimit = value.traceEventLoadLimit;
        if (ImGui::InputInt("Trace events loaded per request", &eventLimit)) {
            value.traceEventLoadLimit = std::clamp(eventLimit, 50, 5000);
            changed = true;
        }
    }

    if (ImGui::CollapsingHeader("Projects & sessions", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##ProjectDirectory", "Project storage directory",
                                 projectDirectory_.data(), projectDirectory_.size());
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            value.projectDirectory = projectDirectory_.data();
            changed = true;
        }

        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##SessionDirectory", "Session export directory",
                                 sessionDirectory_.data(), sessionDirectory_.size());
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            value.sessionDirectory = sessionDirectory_.data();
            changed = true;
        }

        int historyLimit = value.sessionHistoryLimit;
        if (ImGui::InputInt("Session history retention", &historyLimit)) {
            value.sessionHistoryLimit = std::clamp(historyLimit, 0, 500);
            changed = true;
        }
    }

    if (ImGui::CollapsingHeader("MCP & AI activity", ImGuiTreeNodeFlags_DefaultOpen)) {
        static const char* profiles[] = {"compact", "all"};
        int profile = IndexOf(value.mcpToolProfile, profiles, IM_ARRAYSIZE(profiles));
        if (ImGui::Combo("Default MCP tool profile", &profile, profiles, IM_ARRAYSIZE(profiles))) {
            value.mcpToolProfile = profiles[profile];
            changed = true;
        }

        int activityLimit = value.aiActivityHistoryLimit;
        if (ImGui::InputInt("AI Activity history limit", &activityLimit)) {
            value.aiActivityHistoryLimit = std::clamp(activityLimit, 50, 2000);
            changed = true;
        }
        changed |= ImGui::Checkbox("Show active AI status in title bar", &value.showAiActivityInTitleBar);

        int refreshMs = value.autoRefreshMs;
        if (ImGui::InputInt("UI auto refresh (ms)", &refreshMs)) {
            value.autoRefreshMs = std::clamp(refreshMs, 100, 10000);
            changed = true;
        }
    }

    if (changed) Save(context);

    ImGui::Separator();
    if (ImGui::Button("Reset technical defaults")) {
        context.settings->ResetDefaults();
        SyncBuffers(context);
        Save(context);
    }
}

} // namespace cortex::ui
