#include "settings_workspace.h"
#include "widgets.h"

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

// Settings are laid out as a two-column grid: a wrapping label on the left and
// a control of one consistent width on the right, so every row lines up.
class SettingsGrid {
public:
    explicit SettingsGrid(const char* id) {
        const float available = ImGui::GetContentRegionAvail().x;
        // Below this width a label column would wrap every word, so each row
        // stacks its label above a full-width control instead.
        stacked_ = available < 460.0f;
        controlWidth_ = std::clamp(available * 0.42f, 170.0f, 340.0f);
        open_ = ImGui::BeginTable(id, stacked_ ? 1 : 2,
                                  ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX);
        if (open_ && !stacked_) {
            ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, controlWidth_);
        }
    }
    ~SettingsGrid() { if (open_) ImGui::EndTable(); }
    bool Open() const { return open_; }
    bool Stacked() const { return stacked_; }

    // Starts a row with its label and leaves the cursor in the control cell
    // with the item width set to the full control column.
    void Row(const char* label) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if (!stacked_) ImGui::AlignTextToFramePadding();
        ImGui::TextWrapped("%s", label);
        if (!stacked_) ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-1);
    }

private:
    float controlWidth_ = 240.0f;
    bool stacked_ = false;
    bool open_ = false;
};

bool CheckboxRow(SettingsGrid& grid, const char* label, const char* id, bool* value) {
    if (grid.Stacked()) {
        // A checkbox reads best with its label beside it.
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::PushID(id);
        const bool changed = ImGui::Checkbox(label, value);
        ImGui::PopID();
        return changed;
    }
    grid.Row(label);
    return ImGui::Checkbox(id, value);
}

bool IntRow(SettingsGrid& grid, const char* label, const char* id, int* value, int minimum, int maximum) {
    grid.Row(label);
    int edited = *value;
    if (!ImGui::InputInt(id, &edited)) return false;
    *value = std::clamp(edited, minimum, maximum);
    return true;
}

template <size_t N>
bool PathRow(SettingsGrid& grid, const char* label, const char* id, const char* hint,
             std::array<char, N>& buffer, std::string& target) {
    grid.Row(label);
    ImGui::InputTextWithHint(id, hint, buffer.data(), buffer.size());
    if (!ImGui::IsItemDeactivatedAfterEdit()) return false;
    target = buffer.data();
    return true;
}

template <typename T, size_t N>
bool ComboRow(SettingsGrid& grid, const char* label, const char* id,
              const char* const (&names)[N], const T (&values)[N], T& target) {
    grid.Row(label);
    int index = 0;
    for (size_t i = 0; i < N; ++i) if (values[i] == target) index = static_cast<int>(i);
    if (!ImGui::Combo(id, &index, names, static_cast<int>(N))) return false;
    target = values[index];
    return true;
}

