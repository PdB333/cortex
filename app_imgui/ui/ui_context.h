#pragma once

#include <algorithm>
#include <functional>

#include "application/actions_model.h"
#include "application/ai_activity_model.h"
#include "application/diagnostics_model.h"
#include "application/input_model.h"
#include "application/network_model.h"
#include "application/debugger_model.h"
#include "application/project_model.h"
#include "application/patches_model.h"
#include "application/instrumentation_model.h"
#include "application/re_model.h"
#include "application/runtime_events_model.h"
#include "application/settings.h"
#include "application/pointer_maps_model.h"
#include "application/snapshots_model.h"
#include "application/structures_model.h"
#include "application/symbols_model.h"
#include "application/watches_model.h"
#include "application/scripts_model.h"
#include "application/screenshot_model.h"
#include "services/debugger_service.h"
#include "services/disassembly_service.h"
#include "services/memory_service.h"
#include "services/module_service.h"
#include "services/payload_client.h"
#include "services/value_scanner.h"
#include "target/session_manager.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cortex::ui {

struct NavigationEntry {
    std::string workspace;
    uint64_t address = 0;
};

// An entry another workspace hands to the Memory address list: an absolute
// address, or a pointer chain module+baseOffset -> offsets.
struct PendingAddressEntry {
    std::string description;
    uint64_t address = 0;
    services::ScanDataType type = services::ScanDataType::Int32;
    std::string module;
    uint64_t baseOffset = 0;
    std::vector<uint32_t> offsets;
    unsigned pointerSize = 8;
};

// "Find out what writes to / accesses this address": a hardware breakpoint
// in log mode whose hits are counted per instruction.
struct AccessFinderRequest {
    uint64_t address = 0;
    int size = 4;
    bool writesOnly = true;
    // "Find out what addresses this instruction accesses": an execute
    // breakpoint on the instruction at address.
    bool instruction = false;
};

struct UiContext {
    target::SessionManager* sessions = nullptr;
    services::MemoryService* memory = nullptr;
    services::ModuleService* modules = nullptr;
    services::DisassemblyService* disassembly = nullptr;
    services::DebuggerService* debugger = nullptr;
    services::PayloadClient* payload = nullptr;
    application::SettingsStore* settings = nullptr;
    application::DebuggerModel* debuggerModel = nullptr;
    application::ProjectModel* projectModel = nullptr;
    application::SymbolsModel* symbolsModel = nullptr;
    application::StructuresModel* structuresModel = nullptr;
    application::PointerMapsModel* pointerMapsModel = nullptr;
    application::SnapshotsModel* snapshotsModel = nullptr;
    application::ReModel* reModel = nullptr;
    application::InstrumentationModel* instrumentationModel = nullptr;
    application::WatchesModel* watchesModel = nullptr;
    application::ActionsModel* actionsModel = nullptr;
    application::AiActivityModel* aiActivityModel = nullptr;
    application::NetworkModel* networkModel = nullptr;
    application::DiagnosticsModel* diagnosticsModel = nullptr;
    application::ScriptsModel* scriptsModel = nullptr;
    application::InputModel* inputModel = nullptr;
    application::ScreenshotModel* screenshotModel = nullptr;
    application::RuntimeEventsModel* runtimeEventsModel = nullptr;
    application::PatchesModel* patchesModel = nullptr;
    void* nativeRenderDevice = nullptr;

    bool mutationAllowed = false;
    bool requestProcessPicker = false;
    // Commands from global hotkeys and the command palette (see
    // HotkeyActions), delivered to every workspace before drawing.
    std::vector<std::string> commands;
    // The active target is suspended by Pause target.
    bool targetPaused = false;
    // Hotkey actions whose chord could not be registered.
    std::vector<std::string> hotkeyFailures;
    // Entries for the Memory address list, and the Memory tools tab to show
    // with the next navigation to "tools".
    std::vector<PendingAddressEntry> pendingAddresses;
    std::vector<AccessFinderRequest> accessFinderRequests;
    std::string toolsTabRequest;
    std::string status = "Select a process to begin";

