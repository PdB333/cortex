// Desktop UI: header, command palette, Go To, process picker, prompt
// surface, background progress and the dock host.

#include "desktop_app.h"
#include "ui/address_resolver.h"

#include <tlhelp32.h>

namespace cortex::desktop {

std::string Trim(std::string value) {
    auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

bool ParseAddressToken(const std::string& text, uint64_t& value, int defaultBase) {
    const std::string token = Trim(text);
    if (token.empty()) return false;
    int base = defaultBase;
    if (token.size() > 2 && token[0] == '0' && (token[1] == 'x' || token[1] == 'X')) {
        base = 0;
    } else if (base == 0) {
        const bool hasHexAlpha = std::any_of(token.begin(), token.end(), [](unsigned char ch) {
            ch = static_cast<unsigned char>(std::tolower(ch));
            return ch >= 'a' && ch <= 'f';
        });
        if (hasHexAlpha) base = 16;
    }

    try {
        size_t used = 0;
        const unsigned long long parsed = std::stoull(token, &used, base);
        if (used != token.size()) return false;
        value = static_cast<uint64_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

bool ResolveAddressExpression(AppState& app, const char* expression,
                              uint64_t& address, std::string& error) {
    error.clear();
    const std::string value = Trim(expression ? expression : "");
    if (value.empty()) {
        error = "Enter an address or module+offset";
        return false;
    }
    if (ParseAddressToken(value, address)) return true;

    // Cheat Engine address expressions: module+offset, [pointer]+offset,
    // module.Export, user-defined symbols...
    std::string expressionError;
    if (cortex::ui::EvaluateContextAddress(app.ui, value, address, &expressionError)) return true;

    if (app.projectModel.Refresh(nullptr)) {
        std::string projectExpression;
        if (app.projectModel.FindAddress(value, projectExpression) &&
            projectExpression != value) {
            if (ResolveAddressExpression(app, projectExpression.c_str(), address, error))
                return true;
        }
        for (const auto& path : app.projectModel.PointerPaths()) {
            if (path.name != value) continue;
            if (app.projectModel.ResolvePointerPath(path.name, projectExpression, nullptr) &&
                projectExpression != value &&
                ResolveAddressExpression(app, projectExpression.c_str(), address, error))
                return true;
        }
    }

    if (app.symbolsModel.Lookup(value, nullptr)) {
        const auto& symbol = app.symbolsModel.Result();
        if (symbol.found && !symbol.address.empty() && symbol.address != value &&
            ResolveAddressExpression(app, symbol.address.c_str(), address, error))
            return true;
    }

    error = expressionError.empty() ? "Address, module, project or symbol not found" : expressionError;
    return false;
}

void NavigateTo(AppState& app, uint64_t address, const char* workspace) {
    app.ui.NavigateTo(workspace ? workspace : "memory-browser", address);
}

// Resumes a process Cortex suspended with Pause target.
void ReleasePause(AppState& app, uint64_t pid) {
    if (app.pausedPids.erase(pid) > 0) cortex::process_control::Resume(pid);
}

void ResetTargetState(AppState& app) {
    if (const auto session = app.sessions.Active()) {
        ReleasePause(app, session->Target().processId);
        app.autoAttachSkipPid = session->Target().processId;
    }
    app.debuggerModel.Reset();
    app.projectModel.Reset();
    app.symbolsModel.Reset();
    app.structuresModel.Reset();
    app.pointerMapsModel.Reset();
    app.snapshotsModel.Reset();
    app.reModel.Reset();
    app.instrumentationModel.Reset();
    app.watchesModel.Reset();
    app.actionsModel.Reset();
    app.networkModel.Reset();
    app.diagnosticsModel.Reset();
    app.scriptsModel.Reset();
    app.inputModel.Reset();
    app.screenshotModel.Reset();
    app.runtimeEventsModel.Reset();
    app.patchesModel.Reset();
    app.promptModel.Reset();
    app.sessions.Detach();
    app.payload.Reset();
    app.ui.mutationAllowed = false;
    app.ui.ResetNavigation();
    app.ui.requestWorkspace = "memory";
    app.ui.status = "Detached";
}

enum class CommandAction {
    PresetMemory,
    PresetDebug,
    PresetRE,
    PresetTrace,
    PresetAutomation,
    PresetRuntime,
    SelectProcess,
    Detach,
    ToggleWrites,
    EnableRuntime,
    ViewOverview,
    ViewAddresses,
    ViewMemory,
    ViewMemoryBrowser,
    ViewDisassembly,
    ViewModules,
    ViewProject,
    ViewSymbols,
    ViewStructures,
    ViewPointerMaps,
    ViewSnapshots,
    ViewReWorkspace,
    ViewInstrumentation,
    ViewWatches,
    ViewActions,
    ViewNetwork,
    ViewDiagnostics,
    ViewScripts,
    ViewInput,
    ViewScreenshots,
    ViewDebugger,
    ViewTrace,
    ViewAdvanced,
    ViewSessions,
    ViewSettings,
    NavigateBack,
    NavigateForward,
    GoTo,
    Dispatch
};

struct CommandEntry {
    const char* label;
    CommandAction action;
    const char* command = nullptr;  // for Dispatch
};

constexpr CommandEntry kCommands[] = {
    {"Workspace: Memory", CommandAction::PresetMemory},
    {"Workspace: Debug", CommandAction::PresetDebug},
    {"Workspace: RE", CommandAction::PresetRE},
    {"Workspace: Trace", CommandAction::PresetTrace},
    {"Workspace: Automation", CommandAction::PresetAutomation},
    {"Workspace: Runtime", CommandAction::PresetRuntime},
    {"Target: Select process", CommandAction::SelectProcess},
    {"Target: Detach active target", CommandAction::Detach},
    {"Safety: Toggle write permission", CommandAction::ToggleWrites},
    {"Runtime: Enable instrumentation", CommandAction::EnableRuntime},
    {"View: Addresses", CommandAction::ViewAddresses},
    {"View: Memory scanner", CommandAction::ViewMemory},
    {"View: Memory viewer", CommandAction::ViewMemoryBrowser},
    {"View: Memory tools", CommandAction::Dispatch, "view_tools"},
    {"View: Disassembler", CommandAction::ViewDisassembly},
    {"View: Modules", CommandAction::ViewModules},
    {"View: Project", CommandAction::ViewProject},
    {"View: Symbols", CommandAction::ViewSymbols},
    {"View: Structures", CommandAction::ViewStructures},
    {"View: Pointer Maps", CommandAction::ViewPointerMaps},
    {"View: Snapshots", CommandAction::ViewSnapshots},
    {"View: Reverse Engineering", CommandAction::ViewReWorkspace},
    {"View: Instrumentation", CommandAction::ViewInstrumentation},
    {"View: Watches & Freezes", CommandAction::ViewWatches},
    {"View: Actions journal", CommandAction::ViewActions},
    {"View: Network", CommandAction::ViewNetwork},
    {"View: Diagnostics", CommandAction::ViewDiagnostics},
    {"View: Lua Scripts", CommandAction::ViewScripts},
    {"View: Input", CommandAction::ViewInput},
    {"View: Screenshots", CommandAction::ViewScreenshots},
    {"View: Debugger", CommandAction::ViewDebugger},
    {"View: Trace", CommandAction::ViewTrace},
    {"View: Advanced runtime", CommandAction::ViewAdvanced},
    {"View: Sessions", CommandAction::ViewSessions},
    {"View: Settings", CommandAction::ViewSettings},
    {"Navigate: Back", CommandAction::NavigateBack},
    {"Navigate: Forward", CommandAction::NavigateForward},
    {"Navigate: Go to address", CommandAction::GoTo},
    {"Target: Pause / resume the process", CommandAction::Dispatch, "pause_target"},
    {"Target: Attach to the foreground process", CommandAction::Dispatch, "attach_foreground"},
    {"Scan: Next scan", CommandAction::Dispatch, "scan_next"},
    {"Scan: Next scan, increased value", CommandAction::Dispatch, "scan_increased"},
    {"Scan: Next scan, decreased value", CommandAction::Dispatch, "scan_decreased"},
    {"Scan: Next scan, changed value", CommandAction::Dispatch, "scan_changed"},
    {"Scan: Next scan, unchanged value", CommandAction::Dispatch, "scan_unchanged"},
    {"Scan: Undo the last scan", CommandAction::Dispatch, "scan_undo"},
    {"Scan: Cancel the running scan", CommandAction::Dispatch, "scan_cancel"},
    {"Address list: Freeze / unfreeze all", CommandAction::Dispatch, "freeze_toggle_all"}
};

bool CommandMatches(const char* label, const char* filter) {
    if (!filter || !*filter) return true;
    return Lower(label ? label : "").find(Lower(filter)) != std::string::npos;
}

void ExecuteCommand(AppState& app, CommandAction action, const char* command = nullptr) {
    using cortex::ui::WorkspacePreset;
    switch (action) {
        case CommandAction::Dispatch:
            if (command) DispatchCommand(app, command);
            break;
        case CommandAction::PresetMemory: app.workspaces.ApplyPreset(WorkspacePreset::Memory); break;
        case CommandAction::PresetDebug: app.workspaces.ApplyPreset(WorkspacePreset::Debug); break;
        case CommandAction::PresetRE: app.workspaces.ApplyPreset(WorkspacePreset::ReverseEngineering); break;
        case CommandAction::PresetTrace: app.workspaces.ApplyPreset(WorkspacePreset::Trace); break;
        case CommandAction::PresetAutomation: app.workspaces.ApplyPreset(WorkspacePreset::Automation); break;
        case CommandAction::PresetRuntime: app.workspaces.ApplyPreset(WorkspacePreset::Runtime); break;
        case CommandAction::SelectProcess:
            app.ui.requestProcessPicker = true;
            break;
        case CommandAction::Detach:
            if (app.sessions.Active()) ResetTargetState(app);
            break;
        case CommandAction::ToggleWrites:
            if (app.sessions.Active()) app.ui.mutationAllowed = !app.ui.mutationAllowed;
            break;
        case CommandAction::EnableRuntime: {
            if (!app.sessions.Active()) {
                app.ui.status = "Select a process first";
                break;
            }
            if (!app.ui.mutationAllowed) {
                app.ui.status = "Enable writes before runtime injection";
                break;
            }
            std::string error;
            if (app.payload.EnsureReady(&error))
                app.ui.status = "Cortex runtime enabled";
            else
                app.ui.status = "Runtime enable failed: " + error;
            break;
        }
        case CommandAction::ViewOverview: app.workspaces.Select("overview"); break;
        case CommandAction::ViewAddresses: app.workspaces.Select("addresses"); break;
        case CommandAction::ViewMemory: app.workspaces.Select("memory"); break;
        case CommandAction::ViewMemoryBrowser: app.workspaces.Select("memory-browser"); break;
        case CommandAction::ViewDisassembly: app.workspaces.Select("disassembly"); break;
        case CommandAction::ViewModules: app.workspaces.Select("modules"); break;
        case CommandAction::ViewProject: app.workspaces.Select("project"); break;
        case CommandAction::ViewSymbols: app.workspaces.Select("symbols"); break;
        case CommandAction::ViewStructures: app.workspaces.Select("structures"); break;
        case CommandAction::ViewPointerMaps: app.workspaces.Select("pointermaps"); break;
        case CommandAction::ViewSnapshots: app.workspaces.Select("snapshots"); break;
        case CommandAction::ViewReWorkspace: app.workspaces.Select("re"); break;
        case CommandAction::ViewInstrumentation: app.workspaces.Select("instrumentation"); break;
        case CommandAction::ViewWatches: app.workspaces.Select("watches"); break;
        case CommandAction::ViewActions: app.workspaces.Select("actions"); break;
        case CommandAction::ViewNetwork: app.workspaces.Select("network"); break;
        case CommandAction::ViewDiagnostics: app.workspaces.Select("diagnostics"); break;
        case CommandAction::ViewScripts: app.workspaces.Select("scripts"); break;
        case CommandAction::ViewInput: app.workspaces.Select("input"); break;
        case CommandAction::ViewScreenshots: app.workspaces.Select("screenshots"); break;
        case CommandAction::ViewDebugger: app.workspaces.Select("debugger"); break;
        case CommandAction::ViewTrace: app.workspaces.Select("trace"); break;
        case CommandAction::ViewAdvanced: app.workspaces.Select("runtime"); break;
        case CommandAction::ViewSessions: app.workspaces.Select("sessions"); break;
        case CommandAction::ViewSettings: app.workspaces.Select("settings"); break;
        case CommandAction::NavigateBack:
            if (!app.ui.NavigateBack()) app.ui.status = "No previous address";
            break;
        case CommandAction::NavigateForward:
            if (!app.ui.NavigateForward()) app.ui.status = "No next address";
            break;
        case CommandAction::GoTo:
            app.requestGoTo = true;
            break;
    }
}

void HandleGlobalShortcuts(AppState& app) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return;

    if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_P, false)) {
        app.requestCommandPalette = true;
    } else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_K, false)) {
        app.requestCommandPalette = true;
    }

    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_G, false) && app.sessions.Active())
        app.requestGoTo = true;

    if (io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false) && app.ui.CanNavigateBack())
        app.ui.NavigateBack();
    if (io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_RightArrow, false) && app.ui.CanNavigateForward())
        app.ui.NavigateForward();
}