template <size_t N>
bool ComboRow(SettingsGrid& grid, const char* label, const char* id,
              const char* const (&names)[N], std::string& target) {
    grid.Row(label);
    int index = IndexOf(target, names, static_cast<int>(N));
    if (!ImGui::Combo(id, &index, names, static_cast<int>(N))) return false;
    target = names[index];
    return true;
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
    const std::string savedTo = "Saved automatically to " + context.settings->Path().u8string();
    HintText(savedTo.c_str());
    ImGui::Spacing();

    if (ImGui::CollapsingHeader("Runtime & diagnostics", ImGuiTreeNodeFlags_DefaultOpen)) {
        SettingsGrid grid("RuntimeSettings");
        if (grid.Open()) {
            changed |= CheckboxRow(grid, "Load runtime automatically after attach", "##AutoLoadRuntime",
                                   &value.autoLoadRuntimeOnAttach);
            changed |= CheckboxRow(grid, "Legacy HTTP compatibility API", "##HttpApi", &value.httpApiEnabled);
            changed |= CheckboxRow(grid, "Runtime diagnostics", "##Diagnostics", &value.diagnosticsEnabled);
            changed |= CheckboxRow(grid, "Write minidumps", "##Minidumps", &value.diagnosticsWriteMinidump);
            changed |= PathRow(grid, "Crash / dump directory", "##CrashDirectory", "runtime default",
                               crashDirectory_, value.diagnosticsCrashDirectory);
            changed |= PathRow(grid, "Symbol search path", "##SymbolPath", "system default",
                               symbolPath_, value.diagnosticsSymbolPath);
            changed |= IntRow(grid, "Maximum diagnostic stack frames", "##StackFrames",
                              &value.diagnosticsMaxStackFrames, 16, 256);
        }
    }

    if (ImGui::CollapsingHeader("Memory & scanner", ImGuiTreeNodeFlags_DefaultOpen)) {
        SettingsGrid grid("MemorySettings");
        if (grid.Open()) {
            static const int rowWidths[] = {8, 16, 32};
            static const char* const rowWidthNames[] = {"8", "16", "32"};
            changed |= ComboRow(grid, "Memory bytes per row", "##BytesPerRow",
                                rowWidthNames, rowWidths, value.memoryBytesPerRow);

            static const int readSizes[] = {128, 256, 512, 1024, 2048, 4096};
            static const char* const readSizeNames[] = {"128", "256", "512", "1024", "2048", "4096"};
            changed |= ComboRow(grid, "Default memory read size", "##ReadSize",
                                readSizeNames, readSizes, value.memoryReadSize);

            static const char* const scanTypes[] = {"i32", "i64", "f32", "f64", "string", "bytes"};
            changed |= ComboRow(grid, "Default scan type", "##ScanType", scanTypes, value.defaultScanType);
            changed |= IntRow(grid, "Maximum scan results", "##MaxScanResults",
                              &value.maxScanResults, 100, 50000);
        }
    }

    if (ImGui::CollapsingHeader("Debugger & trace", ImGuiTreeNodeFlags_DefaultOpen)) {
        SettingsGrid grid("DebuggerSettings");
        if (grid.Open()) {
            static const char* const backends[] = {"windows", "veh"};
            changed |= ComboRow(grid, "Debugger backend", "##DebuggerBackend", backends, value.debuggerBackend);
            static const char* const actions[] = {"log", "pause"};
            changed |= ComboRow(grid, "Default breakpoint action", "##BreakpointAction",
                                actions, value.breakpointDefaultAction);
            changed |= CheckboxRow(grid, "Hardware breakpoints process-global", "##HwGlobal",
                                   &value.hardwareBreakpointsGlobal);
            changed |= IntRow(grid, "Default trace step budget", "##TraceSteps",
                              &value.traceMaxSteps, 100, 1000000);
            changed |= IntRow(grid, "Trace events loaded per request", "##TraceEvents",
                              &value.traceEventLoadLimit, 50, 5000);
        }
    }

    if (ImGui::CollapsingHeader("Projects & sessions", ImGuiTreeNodeFlags_DefaultOpen)) {
        SettingsGrid grid("ProjectSettings");
        if (grid.Open()) {
            changed |= PathRow(grid, "Project storage directory", "##ProjectDirectory", "default",
                               projectDirectory_, value.projectDirectory);
            changed |= PathRow(grid, "Session export directory", "##SessionDirectory", "default",
                               sessionDirectory_, value.sessionDirectory);
            changed |= IntRow(grid, "Session history retention", "##SessionHistory",
                              &value.sessionHistoryLimit, 0, 500);
        }
    }

    if (ImGui::CollapsingHeader("MCP & AI activity", ImGuiTreeNodeFlags_DefaultOpen)) {
        SettingsGrid grid("McpSettings");
        if (grid.Open()) {
            static const char* const profiles[] = {"compact", "all"};
            changed |= ComboRow(grid, "Default MCP tool profile", "##McpProfile", profiles, value.mcpToolProfile);
            changed |= IntRow(grid, "AI Activity history limit", "##AiHistory",
                              &value.aiActivityHistoryLimit, 50, 2000);
            changed |= CheckboxRow(grid, "Show AI status in the header", "##AiInHeader",
                                   &value.showAiActivityInTitleBar);
            changed |= IntRow(grid, "UI auto refresh (ms)", "##AutoRefresh", &value.autoRefreshMs, 100, 10000);
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