    // Lightweight cross-workspace navigation. A workspace can request another
    // workspace and optionally pass one address without depending on its class.
    std::string requestWorkspace;
    // Runs a long runtime operation (loading the runtime, captures, diffs,
    // exports) away from the UI thread. While it runs the window stays
    // responsive and shows progress instead of freezing; no workspace is
    // drawn, so the operation may freely update models and `status`. Without
    // a runner (headless tests) the work runs inline.
    std::function<void(std::string, std::function<void()>)> runInBackground;
    void RunInBackground(std::string label, std::function<void()> work) {
        if (runInBackground) runInBackground(std::move(label), std::move(work));
        else work();
    }
    // Workspace ids open this frame, published by the registry before any
    // workspace draws. Lets summary surfaces avoid repeating a full panel.
    std::vector<std::string> openWorkspaces;
    bool WorkspaceOpen(const std::string& id) const {
        return std::find(openWorkspaces.begin(), openWorkspaces.end(), id) != openWorkspaces.end();
    }
    uint64_t navigationAddress = 0;
    bool navigationAddressPending = false;
    std::string navigationTargetWorkspace;
    std::vector<NavigationEntry> navigationBack;
    std::vector<NavigationEntry> navigationForward;
    NavigationEntry navigationCurrent;
    bool navigationCurrentValid = false;

    void NavigateTo(const std::string& workspace, uint64_t address, bool recordHistory = true) {
        const std::string targetWorkspace = workspace.empty() ? "memory-browser" : workspace;
        if (recordHistory && navigationCurrentValid &&
            (navigationCurrent.workspace != targetWorkspace || navigationCurrent.address != address)) {
            navigationBack.push_back(navigationCurrent);
            if (navigationBack.size() > 64) navigationBack.erase(navigationBack.begin());
            navigationForward.clear();
        }
        navigationCurrent = {targetWorkspace, address};
        navigationCurrentValid = true;
        requestWorkspace = targetWorkspace;
        navigationTargetWorkspace = targetWorkspace;
        navigationAddress = address;
        navigationAddressPending = true;
    }

    bool ConsumeNavigation(const std::string& workspace, uint64_t& address) {
        if (!navigationAddressPending || navigationTargetWorkspace != workspace) return false;
        address = navigationAddress;
        navigationAddressPending = false;
        navigationTargetWorkspace.clear();
        return true;
    }

    bool CanNavigateBack() const { return !navigationBack.empty(); }
    bool CanNavigateForward() const { return !navigationForward.empty(); }

    bool NavigateBack() {
        if (navigationBack.empty()) return false;
        if (navigationCurrentValid) {
            navigationForward.push_back(navigationCurrent);
            if (navigationForward.size() > 64) navigationForward.erase(navigationForward.begin());
        }
        const NavigationEntry target = navigationBack.back();
        navigationBack.pop_back();
        NavigateTo(target.workspace, target.address, false);
        return true;
    }

    bool NavigateForward() {
        if (navigationForward.empty()) return false;
        if (navigationCurrentValid) {
            navigationBack.push_back(navigationCurrent);
            if (navigationBack.size() > 64) navigationBack.erase(navigationBack.begin());
        }
        const NavigationEntry target = navigationForward.back();
        navigationForward.pop_back();
        NavigateTo(target.workspace, target.address, false);
        return true;
    }

    void ResetNavigation() {
        requestWorkspace.clear();
        navigationAddress = 0;
        navigationAddressPending = false;
        navigationTargetWorkspace.clear();
        navigationBack.clear();
        navigationForward.clear();
        navigationCurrent = {};
        navigationCurrentValid = false;
    }

    // Optional generic runtime-tool preset used by context menus such as
    // "Find what writes this".
    std::string runtimeToolPreset;
    std::string runtimeArgumentsPreset;
    bool requestSemanticRuntime = false;
};

} // namespace cortex::ui
