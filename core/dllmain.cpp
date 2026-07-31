#include <windows.h>
#include <MinHook.h>
#include <cstdio>
#include <atomic>
#include <string>

#include "config.h"
#include "log.h"
#include "diagnostics/diagnostics.h"
#include "diagnostics/registry.h"
#include "diagnostics/symbolizer.h"
#include "diagnostics/hooks.h"
// Keep diagnostics implementation in the injected core translation unit.
// Dedicated test targets compile these files separately.
#include "diagnostics/diagnostics.cpp"
#include "diagnostics/registry.cpp"
#include "diagnostics/symbolizer.cpp"
#include "diagnostics/hooks.cpp"
#ifdef CORTEX_KIERO
#include "hook/kiero_hook.h"
#endif
#ifdef CORTEX_D3D8
#include "hook/d3d8_hook.h"
#endif
#include "hook/input_hook.h"
#include "hook/dinput_hook.h"
#include "hook/net_hook.h"
#include "api/server.h"
#include "debugger/debugger.h"
#include "symbols/symbols.h"
#include "project/project.h"
#include "freeze/freeze.h"
#include "struct/structs.h"
#include "call/call.h"
#include "watch/watch.h"

namespace {
    HMODULE g_hModule = nullptr;
    // 0=running, 1=shutdown in progress, 2=shutdown complete.
    std::atomic<int> g_shutdownState{0};

    void SetupConsole() {
        AllocConsole();
        FILE* f;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
        SetConsoleTitleA("Cortex - debug console");
        printf("[Cortex] console attached\n");
    }

    DWORD WINAPI InitThread(LPVOID) {
        dbglog::Init();
        dbglog::Line("InitThread start");
        config::Config cfg = config::Load();

        if (cfg.log_console) SetupConsole();

        std::string crashDirectory = cfg.diagnostics_crash_directory;
        if (crashDirectory.empty())
            crashDirectory = config::GetModuleDir() + "\\cortex_crashes";

        diagnostics::Options diagOptions;
        diagOptions.enabled = cfg.diagnostics_enabled;
        diagOptions.writeMinidump = cfg.diagnostics_write_minidump;
        diagOptions.outputDirectory = crashDirectory;

        bool registryReady = true;
        if (cfg.diagnostics_enabled) {
            registryReady = diagnostics::RegistryInit(crashDirectory.c_str());
            dbglog::Line("diagnostics::RegistryInit %s", registryReady ? "done" : "failed");
        }
        if (diagnostics::Init(diagOptions)) {
            dbglog::Line("diagnostics::Init done, enabled=%d", diagnostics::IsEnabled() ? 1 : 0);
        } else {
            dbglog::Line("diagnostics::Init failed");
            if (registryReady && cfg.diagnostics_enabled) diagnostics::RegistryShutdown();
        }

        if (MH_Initialize() != MH_OK) {
            printf("[Cortex] MH_Initialize failed\n");
            dbglog::Line("MH_Initialize failed");
            return 1;
        }
        dbglog::Line("MH_Initialize ok");

#ifdef CORTEX_KIERO
        if (!hook::StartKieroHookWithRetry()) {
            printf("[Cortex] kiero renderer not ready; retrying in background\n");
            dbglog::Line("kiero renderer not ready; retry scheduled");
        } else {
            printf("[Cortex] kiero render hook installed\n");
            dbglog::Line("InitKieroHook ok");
        }
#endif
#ifdef CORTEX_D3D8
        if (!hook::InitD3D8Hook()) {
            printf("[Cortex] D3D8 hook install failed\n");
            dbglog::Line("InitD3D8Hook failed");
        } else {
            printf("[Cortex] D3D8 EndScene/Reset hooked\n");
            dbglog::Line("InitD3D8Hook ok");
        }
#endif

        if (!hook::InitInputHook()) {
            printf("[Cortex] cursor input hook install failed\n");
            dbglog::Line("InitInputHook failed");
        } else {
            printf("[Cortex] cursor input hooked\n");
            dbglog::Line("InitInputHook ok");
        }

        if (!hook::InitDInputHook()) {
            printf("[Cortex] DirectInput hook install failed\n");
            dbglog::Line("InitDInputHook failed");
        } else {
            printf("[Cortex] DirectInput hooked\n");
            dbglog::Line("InitDInputHook ok");
        }

        if (!nethook::Init()) dbglog::Line("nethook::Init failed");
        else                  dbglog::Line("nethook::Init ok");

        dbg::Init();
        dbglog::Line("dbg::Init done");

        std::string symbolSearchPath = cfg.diagnostics_symbol_path;
        if (symbolSearchPath.empty())
            symbolSearchPath = config::GetModuleDir() + "\\cortex_symbols";
        symbols::Init(symbolSearchPath, true);
        dbglog::Line("symbols::Init %s, path=%s",
                     symbols::IsInitialized() ? "done" : "failed",
                     symbolSearchPath.c_str());

        if (cfg.diagnostics_enabled && diagnostics::IsEnabled() && cfg.diagnostics_symbolize) {
            diagnostics::SymbolizerOptions symbolizerOptions;
            symbolizerOptions.enabled = true;
            symbolizerOptions.crashOutputDirectory = crashDirectory;
            symbolizerOptions.symbolSearchPath = symbolSearchPath;
            symbolizerOptions.externalToolPath = cfg.diagnostics_external_symbolizer;
            symbolizerOptions.maxFrames = static_cast<size_t>(cfg.diagnostics_max_stack_frames);
            const bool symbolizerReady = diagnostics::SymbolizerInit(symbolizerOptions);
            dbglog::Line("diagnostics::SymbolizerInit %s", symbolizerReady ? "done" : "failed");
        }

        if (cfg.diagnostics_enabled && diagnostics::IsEnabled()) {
            const bool hooksReady = diagnostics::HookRegistryInit(crashDirectory.c_str());
            dbglog::Line("diagnostics::HookRegistryInit %s", hooksReady ? "done" : "failed");
        }

        project::Init();
        dbglog::Line("project::Init done");

        structs::Init();
        dbglog::Line("structs::Init done");

        freeze::Init();
        dbglog::Line("freeze::Init done");

        remotecall::Init();
        dbglog::Line("remotecall::Init done");

        watch::Init();
        dbglog::Line("watch::Init done");

        if (api::Start(cfg.port, cfg.api_token)) {
            printf("[Cortex] API listening on http://127.0.0.1:%d\n", cfg.port);
            dbglog::Line("api::Start done, port=%d", cfg.port);
        } else {
            printf("[Cortex] API failed: %s\n", api::GetLastError().c_str());
            dbglog::Line("api::Start failed: %s", api::GetLastError().c_str());
        }

        return 0;
    }