void DrawCommandPalette(AppState& app) {
    if (app.requestCommandPalette) {
        app.commandFilter[0] = '\0';
        ImGui::OpenPopup("Command palette");
        app.requestCommandPalette = false;
    }

    ImGui::SetNextWindowSize(ImVec2(cortex::ui::Px(640), cortex::ui::Px(460)), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Command palette", nullptr,
                                ImGuiWindowFlags_NoSavedSettings)) return;

    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1);
    const bool enter = ImGui::InputTextWithHint(
        "##CommandFilter", "Type a Cortex command...",
        app.commandFilter, sizeof(app.commandFilter),
        ImGuiInputTextFlags_EnterReturnsTrue);

    const CommandEntry* firstMatch = nullptr;
    ImGui::Separator();
    ImGui::BeginChild("CommandResults", ImVec2(0, -38), ImGuiChildFlags_None);
    for (const auto& command : kCommands) {
        if (!CommandMatches(command.label, app.commandFilter)) continue;
        if (!firstMatch) firstMatch = &command;
        if (ImGui::Selectable(command.label)) {
            ExecuteCommand(app, command.action, command.command);
            ImGui::CloseCurrentPopup();
            break;
        }
    }
    ImGui::EndChild();

    if (enter && firstMatch) {
        ExecuteCommand(app, firstMatch->action, firstMatch->command);
        ImGui::CloseCurrentPopup();
    }

    if (ImGui::Button("Close", ImVec2(cortex::ui::Px(100), cortex::ui::Px(28)))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void DrawGoTo(AppState& app) {
    if (app.requestGoTo) {
        ImGui::OpenPopup("Go to");
        app.requestGoTo = false;
    }

    ImGui::SetNextWindowSize(ImVec2(cortex::ui::Px(520), cortex::ui::Px(190)), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Go to", nullptr,
                                ImGuiWindowFlags_NoSavedSettings)) return;

    ImGui::TextUnformatted("Address or module+offset");
    ImGui::TextDisabled("Examples: 0x140001000   game.exe+1F234");
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1);
    const bool enter = ImGui::InputTextWithHint(
        "##GoToExpression", "0x... or module+offset",
        app.goToExpression, sizeof(app.goToExpression),
        ImGuiInputTextFlags_EnterReturnsTrue);

    auto go = [&](const char* workspace) {
        uint64_t address = 0;
        std::string error;
        if (!ResolveAddressExpression(app, app.goToExpression, address, error)) {
            app.ui.status = "Go To: " + error;
            return false;
        }
        NavigateTo(app, address, workspace);
        app.ui.status = "Navigated to " + std::string(app.goToExpression);
        return true;
    };

    if ((ImGui::Button("Memory", ImVec2(cortex::ui::Px(130), cortex::ui::Px(32))) || enter) && go("memory-browser"))
        ImGui::CloseCurrentPopup();
    ImGui::SameLine();
    if (ImGui::Button("Disassembly", ImVec2(cortex::ui::Px(130), cortex::ui::Px(32))) && go("disassembly"))
        ImGui::CloseCurrentPopup();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(cortex::ui::Px(100), cortex::ui::Px(32)))) ImGui::CloseCurrentPopup();

    ImGui::EndPopup();
}

