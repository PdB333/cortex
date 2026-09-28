#pragma once

// Shared state and entry points of the Cortex desktop application. The
// implementation is split by concern: desktop_ui.cpp draws the interface,
// cli.cpp handles command-line modes, smoke_modes.cpp holds the GUI test
// modes and main.cpp owns the window, Direct3D device and frame loop.

#include "application/actions_model.h"
#include "application/ai_activity_model.h"
#include "application/diagnostics_model.h"
#include "application/input_model.h"
#include "application/network_model.h"
#include "application/debugger_model.h"
#include "application/hotkeys.h"
#include "application/settings.h"
#include "application/project_model.h"
#include "application/patches_model.h"
#include "application/instrumentation_model.h"
#include "application/re_model.h"
#include "application/runtime_events_model.h"
#include "application/pointer_maps_model.h"
#include "application/prompt_model.h"
#include "application/snapshots_model.h"
#include "application/structures_model.h"
#include "application/symbols_model.h"
#include "application/watches_model.h"
#include "application/scripts_model.h"
#include "application/screenshot_model.h"
#include "ui/actions_workspace.h"
#include "ui/address_context_menu.h"
#include "ui/addresses_workspace.h"
#include "ui/bottom_panel_workspace.h"
#include "ui/diagnostics_workspace.h"
#include "ui/input_workspace.h"
#include "ui/network_workspace.h"
#include "ui/debugger_workspace.h"
#include "ui/disassembly_workspace.h"
#include "ui/events_workspace.h"
#include "ui/memory_browser_workspace.h"
#include "ui/memory_workspace.h"
#include "ui/modules_workspace.h"
#include "ui/overview_workspace.h"
#include "ui/patches_workspace.h"
#include "ui/instrumentation_workspace.h"
#include "ui/project_workspace.h"
#include "ui/re_workspace.h"
#include "ui/pointer_maps_workspace.h"
#include "ui/snapshots_workspace.h"
#include "ui/structures_workspace.h"
#include "ui/runtime_workspace.h"
#include "ui/sessions_workspace.h"
#include "ui/settings_workspace.h"
#include "ui/symbols_workspace.h"
#include "ui/trace_workspace.h"
#include "ui/watches_workspace.h"
#include "ui/scripts_workspace.h"
#include "ui/screenshot_workspace.h"
#include "services/user_data.h"
#include "ui/fonts.h"
#include "ui/theme.h"
#include "ui/widgets.h"
#include "ui/ui_context.h"
#include "ui/workspace_registry.h"
#include "services/debugger_service.h"
#include "services/disassembly_service.h"
#include "services/memory_service.h"
#include "services/module_service.h"
#include "services/payload_client.h"
#include "target/catalog.h"
#include "target/local_backend.h"
#include "target/session_manager.h"
#include "process/process_control.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <d3d11.h>
#include <shellapi.h>
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace cortex::desktop {

// Command line (cli.cpp).
std::vector<std::string> CurrentCommandLineArgs();
std::string ExecutableDirectory();
// Runs a command-line mode; returns -1 when the desktop UI should start.
int HandleCommandLine();

// The Direct3D 11 device of the desktop window (main.cpp), or null in
// headless modes.
void* NativeRenderDevice();

inline std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

// One long runtime operation running off the UI thread (see
// UiContext::RunInBackground). While it is active the frame loop keeps the
// window responsive and draws only a progress card.
struct BackgroundTask {
    std::future<void> future;
    std::string label;
    std::chrono::steady_clock::time_point started;

    bool Active() const { return future.valid(); }

    void Start(std::string taskLabel, std::function<void()> work) {
        label = std::move(taskLabel);
        started = std::chrono::steady_clock::now();
        future = std::async(std::launch::async, std::move(work));
    }

    // Returns true once the task has finished; rethrows its exception.
    bool Finish() {
        if (!Active()) return true;
        if (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return false;
        auto finished = std::move(future);
        finished.get();
        return true;
    }
};

struct AppState {
    cortex::target::Catalog catalog;
    cortex::target::SessionManager sessions;
    cortex::services::MemoryService memory;
    cortex::services::ModuleService modules;
    cortex::services::DisassemblyService disassembly;
    cortex::services::DebuggerService debugger;
    cortex::services::PayloadClient payload;
    cortex::application::SettingsStore settings;
    cortex::application::DebuggerModel debuggerModel;
    cortex::application::ProjectModel projectModel;
    cortex::application::SymbolsModel symbolsModel;
    cortex::application::StructuresModel structuresModel;
    cortex::application::PointerMapsModel pointerMapsModel;
    cortex::application::SnapshotsModel snapshotsModel;
    cortex::application::ReModel reModel;
    cortex::application::InstrumentationModel instrumentationModel;
    cortex::application::WatchesModel watchesModel;
    cortex::application::ActionsModel actionsModel;
    cortex::application::AiActivityModel aiActivityModel;
    cortex::application::NetworkModel networkModel;
    cortex::application::DiagnosticsModel diagnosticsModel;
    cortex::application::ScriptsModel scriptsModel;
    cortex::application::InputModel inputModel;
    cortex::application::ScreenshotModel screenshotModel;
    cortex::application::RuntimeEventsModel runtimeEventsModel;
    cortex::application::PatchesModel patchesModel;
    cortex::application::PromptModel promptModel;
    cortex::ui::UiContext ui;
    cortex::ui::WorkspaceRegistry workspaces;

    std::vector<cortex::target::TargetDescriptor> targets;
    int selectedTarget = -1;
    char processFilter[160] = {};
    bool requestCommandPalette = false;
    bool requestGoTo = false;
    char commandFilter[160] = {};
    char goToExpression[160] = {};
    char promptAnswer[512] = {};
    int promptAnswerId = -1;

    // Main window (main.cpp), for hotkeys and Bring Cortex to the front.
    void* window = nullptr;
    cortex::application::HotkeyRegistrar hotkeys;
    // Processes suspended with Pause target; resumed on detach and exit.
    std::set<uint64_t> pausedPids;
    // Last process other than Cortex that had the foreground window.
    uint64_t lastForegroundPid = 0;
    // Auto-attach skips the process the user detached from.
    uint64_t autoAttachSkipPid = 0;
    std::chrono::steady_clock::time_point lastAutoAttach{};

    // Declared last so it is destroyed first: its future waits for a running
    // task, which may still use the models above.
    BackgroundTask background;

    AppState()
        : sessions(catalog),
          memory(sessions),
          modules(sessions),
          disassembly(sessions),
          debugger(sessions),
          payload(sessions, ExecutableDirectory()),
          settings(ExecutableDirectory()),
          debuggerModel(sessions, payload, settings),
          projectModel(payload),
          symbolsModel(payload),
          structuresModel(payload),
          pointerMapsModel(payload),
          snapshotsModel(payload),
          reModel(payload),
          instrumentationModel(payload),
          watchesModel(payload),
          actionsModel(payload),
          networkModel(payload),
          diagnosticsModel(payload, ExecutableDirectory()),
          scriptsModel(payload),
          inputModel(payload),
          screenshotModel(payload),
          runtimeEventsModel(payload),
          patchesModel(payload),
          promptModel(payload) {
        catalog.AddBackend(std::make_shared<cortex::target::LocalBackend>());

        ui.sessions = &sessions;
        ui.memory = &memory;
        ui.modules = &modules;
        ui.disassembly = &disassembly;
        ui.debugger = &debugger;
        ui.payload = &payload;
        ui.settings = &settings;
        ui.debuggerModel = &debuggerModel;
        ui.projectModel = &projectModel;
        ui.symbolsModel = &symbolsModel;
        ui.structuresModel = &structuresModel;
        ui.pointerMapsModel = &pointerMapsModel;
        ui.snapshotsModel = &snapshotsModel;
        ui.reModel = &reModel;
        ui.instrumentationModel = &instrumentationModel;
        ui.watchesModel = &watchesModel;
        ui.actionsModel = &actionsModel;
        ui.aiActivityModel = &aiActivityModel;
        ui.networkModel = &networkModel;
        ui.diagnosticsModel = &diagnosticsModel;
        ui.scriptsModel = &scriptsModel;
        ui.inputModel = &inputModel;
        ui.screenshotModel = &screenshotModel;
        ui.runtimeEventsModel = &runtimeEventsModel;
        ui.patchesModel = &patchesModel;
        ui.nativeRenderDevice = NativeRenderDevice();

        workspaces.Add<cortex::ui::OverviewWorkspace>();
        workspaces.Add<cortex::ui::BottomPanelWorkspace>();
        workspaces.Add<cortex::ui::AddressesWorkspace>();
        workspaces.Add<cortex::ui::MemoryWorkspace>();
        workspaces.Add<cortex::ui::MemoryBrowserWorkspace>();
        workspaces.Add<cortex::ui::DisassemblyWorkspace>();
        workspaces.Add<cortex::ui::ModulesWorkspace>();
        workspaces.Add<cortex::ui::DebuggerWorkspace>();
        workspaces.Add<cortex::ui::RuntimeWorkspace>();
        workspaces.Add<cortex::ui::SessionsWorkspace>();
        workspaces.Add<cortex::ui::SettingsWorkspace>();
        workspaces.Add<cortex::ui::ProjectWorkspace>();
        workspaces.Add<cortex::ui::SymbolsWorkspace>();
        workspaces.Add<cortex::ui::StructuresWorkspace>();
        workspaces.Add<cortex::ui::PointerMapsWorkspace>();
        workspaces.Add<cortex::ui::SnapshotsWorkspace>();
        workspaces.Add<cortex::ui::ReWorkspace>();
        workspaces.Add<cortex::ui::InstrumentationWorkspace>();
        workspaces.Add<cortex::ui::WatchesWorkspace>();
        workspaces.Add<cortex::ui::ActionsWorkspace>();
        workspaces.Add<cortex::ui::NetworkWorkspace>();
        workspaces.Add<cortex::ui::DiagnosticsWorkspace>();
        workspaces.Add<cortex::ui::ScriptsWorkspace>();
        workspaces.Add<cortex::ui::InputWorkspace>();
        workspaces.Add<cortex::ui::ScreenshotWorkspace>();
        workspaces.Add<cortex::ui::PatchesWorkspace>();
        workspaces.Add<cortex::ui::EventsWorkspace>();
        workspaces.Add<cortex::ui::TraceWorkspace>();
        workspaces.ApplyPreset(cortex::ui::WorkspacePreset::Memory, false);
    }

    ~AppState() {
        for (const auto pid : pausedPids) cortex::process_control::Resume(pid);
    }

    void OnAttached(const cortex::target::TargetDescriptor& target,
                    bool selectMemory = true) {
        payload.Reset();
        debuggerModel.Reset();
        projectModel.Reset();
        symbolsModel.Reset();
        structuresModel.Reset();
        pointerMapsModel.Reset();
        snapshotsModel.Reset();
        reModel.Reset();
        instrumentationModel.Reset();
        watchesModel.Reset();
        actionsModel.Reset();
        networkModel.Reset();
        diagnosticsModel.Reset();
        scriptsModel.Reset();
        inputModel.Reset();
        screenshotModel.Reset();
        runtimeEventsModel.Reset();
        patchesModel.Reset();
        promptModel.Reset();
        ui.mutationAllowed = false;
        ui.ResetNavigation();
        ui.status = "Attached to " + target.name;
        if (selectMemory) ui.requestWorkspace = "memory";

        if (settings.Values().autoLoadRuntimeOnAttach) {
            ui.RunInBackground("Loading the Cortex runtime into the target", [this]() {
                std::string error;
                if (payload.EnsureReady(&error))
                    ui.status += " | runtime auto-loaded";
                else
                    ui.status += " | runtime auto-load failed: " + error;
            });
        }
    }

    void RefreshTargets() {
        targets = catalog.Targets();
        const DWORD selfPid = GetCurrentProcessId();
        targets.erase(std::remove_if(targets.begin(), targets.end(),
            [selfPid](const cortex::target::TargetDescriptor& target) {
                return target.processId == selfPid;
            }), targets.end());

        std::sort(targets.begin(), targets.end(),
            [](const auto& a, const auto& b) {
                const std::string an = Lower(a.name);
                const std::string bn = Lower(b.name);
                if (an != bn) return an < bn;
                return a.processId < b.processId;
            });
        selectedTarget = -1;
    }
};

inline bool SmokeArgValue(const std::vector<std::string>& args,
                   const std::string& name, std::string& value) {
    for (size_t i = 2; i + 1 < args.size(); ++i) {
        if (args[i] == name) {
            value = args[i + 1];
            return true;
        }
    }
    return false;
}


// Desktop UI (desktop_ui.cpp).
void DrawApp(AppState& app);
void HandleGlobalShortcuts(AppState& app);
// Runs a hotkey or palette command (see HotkeyActions): target commands
// here, the rest in the workspaces.
void DispatchCommand(AppState& app, const std::string& command);
void ResetTargetState(AppState& app);
void NavigateTo(AppState& app, uint64_t address, const char* workspace);
bool ParseAddressToken(const std::string& text, uint64_t& value, int defaultBase = 0);

// Every workspace preset, in header order.
inline constexpr cortex::ui::WorkspacePreset kGuiPresets[] = {
    cortex::ui::WorkspacePreset::Memory,
    cortex::ui::WorkspacePreset::Debug,
    cortex::ui::WorkspacePreset::ReverseEngineering,
    cortex::ui::WorkspacePreset::Trace,
    cortex::ui::WorkspacePreset::Automation,
    cortex::ui::WorkspacePreset::Runtime
};

#if CORTEX_WITH_TEST_MODES
// GUI test modes (smoke_modes.cpp), used by CI and release validation.
int RunImGuiSmokeTest();
int RunGuiWorkspaceSuite();
int RunGuiAttachedSuite(const std::vector<std::string>& args);
int RunGuiMultiSessionSuite(const std::vector<std::string>& args);
int RunGuiCrossBitnessSuite(const std::vector<std::string>& args);
int RunAiActivityChannelSmoke();
int RunPromptChannelSmoke(const std::vector<std::string>& args);
int RunEventChannelSmoke(const std::vector<std::string>& args);
#endif

}  // namespace cortex::desktop