    DWORD WINAPI ShutdownThread(LPVOID) {
        int expected = 0;
        if (!g_shutdownState.compare_exchange_strong(expected, 1)) return expected == 2 ? 0 : 1;
        api::Stop();
        watch::Shutdown();
        remotecall::Shutdown();
        freeze::Shutdown();
        diagnostics::HookRegistryShutdown();
        diagnostics::SymbolizerShutdown();
        symbols::Shutdown();
        if (!dbg::Shutdown()) {
            dbglog::Line("shutdown refused: a debugger-paused thread did not leave Cortex code in time");
            g_shutdownState = 0;
            return 1;
        }
#ifdef CORTEX_KIERO
        hook::ShutdownKieroHook();
#endif
#ifdef CORTEX_D3D8
        hook::ShutdownD3D8Hook();
#endif
        diagnostics::Shutdown();
        diagnostics::RegistryShutdown();
        MH_Uninitialize();
        FreeConsole();
        g_shutdownState = 2;
        return 0;
    }
}

// Explicit unload contract: callers that intend to FreeLibrary(cortex_core)
// must call this first. DllMain cannot safely join workers while Windows holds
// the loader lock.
extern "C" __declspec(dllexport) BOOL CortexShutdown() {
    return ShutdownThread(nullptr) == 0 ? TRUE : FALSE;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            g_hModule = hModule;
            DisableThreadLibraryCalls(hModule);
            HANDLE h = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
            if (h) CloseHandle(h);
            break;
        }
        case DLL_PROCESS_DETACH: {
            // No blocking cleanup here: DLL_PROCESS_DETACH runs under the
            // loader lock. Explicit unloaders must call CortexShutdown first;
            // process termination needs no cleanup.
            break;
        }
    }
    return TRUE;
}