bool MatchesFilter(const cortex::target::TargetDescriptor& target, const char* filter) {
    if (!filter || !*filter) return true;
    const std::string query = Lower(filter);
    return Lower(target.name).find(query) != std::string::npos ||
           Lower(target.windowTitle).find(query) != std::string::npos ||
           std::to_string(target.processId).find(query) != std::string::npos;
}

void AttachTarget(AppState& app, const cortex::target::TargetDescriptor& target) {
    std::string error;
    if (app.sessions.Attach(target, &error)) {
        app.OnAttached(target);
        ImGui::CloseCurrentPopup();
    } else {
        app.ui.status = "Attach failed: " + error;
    }
}

void DrawProcessPicker(AppState& app) {
    if (app.ui.requestProcessPicker) {
        app.RefreshTargets();
        ImGui::OpenPopup("Select process");
        app.ui.requestProcessPicker = false;
    }

    ImGui::SetNextWindowSize(ImVec2(cortex::ui::Px(760), cortex::ui::Px(580)), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Select process", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Choose a process");
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Double-click to attach");
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) app.RefreshTargets();
    ImGui::Spacing();

    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##ProcessFilter", "Search process, window title or PID...",
                             app.processFilter, sizeof(app.processFilter));
    ImGui::Spacing();

    const float footerHeight = 54.0f;
    if (cortex::ui::BeginDataTable("ProcessTable", 5,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                          ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
                          ImVec2(0, -footerHeight))) {
        ImGui::TableSetupColumn("Process", ImGuiTableColumnFlags_WidthStretch, 0.42f);
        ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, cortex::ui::Px(78.0f));
        ImGui::TableSetupColumn("Arch", ImGuiTableColumnFlags_WidthFixed, cortex::ui::Px(72.0f));
        ImGui::TableSetupColumn("Window", ImGuiTableColumnFlags_WidthStretch, 0.50f);
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, cortex::ui::Px(82.0f));
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < app.targets.size(); ++i) {
            const auto& target = app.targets[i];
            if (!MatchesFilter(target, app.processFilter)) continue;

            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const bool selected = app.selectedTarget == static_cast<int>(i);
            if (ImGui::Selectable(target.name.c_str(), selected,
                                  ImGuiSelectableFlags_SpanAllColumns |
                                  ImGuiSelectableFlags_AllowDoubleClick)) {
                app.selectedTarget = static_cast<int>(i);
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) AttachTarget(app, target);
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%llu", static_cast<unsigned long long>(target.processId));
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(cortex::target::ArchitectureName(target.architecture));
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(target.windowTitle.empty() ? "-" : target.windowTitle.c_str());
            ImGui::TableSetColumnIndex(4);
            if (app.sessions.ActiveTargetId() == target.id)
                ImGui::TextUnformatted("ACTIVE");
            else if (app.sessions.HasSession(target.id))
                ImGui::TextDisabled("attached");
            else
                ImGui::TextDisabled("-");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    const bool validSelection =
        app.selectedTarget >= 0 && app.selectedTarget < static_cast<int>(app.targets.size());

    ImGui::BeginDisabled(!validSelection);
    if (ImGui::Button("Attach", ImVec2(cortex::ui::Px(150), cortex::ui::Px(38))) && validSelection) {
        AttachTarget(app, app.targets[static_cast<size_t>(app.selectedTarget)]);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(cortex::ui::Px(110), cortex::ui::Px(38))) ||
        (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
         ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void DrawPromptSurface(AppState& app) {
    app.promptModel.Poll();

    if (app.promptModel.Active() &&
        app.promptAnswerId != app.promptModel.Id()) {
        app.promptAnswerId = app.promptModel.Id();
        std::memset(app.promptAnswer, 0, sizeof(app.promptAnswer));
        ImGui::OpenPopup("Human prompt");
    }

    ImGui::SetNextWindowSize(ImVec2(cortex::ui::Px(560), 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(
            "Human prompt", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoSavedSettings))
        return;

    if (!app.promptModel.Active()) {
        app.promptAnswerId = -1;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    const bool timedTest = app.promptModel.Kind() == "timed_test";
    const bool answerReady = !timedTest || app.promptModel.RemainingMs() <= 0;

    ImGui::TextUnformatted(timedTest
        ? "Human test requested"
        : "Human action requested");
    ImGui::SameLine();
    ImGui::TextDisabled("Prompt #%d", app.promptModel.Id());
    ImGui::Separator();

    if (timedTest) {
        ImGui::TextWrapped("%s", app.promptModel.Message().c_str());
    } else {
        ImGui::TextUnformatted(app.promptModel.Label().c_str());
        ImGui::Spacing();
        ImGui::TextDisabled("%s",
            app.promptModel.CurrentValue().empty()
                ? "Current value not provided"
                : app.promptModel.CurrentValue().c_str());
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("->");
        ImGui::SameLine();
        ImGui::Text("%s", app.promptModel.TargetValue().c_str());
    }

    if (timedTest) {
        ImGui::Spacing();
        if (!answerReady) {
            const int64_t seconds =
                std::max<int64_t>(1, (app.promptModel.RemainingMs() + 999) / 1000);
            ImGui::Text("Answer unlocks in %llds",
                        static_cast<long long>(seconds));
        }

        ImGui::BeginDisabled(!answerReady);
        ImGuiInputTextFlags inputFlags = ImGuiInputTextFlags_EnterReturnsTrue;
        if (app.promptModel.AnswerType() == "number")
            inputFlags |= ImGuiInputTextFlags_CharsScientific;
        const bool submittedByEnter = ImGui::InputTextWithHint(
            "##PromptAnswer",
            app.promptModel.AnswerType() == "number"
                ? "Enter the measured number"
                : "Enter the result",
            app.promptAnswer, sizeof(app.promptAnswer), inputFlags);
        ImGui::EndDisabled();

        if (submittedByEnter && answerReady && app.promptAnswer[0] != '\0') {
            std::string error;
            if (app.promptModel.Answer(app.promptAnswer, &error)) {
                app.promptAnswerId = -1;
                ImGui::CloseCurrentPopup();
            } else {
                app.ui.status = "Prompt answer failed: " + error;
            }
        }
    }

    if (!app.promptModel.LastError().empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("Error: %s", app.promptModel.LastError().c_str());
    }

    ImGui::Spacing();
    const bool canSubmit =
        !timedTest || (answerReady && app.promptAnswer[0] != '\0');
    ImGui::BeginDisabled(!canSubmit);
    if (ImGui::Button(timedTest ? "Submit result" : "Done", ImVec2(cortex::ui::Px(130), cortex::ui::Px(34)))) {
        std::string error;
        const std::string answer = timedTest ? app.promptAnswer : "ack";
        if (app.promptModel.Answer(answer, &error)) {
            app.promptAnswerId = -1;
            ImGui::CloseCurrentPopup();
        } else {
            app.ui.status = "Prompt answer failed: " + error;
        }
    }
    ImGui::EndDisabled();

    ImGui::EndPopup();
}

void ActivateAttachedTarget(
        AppState& app, const cortex::target::TargetDescriptor& target) {
    if (app.sessions.ActiveTargetId() == target.id) return;
    if (!app.sessions.Activate(target.id)) {
        app.ui.status = "Could not activate " + target.name;
        return;
    }
    app.OnAttached(target, false);
    app.ui.status = "Active target: " + target.name +
                    " (PID " + std::to_string(target.processId) + ")";
}

constexpr ImVec4 kWriteAccent(0.93f, 0.69f, 0.29f, 1.0f);

void DetachTarget(AppState& app, const cortex::target::TargetDescriptor& target, bool wasActive) {
    ReleasePause(app, target.processId);
    app.autoAttachSkipPid = target.processId;
    app.sessions.Detach(target.id);
    if (!wasActive) return;
    const auto remaining = app.sessions.AttachedTargets();
    if (!remaining.empty()) {
        app.sessions.Activate(remaining.front().id);
        app.OnAttached(remaining.front(), false);
    } else {
        ResetTargetState(app);
    }
}

// Attaches (or switches back) to a process id.
bool AttachProcessId(AppState& app, uint64_t pid, const std::string& verb) {
    for (const auto& target : app.sessions.AttachedTargets()) {
        if (target.processId != pid) continue;
        ActivateAttachedTarget(app, target);
        return true;
    }
    app.RefreshTargets();
    for (const auto& target : app.targets) {
        if (target.processId != pid) continue;
        std::string error;
        if (!app.sessions.Attach(target, &error)) {
            app.ui.status = "Attach failed: " + error;
            return false;
        }
        app.OnAttached(target);
        app.ui.status = verb + target.name + " (PID " + std::to_string(pid) + ")";
        return true;
    }
    app.ui.status = "Process " + std::to_string(pid) + " was not found";
    return false;
}

void TogglePauseTarget(AppState& app) {
    const auto session = app.sessions.Active();
    if (!session) {
        app.ui.status = "Select a process first";
        return;
    }
    const uint64_t pid = session->Target().processId;
    std::string error;
    if (app.pausedPids.count(pid)) {
        if (!cortex::process_control::Resume(pid, &error)) {
            app.ui.status = "Resume failed: " + error;
            return;
        }
        app.pausedPids.erase(pid);
        app.ui.status = "Target resumed";
        return;
    }
    if (!app.ui.mutationAllowed) {
        app.ui.status = "Allow writes to pause the target";
        return;
    }
    if (!cortex::process_control::Suspend(pid, &error)) {
        app.ui.status = "Pause failed: " + error;
        return;
    }
    app.pausedPids.insert(pid);
    app.ui.status = "Target paused: every thread is suspended until Resume target";
}

void AttachForegroundProcess(AppState& app) {
    if (app.lastForegroundPid == 0) {
        app.ui.status = "Bring the target window to the front first, then use the hotkey";
        return;
    }
    AttachProcessId(app, app.lastForegroundPid, "Attached to the foreground process ");
}

void TrackForegroundProcess(AppState& app) {
    HWND foreground = GetForegroundWindow();
    if (!foreground) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(foreground, &pid);
    if (pid != 0 && pid != GetCurrentProcessId()) app.lastForegroundPid = pid;
}

std::string NarrowName(const wchar_t* text) {
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) return {};
    std::string result(static_cast<size_t>(bytes - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), bytes, nullptr, nullptr);
    return result;
}

// Settings > Process attach: attach as soon as a listed process runs.
void AutoAttach(AppState& app) {
    const auto& values = app.settings.Values();
    if (!values.autoAttachEnabled || app.sessions.Active()) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - app.lastAutoAttach < std::chrono::seconds(2)) return;
    app.lastAutoAttach = now;

    std::vector<std::string> names;
    std::string current;
    for (const char ch : values.autoAttachProcesses + ",") {
        if (ch == ',' || ch == ';') {
            current = Lower(Trim(current));
            if (!current.empty()) {
                names.push_back(current);
                if (current.size() < 4 || current.compare(current.size() - 4, 4, ".exe") != 0)
                    names.push_back(current + ".exe");
            }
            current.clear();
        } else {
            current += ch;
        }
    }
    if (names.empty()) return;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    uint64_t match = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == GetCurrentProcessId() || entry.th32ProcessID == app.autoAttachSkipPid) continue;
            const std::string name = Lower(NarrowName(entry.szExeFile));
            if (std::find(names.begin(), names.end(), name) != names.end()) {
                match = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    if (match) AttachProcessId(app, match, "Auto-attached to ");
}

void DispatchCommand(AppState& app, const std::string& command) {
    if (command == "pause_target") {
        TogglePauseTarget(app);
    } else if (command == "attach_foreground") {
        AttachForegroundProcess(app);
    } else if (command == "show_cortex") {
        HWND window = static_cast<HWND>(app.window);
        if (!window) return;
        if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
        SetForegroundWindow(window);
    } else if (command == "view_tools") {
        app.workspaces.Select("tools");
    } else if (!command.empty()) {
        app.ui.commands.push_back(command);
    }
}

// Session chips only earn their space once more than one target is attached;
// the active target is already named in the first header row.
void DrawSessionChips(AppState& app) {
    const auto attached = app.sessions.AttachedTargets();
    if (attached.size() < 2) return;

    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::TextDisabled("Sessions");
    for (const auto& target : attached) {
        ImGui::SameLine();
        ImGui::PushID(target.id.c_str());
        const bool active = app.sessions.ActiveTargetId() == target.id;
        if (active)
            ImGui::PushStyleColor(
                ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));

        const std::string label =
            target.name + " [" + std::to_string(target.processId) + "]";
        if (ImGui::Button(label.c_str()))
            ActivateAttachedTarget(app, target);

        if (active) ImGui::PopStyleColor();

        if (ImGui::BeginPopupContextItem("SessionMenu")) {
            if (!active && ImGui::MenuItem("Activate"))
                ActivateAttachedTarget(app, target);
            if (ImGui::MenuItem("Detach"))
                DetachTarget(app, target, active);
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
}

void DrawDebugControls(AppState& app) {
    if (!app.sessions.Active() || !app.debuggerModel.Ready()) return;

    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::BeginDisabled(!app.ui.mutationAllowed ||
                         app.debuggerModel.CurrentThread() == 0);
    std::string error;
    if (ImGui::Button("Pause") && !app.debuggerModel.Pause(&error))
        app.ui.status = "Pause failed: " + error;
    ImGui::SameLine();
    if (ImGui::Button("Continue") && !app.debuggerModel.Resume(&error))
        app.ui.status = "Continue failed: " + error;
    ImGui::SameLine();
    if (ImGui::Button("Step") && !app.debuggerModel.Step(2000, &error))
        app.ui.status = "Step failed: " + error;
    ImGui::SameLine();
    if (ImGui::Button("Over") && !app.debuggerModel.StepOver(5000, &error))
        app.ui.status = "Step over failed: " + error;
    ImGui::EndDisabled();
}

std::string AiStatusText(const AppState& app) {
    if (!app.settings.Values().showAiActivityInTitleBar) return {};
    if (app.aiActivityModel.Connected()) {
        return "AI " + std::to_string(app.aiActivityModel.SessionCount()) + " session(s) / " +
               std::to_string(app.aiActivityModel.ActiveTaskCount()) + " active";
    }
    return app.aiActivityModel.Listening() ? "AI idle" : "AI listener unavailable";
}

// The write gate is the app's main safety control, so it must read clearly in
// both states: an outlined "Read-only" box, or an amber "Writes allowed".
void DrawWriteGate(AppState& app) {
    const bool on = app.ui.mutationAllowed;
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, on ? kWriteAccent : ImVec4(0.42f, 0.47f, 0.53f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_CheckMark, kWriteAccent);
    if (on) ImGui::PushStyleColor(ImGuiCol_Text, kWriteAccent);
    ImGui::Checkbox(on ? "Writes allowed###AllowWrites" : "Read-only###AllowWrites",
                    &app.ui.mutationAllowed);
    if (on) ImGui::PopStyleColor();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Allow writes: required for edits, freeze and runtime injection.\n"
                          "Read-only inspection stays available either way.");
    }
}

float WriteGateWidth(const AppState& app) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const char* label = app.ui.mutationAllowed ? "Writes allowed" : "Read-only";
    return ImGui::GetFrameHeight() + style.ItemInnerSpacing.x + ImGui::CalcTextSize(label).x;
}

