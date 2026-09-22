#pragma once

#include "application/debugger_model.h"
#include "application/project_model.h"
#include "application/instrumentation_model.h"
#include "application/re_model.h"
#include "application/settings.h"
#include "application/pointer_maps_model.h"
#include "application/snapshots_model.h"
#include "application/structures_model.h"
#include "application/symbols_model.h"
#include "services/debugger_service.h"
#include "services/disassembly_service.h"
#include "services/memory_service.h"
#include "services/module_service.h"
#include "services/payload_client.h"
#include "target/session_manager.h"

#include <cstdint>
#include <string>

namespace cortex::ui {

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

    bool mutationAllowed = false;
    bool requestProcessPicker = false;
    std::string status = "Select a process to begin";

    // Lightweight cross-workspace navigation. A workspace can request another
    // workspace and optionally pass one address without depending on its class.
    std::string requestWorkspace;
    uint64_t navigationAddress = 0;
    bool navigationAddressPending = false;

    // Optional generic runtime-tool preset used by context menus such as
    // "Find what writes this".
    std::string runtimeToolPreset;
    std::string runtimeArgumentsPreset;
};

} // namespace cortex::ui
