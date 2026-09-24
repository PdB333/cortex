#pragma once

#include "application/actions_model.h"
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
#include "target/session_manager.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cortex::ui {

struct NavigationEntry {
    std::string workspace;
    uint64_t address = 0;
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
    std::string status = "Select a process to begin";

    // Lightweight cross-workspace navigation. A workspace can request another
    // workspace and optionally pass one address without depending on its class.
    std::string requestWorkspace;
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