// Two fixed rows. Nothing is inserted or removed when a target attaches, so
// controls never move under the cursor.
void DrawHeader(AppState& app) {
    const auto session = app.sessions.Active();
    const ImGuiStyle& style = ImGui::GetStyle();

    // Row 1: identity, target, and the right-aligned safety/AI cluster.
    ImGui::AlignTextToFramePadding();
    {
        cortex::ui::HeadingFont heading;
        ImGui::TextUnformatted("CORTEX");
    }
    ImGui::SameLine(0.0f, style.ItemSpacing.x * 3.0f);
    if (ImGui::Button(session ? "Change process" : "Select process"))
        app.ui.requestProcessPicker = true;

    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    if (session) {
        const auto& target = session->Target();
        ImGui::Text("%s  |  PID %llu  |  %s",
                    target.name.c_str(),
                    static_cast<unsigned long long>(target.processId),
                    cortex::target::ArchitectureName(target.architecture));
        if (app.ui.targetPaused) {
            ImGui::SameLine();
            ImGui::TextColored(kWriteAccent, "PAUSED");
        }
        if (!app.payload.Ready()) {
            std::string runtimeReason;
            if (!app.payload.RuntimeSupportAvailable(&runtimeReason)) {
                ImGui::SameLine();
                ImGui::TextDisabled("|  runtime: %s",
                    cortex::ui::RuntimeSupportText(runtimeReason).c_str());
            }
        }
    } else {
        ImGui::TextDisabled("No process attached");
    }

    const std::string aiText = AiStatusText(app);
    float clusterWidth = 0.0f;
    if (!aiText.empty()) clusterWidth += ImGui::CalcTextSize(aiText.c_str()).x + style.ItemSpacing.x * 2.0f;
    if (session) {
        clusterWidth += WriteGateWidth(app) + style.ItemSpacing.x;
        clusterWidth += ImGui::CalcTextSize("Detach").x + style.FramePadding.x * 2.0f;
    }
    const float rightEdge = ImGui::GetWindowContentRegionMax().x;
    ImGui::SameLine();
    const float clusterStart = rightEdge - clusterWidth;
    if (clusterStart > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(clusterStart);

    if (!aiText.empty()) {
        ImGui::AlignTextToFramePadding();
        if (app.aiActivityModel.Connected()) ImGui::TextUnformatted(aiText.c_str());
        else ImGui::TextDisabled("%s", aiText.c_str());
        if (session) ImGui::SameLine(0.0f, style.ItemSpacing.x * 2.0f);
    }
    if (session) {
        DrawWriteGate(app);
        ImGui::SameLine();
        if (ImGui::Button("Detach")) ResetTargetState(app);
    }

    // Row 2: layouts, navigation, debugger control and extra sessions.
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Workspace");
    ImGui::SameLine();
    app.workspaces.DrawPresetButtons();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::BeginDisabled(!session);
    if (ImGui::Button("Go to...")) app.requestGoTo = true;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Go to an address, module offset or symbol (Ctrl+G)");
    ImGui::SameLine();
    const bool paused = app.ui.targetPaused;
    if (paused) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.38f, 0.10f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.66f, 0.46f, 0.13f, 1.0f));
    }
    if (ImGui::Button(paused ? "Resume target###PauseTarget" : "Pause target###PauseTarget"))
        TogglePauseTarget(app);
    if (paused) ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(paused ? "Every thread of the target is suspended. Resume it here."
                                 : "Suspend every thread of the target (needs Writes allowed).\n"
                                   "A global hotkey can be set in Settings > Hotkeys.");
    ImGui::EndDisabled();
    DrawDebugControls(app);
    DrawSessionChips(app);
    ImGui::Separator();
}

