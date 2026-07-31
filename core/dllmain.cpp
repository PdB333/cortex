#include <windows.h>
#include <MinHook.h>
#include <cstdio>
#include <atomic>

#include "config.h"
#include "log.h"
#include "diagnostics/diagnostics.h"
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

        if (diagnostics::Init()) {
            dbglog::Line("diagnostics::Init done");
        } else {
            dbglog::Line("diagnostics::Init failed");
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

        symbols::Init();
        dbglog::Line("symbols::Init done");

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

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpvReserved) {
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