// Drawn instead of the workspace UI while a background task runs. It reads
// nothing the task may be writing (models, status), only the task's own label
// and start time, and keeps the dock space alive so the layout is unchanged
// when the workspaces come back.
void DrawBackgroundProgress(AppState& app) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("CortexDockHost", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                 ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_MenuBar);
    ImGui::PopStyleVar(2);
    if (ImGui::BeginMenuBar()) ImGui::EndMenuBar();

    const ImGuiID dockspaceId = ImGui::GetID("CortexDockSpace");
    ImGui::DockSpace(dockspaceId, ImVec2(0, 0), ImGuiDockNodeFlags_KeepAliveOnly);

    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - app.background.started).count();
    static const char* kDots[] = {"", ".", "..", "..."};
    const ImVec2 cardSize(cortex::ui::Px(460), cortex::ui::Px(120));
    const ImVec2 region = ImGui::GetContentRegionAvail();
    ImGui::SetCursorPos(ImVec2(std::max(0.0f, (region.x - cardSize.x) * 0.5f),
                               std::max(0.0f, (region.y - cardSize.y) * 0.5f)));
    ImGui::BeginChild("BackgroundProgress", cardSize, ImGuiChildFlags_Borders);
    {
        cortex::ui::HeadingFont heading;
        ImGui::Text("%s%s", app.background.label.c_str(),
                    kDots[static_cast<int>(elapsed * 2.0) % 4]);
    }
    ImGui::Spacing();
    ImGui::TextDisabled("%.1f s", elapsed);
    cortex::ui::HintText("The window stays responsive; this finishes on its own.");
    ImGui::EndChild();
    ImGui::End();
}

void DrawApp(AppState& app) {
    if (app.background.Active()) {
        bool finished = false;
        try {
            finished = app.background.Finish();
        } catch (const std::exception& ex) {
            finished = true;
            app.ui.status = app.background.label + " failed: " + ex.what();
        } catch (...) {
            finished = true;
            app.ui.status = app.background.label + " failed";
        }
        if (!finished) {
            DrawBackgroundProgress(app);
            return;
        }
    }

    app.aiActivityModel.Poll(app.settings.Values().aiActivityHistoryLimit);
    TrackForegroundProcess(app);
    AutoAttach(app);
    {
        const auto session = app.sessions.Active();
        app.ui.targetPaused = session && app.pausedPids.count(session->Target().processId) > 0;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_MenuBar;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("CortexDockHost", nullptr, flags);
    ImGui::PopStyleVar(2);

    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Select process..."))
                app.ui.requestProcessPicker = true;
            if (ImGui::MenuItem("Attach to foreground process", nullptr, false, app.lastForegroundPid != 0))
                AttachForegroundProcess(app);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("The last window used before Cortex. Bind a global hotkey in\n"
                                  "Settings > Hotkeys to attach straight from the game.");
            const bool attached = static_cast<bool>(app.sessions.Active());
            ImGui::BeginDisabled(!attached);
            if (ImGui::MenuItem(app.ui.targetPaused ? "Resume target" : "Pause target"))
                TogglePauseTarget(app);
            if (ImGui::MenuItem("Detach active target")) {
                ResetTargetState(app);
            }
            ImGui::EndDisabled();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Command palette...", "Ctrl+Shift+P"))
                app.requestCommandPalette = true;
            if (ImGui::MenuItem("Go to...", "Ctrl+G", false,
                                static_cast<bool>(app.sessions.Active())))
                app.requestGoTo = true;
            ImGui::Separator();
            if (ImGui::MenuItem("Settings"))
                app.workspaces.Select("settings");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Debug")) {
            const bool attached = static_cast<bool>(app.sessions.Active());
            ImGui::BeginDisabled(!attached);
            if (ImGui::MenuItem("Debugger workspace"))
                app.workspaces.Select("debugger");
            if (ImGui::MenuItem("Breakpoints"))
                app.workspaces.Select("debugger");
            ImGui::Separator();
            if (ImGui::MenuItem("Attach debugger", nullptr, false,
                                !app.debuggerModel.Ready())) {
                std::string error;
                if (!app.debuggerModel.EnsureAttached(&error))
                    app.ui.status = "Debugger attach failed: " + error;
            }
            const bool canControl =
                app.debuggerModel.Ready() &&
                app.debuggerModel.CurrentThread() != 0 &&
                app.ui.mutationAllowed;
            ImGui::BeginDisabled(!canControl);
            if (ImGui::MenuItem("Pause")) {
                std::string error;
                if (!app.debuggerModel.Pause(&error))
                    app.ui.status = "Pause failed: " + error;
            }
            if (ImGui::MenuItem("Continue")) {
                std::string error;
                if (!app.debuggerModel.Resume(&error))
                    app.ui.status = "Continue failed: " + error;
            }
            if (ImGui::MenuItem("Step into")) {
                std::string error;
                if (!app.debuggerModel.Step(2000, &error))
                    app.ui.status = "Step failed: " + error;
            }
            if (ImGui::MenuItem("Step over")) {
                std::string error;
                if (!app.debuggerModel.StepOver(5000, &error))
                    app.ui.status = "Step over failed: " + error;
            }
            ImGui::EndDisabled();
            ImGui::EndDisabled();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Tools")) {
            if (ImGui::MenuItem("Memory viewer"))
                app.workspaces.Select("memory-browser");
            if (ImGui::MenuItem("Disassembler"))
                app.workspaces.Select("disassembly");
            if (ImGui::MenuItem("Memory tools (regions, PE, strings, caves)"))
                app.workspaces.Select("tools");
            if (ImGui::MenuItem("Pointer scan")) {
                app.ui.toolsTabRequest = "pointers";
                app.ui.NavigateTo("tools", 0);
            }
            if (ImGui::MenuItem("Modules"))
                app.workspaces.Select("modules");
            if (ImGui::MenuItem("Sessions"))
                app.workspaces.Select("sessions");
            if (ImGui::MenuItem("Runtime"))
                app.workspaces.Select("runtime");
            if (ImGui::MenuItem("Diagnostics"))
                app.workspaces.Select("diagnostics");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Workspace")) {
            app.workspaces.DrawPresetMenu();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            app.workspaces.DrawViewMenu();
            ImGui::Separator();
            if (ImGui::MenuItem("Back", "Alt+Left", false, app.ui.CanNavigateBack()))
                app.ui.NavigateBack();
            if (ImGui::MenuItem("Forward", "Alt+Right", false, app.ui.CanNavigateForward()))
                app.ui.NavigateForward();
            ImGui::Separator();
            if (ImGui::MenuItem("Command palette...", "Ctrl+Shift+P"))
                app.requestCommandPalette = true;
            if (ImGui::MenuItem("Go to...", "Ctrl+G", false,
                                static_cast<bool>(app.sessions.Active())))
                app.requestGoTo = true;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            ImGui::TextDisabled("Cortex v0.8.0-dev-imgui");
            ImGui::Separator();
            if (ImGui::MenuItem("Diagnostics"))
                app.workspaces.Select("diagnostics");
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    DrawHeader(app);

    const float statusHeight = ImGui::GetTextLineHeightWithSpacing() + 12.0f;
    ImVec2 dockSize = ImGui::GetContentRegionAvail();
    dockSize.y = std::max(1.0f, dockSize.y - statusHeight);

    const ImGuiID dockspaceId = ImGui::GetID("CortexDockSpace");
    app.workspaces.PrepareDockLayout(dockspaceId, dockSize);
    ImGui::DockSpace(dockspaceId, dockSize, ImGuiDockNodeFlags_None);

    ImGui::Separator();
    ImGui::TextDisabled("[%s]  %s",
                        cortex::ui::WorkspaceRegistry::PresetName(app.workspaces.Preset()),
                        app.ui.status.c_str());

    DrawProcessPicker(app);
    DrawCommandPalette(app);
    DrawGoTo(app);
    DrawPromptSurface(app);
    ImGui::End();

    app.workspaces.DrawDockWindows(app.ui);
}


}  // namespace cortex::desktop
