#include "application/actions_model.h"
#include "application/ai_activity_model.h"
#include "application/diagnostics_model.h"
#include "application/input_model.h"
#include "application/network_model.h"
#include "application/debugger_model.h"
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
#include "ui/theme.h"
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

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <d3d11.h>
#include <shellapi.h>
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

int CortexMcpMain(int argc, char** argv);
int CortexInjectMain(int argc, char** argv);
int CortexProbeMain(int argc, char** argv);
int CortexDiagnoseMain(int argc, char** argv);
int CortexSymbolizeMain(int argc, char** argv);

namespace {

using CliEntryPoint = int (*)(int, char**);

int RunImGuiSmokeTest();
int RunGuiWorkspaceSuite();
int RunGuiAttachedSuite(const std::vector<std::string>& args);
int RunGuiMultiSessionSuite(const std::vector<std::string>& args);
int RunGuiCrossBitnessSuite(const std::vector<std::string>& args);
int RunAiActivityChannelSmoke();
int RunPromptChannelSmoke(const std::vector<std::string>& args);
int RunEventChannelSmoke(const std::vector<std::string>& args);

std::vector<std::string> CurrentCommandLineArgs() {
    int count = 0;
    LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count);
    std::vector<std::string> result;
    if (!wide || count <= 0) return result;
    result.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
        if (bytes <= 0) {
            result.emplace_back();
            continue;
        }
        std::string value(static_cast<size_t>(bytes), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, value.data(), bytes, nullptr, nullptr);
        if (!value.empty() && value.back() == '\0') value.pop_back();
        result.push_back(std::move(value));
    }
    LocalFree(wide);
    return result;
}

int ForwardCli(CliEntryPoint entry, const char* programName,
               std::vector<std::string>& args, size_t firstArgument,
               const std::vector<std::string>& injected = {}) {
    std::vector<std::string> storage;
    storage.emplace_back(programName ? programName : "cortex");
    storage.insert(storage.end(), injected.begin(), injected.end());
    for (size_t i = firstArgument; i < args.size(); ++i) storage.push_back(args[i]);
    std::vector<char*> raw;
    raw.reserve(storage.size() + 1);
    for (auto& value : storage) raw.push_back(value.data());
    raw.push_back(nullptr);
    return entry(static_cast<int>(storage.size()), raw.data());
}

void PrintCliUsage(FILE* out = stdout) {
    std::fputs(
        "Cortex lightweight migration preview\n\n"
        "Usage:\n"
        "  cortex                         Launch the ImGui desktop UI\n"
        "  cortex --version               Print preview version\n"
        "  cortex --smoke-test            Run deterministic headless ImGui smoke\n"
        "  cortex --gui-workspace-suite   Render all workspaces/presets and validate docking\n"
        "  cortex --gui-attached-suite    Exercise GUI against a live --pid target\n"
        "  cortex --gui-multi-session-suite  Exercise two attached --pid-a/--pid-b targets\n"
        "  cortex --gui-cross-bitness-suite  Validate x64 GUI -> x86 --pid runtime bootstrap\n"
        "  cortex --window-smoke-test     Run native Win32 + D3D11 backend smoke\n"
        "  cortex --ai-activity-smoke     Validate cross-process AI activity IPC\n"
        "  cortex --prompt-channel-smoke  Answer a private prompt for --pid\n"
        "  cortex --event-channel-smoke   Observe a runtime event for --pid\n"
        "  cortex mcp [options]           Run the native/HTTP MCP stdio bridge\n"
        "  cortex inject <target> [dll]   Inject cortex_core.dll\n"
        "  cortex probe --pid <pid>       Inspect target/runtime health\n"
        "  cortex diagnose --pid <pid>    Watch crash/hang diagnostics\n"
        "  cortex analyze <directory>     Analyze a crash directory\n"
        "  cortex symbolize [options]     Resolve PE symbols offline\n",
        out);
}

std::optional<int> RunCliMode(std::vector<std::string>& args) {
    if (args.size() <= 1) return std::nullopt;
    const std::string& command = args[1];

    if (command == "--help" || command == "-h" || command == "help") {
        PrintCliUsage();
        return 0;
    }
    if (command == "--version" || command == "version") {
        std::puts("cortex 0.8.0-dev-imgui");
        return 0;
    }
    if (command == "--smoke-test" || command == "smoke-test")
        return RunImGuiSmokeTest();
    if (command == "--gui-workspace-suite")
        return RunGuiWorkspaceSuite();
    if (command == "--gui-attached-suite")
        return RunGuiAttachedSuite(args);
    if (command == "--gui-multi-session-suite")
        return RunGuiMultiSessionSuite(args);
    if (command == "--gui-cross-bitness-suite")
        return RunGuiCrossBitnessSuite(args);
    if (command == "--ai-activity-smoke")
        return RunAiActivityChannelSmoke();
    if (command == "--prompt-channel-smoke")
        return RunPromptChannelSmoke(args);
    if (command == "--event-channel-smoke")
        return RunEventChannelSmoke(args);
    if (command == "mcp") return ForwardCli(CortexMcpMain, "cortex mcp", args, 2);
    if (command == "inject") return ForwardCli(CortexInjectMain, "cortex inject", args, 2);
    if (command == "probe") return ForwardCli(CortexProbeMain, "cortex probe", args, 2);
    if (command == "diagnose" || command == "diagnostics" || command == "watch")
        return ForwardCli(CortexDiagnoseMain, "cortex diagnose", args, 2);
    if (command == "analyze" || command == "analyse") {
        if (args.size() < 3) {
            std::fputs("cortex analyze: missing crash directory\n", stderr);
            return 2;
        }
        return ForwardCli(CortexDiagnoseMain, "cortex analyze", args, 2, {"--analyze"});
    }
    if (command == "symbolize" || command == "symbolise" || command == "symbols")
        return ForwardCli(CortexSymbolizeMain, "cortex symbolize", args, 2);
    return std::nullopt;
}

ID3D11Device* gDevice = nullptr;
ID3D11DeviceContext* gDeviceContext = nullptr;
IDXGISwapChain* gSwapChain = nullptr;
ID3D11RenderTargetView* gMainRenderTargetView = nullptr;

void WriteStdout(const char* text) {
    if (!text) return;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out && out != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(out, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
    }
}

int HandleCommandLine() {
    auto args = CurrentCommandLineArgs();
    if (args.size() <= 1) return -1;
    if (const auto result = RunCliMode(args)) return *result;
    return -1;
}

std::string ExecutableDirectory() {
    std::wstring buffer(32768, L'\0');
    DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (size == 0 || size >= buffer.size()) return ".";
    buffer.resize(size);
    return std::filesystem::path(buffer).parent_path().u8string();
}

void CreateRenderTarget() {
    ID3D11Texture2D* backBuffer = nullptr;
    gSwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (backBuffer) {
        gDevice->CreateRenderTargetView(backBuffer, nullptr, &gMainRenderTargetView);
        backBuffer->Release();
    }
}

void CleanupRenderTarget() {
    if (gMainRenderTargetView) {
        gMainRenderTargetView->Release();
        gMainRenderTargetView = nullptr;
    }
}

bool CreateDeviceD3D(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    constexpr D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0
    };

    D3D_FEATURE_LEVEL featureLevel{};
    HRESULT result = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        featureLevels, 2, D3D11_SDK_VERSION, &sd,
        &gSwapChain, &gDevice, &featureLevel, &gDeviceContext);

    if (FAILED(result)) {
        result = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            featureLevels, 2, D3D11_SDK_VERSION, &sd,
            &gSwapChain, &gDevice, &featureLevel, &gDeviceContext);
    }
    if (FAILED(result)) return false;
    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (gSwapChain) { gSwapChain->Release(); gSwapChain = nullptr; }
    if (gDeviceContext) { gDeviceContext->Release(); gDeviceContext = nullptr; }
    if (gDevice) { gDevice->Release(); gDevice = nullptr; }
}

LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return true;

    switch (msg) {
        case WM_SIZE:
            if (gDevice && wParam != SIZE_MINIMIZED) {
                CleanupRenderTarget();
                gSwapChain->ResizeBuffers(0, static_cast<UINT>(LOWORD(lParam)),
                                          static_cast<UINT>(HIWORD(lParam)),
                                          DXGI_FORMAT_UNKNOWN, 0);
                CreateRenderTarget();
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

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
        ui.nativeRenderDevice = gDevice;

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
            std::string error;
            if (payload.EnsureReady(&error))
                ui.status += " | runtime auto-loaded";
            else
                ui.status += " | runtime auto-load failed: " + error;
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

bool SmokeArgValue(const std::vector<std::string>& args,
                   const std::string& name, std::string& value) {
    for (size_t i = 2; i + 1 < args.size(); ++i) {
        if (args[i] == name) {
            value = args[i + 1];
            return true;
        }
    }
    return false;
}

bool AttachSmokeTarget(AppState& app, DWORD pid, std::string& error) {
    for (int attempt = 0; attempt < 30; ++attempt) {
        app.RefreshTargets();
        for (const auto& target : app.targets) {
            if (target.processId != pid) continue;
            if (!app.sessions.Attach(target, &error)) return false;
            app.OnAttached(target);
            return true;
        }
        Sleep(100);
    }
    error = "target_not_found";
    return false;
}

int RunPromptChannelSmoke(const std::vector<std::string>& args) {
    std::string pidText;
    std::string answer = "imgui-prompt-e2e";
    SmokeArgValue(args, "--answer", answer);
    if (!SmokeArgValue(args, "--pid", pidText)) {
        std::fputs("--prompt-channel-smoke requires --pid <target pid>\n", stderr);
        return 2;
    }

    DWORD pid = 0;
    try { pid = static_cast<DWORD>(std::stoul(pidText)); }
    catch (...) { pid = 0; }
    if (!pid) {
        std::fputs("invalid prompt smoke pid\n", stderr);
        return 2;
    }

    AppState app;
    std::string error;
    if (!AttachSmokeTarget(app, pid, error)) {
        std::fprintf(stderr, "prompt smoke attach failed: %s\n", error.c_str());
        return 3;
    }

    for (int attempt = 0; attempt < 60 && !app.promptModel.Active(); ++attempt) {
        app.promptModel.Refresh(&error);
        if (!app.promptModel.Active()) Sleep(100);
    }
    if (!app.promptModel.Active()) {
        std::fprintf(stderr, "prompt smoke did not observe an active prompt: %s\n",
                     error.c_str());
        return 4;
    }

    const int promptId = app.promptModel.Id();
    if (!app.promptModel.Answer(answer, &error)) {
        std::fprintf(stderr, "prompt smoke answer failed: %s\n", error.c_str());
        return 5;
    }

    std::printf("PASS: ImGui prompt channel answered prompt %d\n", promptId);
    return 0;
}

int RunEventChannelSmoke(const std::vector<std::string>& args) {
    std::string pidText;
    std::string expected = "prompt.answered";
    SmokeArgValue(args, "--expect", expected);
    if (!SmokeArgValue(args, "--pid", pidText)) {
        std::fputs("--event-channel-smoke requires --pid <target pid>\n", stderr);
        return 2;
    }

    DWORD pid = 0;
    try { pid = static_cast<DWORD>(std::stoul(pidText)); }
    catch (...) { pid = 0; }
    if (!pid) {
        std::fputs("invalid event smoke pid\n", stderr);
        return 2;
    }

    AppState app;
    std::string error;
    if (!AttachSmokeTarget(app, pid, error)) {
        std::fprintf(stderr, "event smoke attach failed: %s\n", error.c_str());
        return 3;
    }
    if (!app.payload.Ready() && !app.payload.TryConnectExisting(&error)) {
        std::fprintf(stderr, "event smoke runtime connect failed: %s\n", error.c_str());
        return 4;
    }

    for (int attempt = 0; attempt < 40; ++attempt) {
        if (app.runtimeEventsModel.RefreshEvents(&error)) {
            for (const auto& event : app.runtimeEventsModel.Events()) {
                if (event.type == expected) {
                    std::printf("PASS: ImGui event channel observed %s\n", expected.c_str());
                    return 0;
                }
            }
        }
        Sleep(100);
    }

    std::fprintf(stderr, "event smoke did not observe %s: %s\n",
                 expected.c_str(), error.c_str());
    return 5;
}

struct SmokeMcpChild {
    PROCESS_INFORMATION process{};
    HANDLE input = INVALID_HANDLE_VALUE;
    HANDLE output = INVALID_HANDLE_VALUE;

    ~SmokeMcpChild() {
        if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
        if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
        if (process.hThread) CloseHandle(process.hThread);
        if (process.hProcess) CloseHandle(process.hProcess);
    }
};

bool StartMcpSmokeChild(SmokeMcpChild& child, std::string& error) {
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE childInput = INVALID_HANDLE_VALUE;
    HANDLE childOutput = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&childInput, &child.input, &security, 0) ||
        !CreatePipe(&child.output, &childOutput, &security, 0)) {
        error = "pipe_create_failed";
        if (childInput != INVALID_HANDLE_VALUE) CloseHandle(childInput);
        if (childOutput != INVALID_HANDLE_VALUE) CloseHandle(childOutput);
        return false;
    }
    SetHandleInformation(child.input, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(child.output, HANDLE_FLAG_INHERIT, 0);

    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!length || length >= executable.size()) {
        error = "module_path_failed";
        CloseHandle(childInput);
        CloseHandle(childOutput);
        return false;
    }
    executable.resize(length);

    std::wstring command = L"\"" + executable + L"\" mcp --tools all";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = childInput;
    startup.hStdOutput = childOutput;
    startup.hStdError = childOutput;

    const BOOL started = CreateProcessW(
        nullptr, mutableCommand.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child.process);
    CloseHandle(childInput);
    CloseHandle(childOutput);
    if (!started) {
        error = "mcp_child_start_failed:" + std::to_string(GetLastError());
        return false;
    }
    return true;
}

bool WriteSmokeJson(HANDLE input, const char* json) {
    const std::string payload = std::string(json ? json : "") + "\n";
    DWORD written = 0;
    return WriteFile(input, payload.data(), static_cast<DWORD>(payload.size()),
                     &written, nullptr) &&
           written == payload.size();
}

void DrainSmokeOutput(HANDLE output, std::string& text) {
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(output, nullptr, 0, nullptr, &available, nullptr) ||
            available == 0)
            return;
        char buffer[4096];
        const DWORD wanted = (std::min)(available, static_cast<DWORD>(sizeof(buffer)));
        DWORD read = 0;
        if (!ReadFile(output, buffer, wanted, &read, nullptr) || read == 0) return;
        text.append(buffer, buffer + read);
    }
}

int RunAiActivityChannelSmoke() {
    cortex::application::AiActivityModel activity;
    if (!activity.Listening()) {
        std::fputs("AI activity smoke could not own the native activity endpoint\n", stderr);
        return 2;
    }

    SmokeMcpChild child;
    std::string error;
    if (!StartMcpSmokeChild(child, error)) {
        std::fprintf(stderr, "AI activity smoke child start failed: %s\n", error.c_str());
        return 3;
    }

    constexpr const char* initialize =
        R"json({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"imgui-ai-activity-smoke","version":"1"}}})json";
    constexpr const char* initialized =
        R"json({"jsonrpc":"2.0","method":"notifications/initialized"})json";
    constexpr const char* toolCall =
        R"json({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"cortex_targets","arguments":{}}})json";

    if (!WriteSmokeJson(child.input, initialize) ||
        !WriteSmokeJson(child.input, initialized) ||
        !WriteSmokeJson(child.input, toolCall)) {
        std::fputs("AI activity smoke could not write MCP requests\n", stderr);
        TerminateProcess(child.process.hProcess, 1);
        return 4;
    }

    bool sawResponse = false;
    bool sawStarted = false;
    bool sawCompleted = false;
    std::string output;
    const ULONGLONG deadline = GetTickCount64() + 8000;
    while (GetTickCount64() < deadline &&
           !(sawResponse && sawStarted && sawCompleted)) {
        DrainSmokeOutput(child.output, output);
        if (output.find("\"id\":2") != std::string::npos) sawResponse = true;
        activity.Poll(500);
        for (const auto& row : activity.Activities()) {
            if (row.tool != "cortex_targets") continue;
            if (row.phase == "started") sawStarted = true;
            if (row.phase == "completed") sawCompleted = true;
        }
        Sleep(10);
    }

    CloseHandle(child.input);
    child.input = INVALID_HANDLE_VALUE;
    WaitForSingleObject(child.process.hProcess, 3000);
    activity.Poll(500);

    if (!sawResponse || !sawStarted || !sawCompleted ||
        activity.ActiveTaskCount() != 0) {
        std::fprintf(stderr,
                     "AI activity smoke missed lifecycle response=%d started=%d completed=%d\n",
                     sawResponse ? 1 : 0, sawStarted ? 1 : 0, sawCompleted ? 1 : 0);
        TerminateProcess(child.process.hProcess, 1);
        return 5;
    }

    std::puts("PASS: ImGui AI activity observed a cross-process MCP tool lifecycle");
    return 0;
}

void DrawApp(AppState& app);

constexpr const char* kGuiRequiredWorkspaces[] = {
    "overview", "bottom", "addresses", "memory", "memory-browser", "disassembly",
    "modules", "debugger", "runtime", "sessions", "settings", "project",
    "symbols", "structures", "pointermaps", "snapshots", "re",
    "instrumentation", "watches", "actions", "network", "diagnostics",
    "scripts", "input", "screenshots", "patches", "events", "trace"
};

constexpr cortex::ui::WorkspacePreset kGuiPresets[] = {
    cortex::ui::WorkspacePreset::Memory,
    cortex::ui::WorkspacePreset::Debug,
    cortex::ui::WorkspacePreset::ReverseEngineering,
    cortex::ui::WorkspacePreset::Trace,
    cortex::ui::WorkspacePreset::Automation,
    cortex::ui::WorkspacePreset::Runtime
};

class HeadlessGuiContext {
public:
    HeadlessGuiContext() {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        io.DisplaySize = ImVec2(1440.0f, 900.0f);
        io.DeltaTime = 1.0f / 60.0f;
        io.IniFilename = nullptr;
        io.Fonts->AddFontDefault();
        io.Fonts->Build();
        cortex::ui::ApplyCortexTheme();
    }

    ~HeadlessGuiContext() {
        if (ImGui::GetCurrentContext())
            ImGui::DestroyContext();
    }

    HeadlessGuiContext(const HeadlessGuiContext&) = delete;
    HeadlessGuiContext& operator=(const HeadlessGuiContext&) = delete;
};

bool RenderGuiFrame(
        AppState& app, std::string& error,
        bool validateDocking = true) {
    error.clear();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1440.0f, 900.0f);
    io.DeltaTime = 1.0f / 60.0f;

    ImGui::NewFrame();
    DrawApp(app);
    ImGui::Render();

    const ImDrawData* drawData = ImGui::GetDrawData();
    if (!drawData || !drawData->Valid || drawData->CmdListsCount <= 0) {
        error = "imgui_draw_data_invalid";
        return false;
    }

    if (validateDocking &&
        !app.workspaces.ValidateOpenWindowsDocked(&error))
        return false;

    return true;
}

bool RenderGuiStable(
        AppState& app, std::string& error,
        bool validateDocking = true) {
    // DockBuilder can assign a just-opened window on the first frame; validate
    // the settled layout on the second frame.
    if (!RenderGuiFrame(app, error, false)) return false;
    return RenderGuiFrame(app, error, validateDocking);
}

bool ValidateGuiRegistry(AppState& app, std::string& error) {
    error.clear();
    if (app.workspaces.Count() != IM_ARRAYSIZE(kGuiRequiredWorkspaces)) {
        error = "workspace_count:" + std::to_string(app.workspaces.Count());
        return false;
    }
    if (!app.workspaces.ValidateUnique(&error)) return false;

    for (const char* id : kGuiRequiredWorkspaces) {
        if (!app.workspaces.Has(id)) {
            error = std::string("workspace_missing:") + id;
            return false;
        }
    }
    return true;
}

bool ExerciseGuiPresets(AppState& app, std::string& error) {
    for (const auto preset : kGuiPresets) {
        app.workspaces.ApplyPreset(preset);
        if (!app.workspaces.IsOpen("bottom")) {
            error = std::string("bottom_panel_closed:") +
                    cortex::ui::WorkspaceRegistry::PresetName(preset);
            return false;
        }
        if (!RenderGuiStable(app, error, true)) {
            error = std::string("preset_") +
                    cortex::ui::WorkspaceRegistry::PresetName(preset) +
                    ":" + error;
            return false;
        }
    }
    return true;
}

bool ExerciseEveryWorkspace(AppState& app, std::string& error) {
    for (const char* id : kGuiRequiredWorkspaces) {
        app.workspaces.CloseAll();
        if (!app.workspaces.Select(id)) {
            error = std::string("workspace_select_failed:") + id;
            return false;
        }
        if (!RenderGuiStable(app, error, true)) {
            error = std::string("workspace_render_failed:") + id + ":" + error;
            return false;
        }
    }
    return true;
}

bool ExerciseNavigationContract(AppState& app, std::string& error) {
    app.ui.ResetNavigation();
    app.ui.NavigateTo("memory-browser", 0x1000);
    app.ui.NavigateTo("disassembly", 0x2000);
    if (!app.ui.CanNavigateBack()) {
        error = "navigation_back_unavailable";
        return false;
    }
    if (!app.ui.NavigateBack() ||
        app.ui.navigationCurrent.workspace != "memory-browser" ||
        app.ui.navigationCurrent.address != 0x1000 ||
        !app.ui.CanNavigateForward()) {
        error = "navigation_back_contract_failed";
        return false;
    }
    if (!app.ui.NavigateForward() ||
        app.ui.navigationCurrent.workspace != "disassembly" ||
        app.ui.navigationCurrent.address != 0x2000) {
        error = "navigation_forward_contract_failed";
        return false;
    }
    app.ui.ResetNavigation();
    return true;
}

bool ParseSmokePid(
        const std::vector<std::string>& args,
        const std::string& name, DWORD& pid,
        std::string& error) {
    std::string value;
    if (!SmokeArgValue(args, name, value)) {
        error = "missing_" + name;
        return false;
    }
    try {
        const unsigned long long parsed = std::stoull(value);
        if (parsed == 0 || parsed > std::numeric_limits<DWORD>::max()) {
            error = "invalid_" + name;
            return false;
        }
        pid = static_cast<DWORD>(parsed);
        return true;
    } catch (...) {
        error = "invalid_" + name;
        return false;
    }
}

uint64_t GuiSmokeCodeAddress(AppState& app, std::string& error) {
    error.clear();

    if (app.debuggerModel.Ready()) {
        if (!app.debuggerModel.RefreshThreads(&error)) return 0;
        const auto& threads = app.debuggerModel.Threads();
        if (!threads.empty() &&
            app.debuggerModel.SelectThread(threads.front(), &error)) {
            const uint64_t ip =
                app.debuggerModel.Snapshot().instructionPointer;
            if (ip) return ip;
        }
        error.clear();
    }

    const auto regions = app.memory.Regions(&error);
    for (const auto& region : regions) {
        if (region.readable && region.executable && region.size >= 64)
            return region.base;
    }
    if (error.empty()) error = "no_readable_executable_region";
    return 0;
}

int RunGuiWorkspaceSuite() {
    AppState app;
    std::string error;
    if (!ValidateGuiRegistry(app, error)) {
        std::fprintf(stderr, "gui workspace suite: %s\n", error.c_str());
        return 2;
    }

    HeadlessGuiContext gui;
    if (!ExerciseGuiPresets(app, error)) {
        std::fprintf(stderr, "gui workspace suite: %s\n", error.c_str());
        return 3;
    }
    if (!ExerciseEveryWorkspace(app, error)) {
        std::fprintf(stderr, "gui workspace suite: %s\n", error.c_str());
        return 4;
    }
    if (!ExerciseNavigationContract(app, error)) {
        std::fprintf(stderr, "gui workspace suite: %s\n", error.c_str());
        return 5;
    }

    std::printf(
        "PASS: GUI workspace suite %zu workspaces / %zu presets / docking / navigation\n",
        static_cast<size_t>(IM_ARRAYSIZE(kGuiRequiredWorkspaces)),
        static_cast<size_t>(IM_ARRAYSIZE(kGuiPresets)));
    return 0;
}

int RunImGuiSmokeTest() {
    const int result = RunGuiWorkspaceSuite();
    if (result != 0) return result;
    std::puts("PASS: deterministic ImGui application smoke");
    return 0;
}

int RunGuiAttachedSuite(const std::vector<std::string>& args) {
    DWORD pid = 0;
    std::string error;
    if (!ParseSmokePid(args, "--pid", pid, error)) {
        std::fprintf(stderr, "--gui-attached-suite: %s\n", error.c_str());
        return 2;
    }

    AppState app;
    if (!AttachSmokeTarget(app, pid, error)) {
        std::fprintf(stderr, "gui attached suite: attach failed: %s\n", error.c_str());
        return 3;
    }

    if (!app.debuggerModel.EnsureAttached(&error)) {
        std::fprintf(stderr, "gui attached suite: debugger attach failed: %s\n", error.c_str());
        return 4;
    }

    const uint64_t address = GuiSmokeCodeAddress(app, error);
    if (!address) {
        std::fprintf(stderr, "gui attached suite: code address failed: %s\n", error.c_str());
        return 5;
    }

    std::vector<uint8_t> bytes;
    if (!app.memory.Read(address, 64, bytes, &error) || bytes.empty()) {
        std::fprintf(stderr, "gui attached suite: memory read failed: %s\n", error.c_str());
        return 6;
    }

    std::vector<cortex::services::DisassemblyInstruction> decoded;
    if (!app.disassembly.Decode(address, 8, decoded, &error) || decoded.empty()) {
        std::fprintf(stderr, "gui attached suite: disassembly failed: %s\n", error.c_str());
        return 7;
    }

    app.ui.NavigateTo("memory-browser", address);
    app.ui.NavigateTo("disassembly", address);

    HeadlessGuiContext gui;
    if (!ExerciseGuiPresets(app, error)) {
        std::fprintf(stderr, "gui attached suite: preset render failed: %s\n", error.c_str());
        return 8;
    }
    if (!ExerciseEveryWorkspace(app, error)) {
        std::fprintf(stderr, "gui attached suite: workspace render failed: %s\n", error.c_str());
        return 9;
    }

    // Give the panel-local live refresh loops enough time to execute at least
    // once while Debug/Mem/Disassembly are visible.
    app.workspaces.ApplyPreset(cortex::ui::WorkspacePreset::Debug);
    app.ui.NavigateTo("disassembly", address);
    if (!RenderGuiStable(app, error, true)) {
        std::fprintf(stderr, "gui attached suite: debug render failed: %s\n", error.c_str());
        return 10;
    }
    Sleep(600);
    if (!RenderGuiStable(app, error, true)) {
        std::fprintf(stderr, "gui attached suite: live refresh render failed: %s\n", error.c_str());
        return 11;
    }

    std::printf(
        "PASS: GUI attached suite pid=%lu address=0x%llX bytes=%zu instructions=%zu\n",
        static_cast<unsigned long>(pid),
        static_cast<unsigned long long>(address),
        bytes.size(), decoded.size());
    return 0;
}

int RunGuiMultiSessionSuite(const std::vector<std::string>& args) {
    DWORD pidA = 0;
    DWORD pidB = 0;
    std::string error;
    if (!ParseSmokePid(args, "--pid-a", pidA, error) ||
        !ParseSmokePid(args, "--pid-b", pidB, error) ||
        pidA == pidB) {
        if (error.empty()) error = "multi_session_pids_must_differ";
        std::fprintf(stderr, "--gui-multi-session-suite: %s\n", error.c_str());
        return 2;
    }

    AppState app;
    if (!AttachSmokeTarget(app, pidA, error)) {
        std::fprintf(stderr, "gui multi-session suite: attach A failed: %s\n", error.c_str());
        return 3;
    }
    const auto first = app.sessions.ActiveTarget();
    if (!first) {
        std::fputs("gui multi-session suite: first target missing\n", stderr);
        return 4;
    }
    const cortex::target::TargetDescriptor targetA = *first;

    if (!AttachSmokeTarget(app, pidB, error)) {
        std::fprintf(stderr, "gui multi-session suite: attach B failed: %s\n", error.c_str());
        return 5;
    }
    const auto second = app.sessions.ActiveTarget();
    if (!second) {
        std::fputs("gui multi-session suite: second target missing\n", stderr);
        return 6;
    }
    const cortex::target::TargetDescriptor targetB = *second;

    if (app.sessions.SessionCount() != 2 ||
        app.sessions.AttachedTargets().size() != 2) {
        std::fprintf(stderr, "gui multi-session suite: expected 2 sessions, got %zu\n",
                     app.sessions.SessionCount());
        return 7;
    }

    HeadlessGuiContext gui;
    const cortex::target::TargetDescriptor targets[] = {targetA, targetB};
    for (int round = 0; round < 3; ++round) {
        for (const auto& target : targets) {
            if (!app.sessions.Activate(target.id)) {
                std::fprintf(stderr, "gui multi-session suite: activate failed: %s\n",
                             target.id.c_str());
                return 8;
            }
            app.OnAttached(target, false);
            app.workspaces.ApplyPreset(
                round % 2 == 0
                    ? cortex::ui::WorkspacePreset::Memory
                    : cortex::ui::WorkspacePreset::Debug);
            if (!RenderGuiStable(app, error, true)) {
                std::fprintf(stderr,
                    "gui multi-session suite: render failed for pid %llu: %s\n",
                    static_cast<unsigned long long>(target.processId),
                    error.c_str());
                return 9;
            }
        }
    }

    app.workspaces.Select("sessions");
    if (!RenderGuiStable(app, error, true)) {
        std::fprintf(stderr, "gui multi-session suite: sessions view failed: %s\n",
                     error.c_str());
        return 10;
    }

    std::printf("PASS: GUI multi-session suite pidA=%lu pidB=%lu switches=6\n",
                static_cast<unsigned long>(pidA),
                static_cast<unsigned long>(pidB));
    return 0;
}

int RunGuiCrossBitnessSuite(const std::vector<std::string>& args) {
    DWORD pid = 0;
    std::string error;
    if (!ParseSmokePid(args, "--pid", pid, error)) {
        std::fprintf(stderr, "--gui-cross-bitness-suite: %s\n", error.c_str());
        return 2;
    }

    AppState app;
    if (!AttachSmokeTarget(app, pid, error)) {
        std::fprintf(stderr, "gui cross-bitness suite: attach failed: %s\n", error.c_str());
        return 3;
    }
    const auto target = app.sessions.ActiveTarget();
    if (!target || target->architecture != cortex::target::Architecture::X86) {
        std::fputs("gui cross-bitness suite: target is not x86\n", stderr);
        return 4;
    }

    if (!app.payload.RuntimeSupportAvailable(&error)) {
        std::fprintf(stderr, "gui cross-bitness suite: support unavailable: %s\n",
                     error.c_str());
        return 5;
    }

    app.ui.mutationAllowed = true;
    if (!app.payload.EnsureReady(&error)) {
        std::fprintf(stderr, "gui cross-bitness suite: runtime enable failed: %s\n",
                     error.c_str());
        return 6;
    }
    if (!app.payload.Ready() || app.payload.TargetProcessId() != pid) {
        std::fputs("gui cross-bitness suite: payload verification failed\n", stderr);
        return 7;
    }

    app.payload.Reset();
    if (!app.payload.TryConnectExisting(&error)) {
        std::fprintf(stderr, "gui cross-bitness suite: reconnect failed: %s\n",
                     error.c_str());
        return 8;
    }

    HeadlessGuiContext gui;
    app.workspaces.ApplyPreset(cortex::ui::WorkspacePreset::ReverseEngineering);
    if (!RenderGuiStable(app, error, true)) {
        std::fprintf(stderr, "gui cross-bitness suite: RE render failed: %s\n",
                     error.c_str());
        return 9;
    }
    app.workspaces.ApplyPreset(cortex::ui::WorkspacePreset::Runtime);
    if (!RenderGuiStable(app, error, true)) {
        std::fprintf(stderr, "gui cross-bitness suite: runtime render failed: %s\n",
                     error.c_str());
        return 10;
    }

    std::printf("PASS: GUI cross-bitness x64->x86 suite pid=%lu runtime-ready\n",
                static_cast<unsigned long>(pid));
    return 0;
}

std::string Trim(std::string value) {
    auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

bool ParseAddressToken(const std::string& text, uint64_t& value, int defaultBase = 0) {
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

    const size_t plus = value.find_last_of('+');
    if (plus == std::string::npos || plus == 0 || plus + 1 >= value.size()) {
        error = "Address not recognized";
        return false;
    }

    const std::string moduleName = Lower(Trim(value.substr(0, plus)));
    uint64_t offset = 0;
    if (!ParseAddressToken(value.substr(plus + 1), offset, 16)) {
        error = "Invalid module offset";
        return false;
    }

    std::string moduleError;
    const auto modules = app.modules.List(&moduleError);
    for (const auto& module : modules) {
        if (Lower(module.name) != moduleName) continue;
        if (offset > std::numeric_limits<uint64_t>::max() - module.base) {
            error = "Address overflow";
            return false;
        }
        address = module.base + offset;
        return true;
    }

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

    error = moduleError.empty() ? "Address, module, project or symbol not found" : moduleError;
    return false;
}

void NavigateTo(AppState& app, uint64_t address, const char* workspace) {
    app.ui.NavigateTo(workspace ? workspace : "memory-browser", address);
}

void ResetTargetState(AppState& app) {
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
    GoTo
};

struct CommandEntry {
    const char* label;
    CommandAction action;
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
    {"Navigate: Go to address", CommandAction::GoTo}
};

bool CommandMatches(const char* label, const char* filter) {
    if (!filter || !*filter) return true;
    return Lower(label ? label : "").find(Lower(filter)) != std::string::npos;
}

void ExecuteCommand(AppState& app, CommandAction action) {
    using cortex::ui::WorkspacePreset;
    switch (action) {
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

    ImGui::SetNextWindowSize(ImVec2(640, 460), ImGuiCond_Appearing);
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
            ExecuteCommand(app, command.action);
            ImGui::CloseCurrentPopup();
            break;
        }
    }
    ImGui::EndChild();

    if (enter && firstMatch) {
        ExecuteCommand(app, firstMatch->action);
        ImGui::CloseCurrentPopup();
    }

    if (ImGui::Button("Close", ImVec2(100, 28))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void DrawGoTo(AppState& app) {
    if (app.requestGoTo) {
        ImGui::OpenPopup("Go to");
        app.requestGoTo = false;
    }

    ImGui::SetNextWindowSize(ImVec2(520, 190), ImGuiCond_Appearing);
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

    if ((ImGui::Button("Memory", ImVec2(130, 32)) || enter) && go("memory-browser"))
        ImGui::CloseCurrentPopup();
    ImGui::SameLine();
    if (ImGui::Button("Disassembly", ImVec2(130, 32)) && go("disassembly"))
        ImGui::CloseCurrentPopup();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100, 32))) ImGui::CloseCurrentPopup();

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

    ImGui::SetNextWindowSize(ImVec2(760, 580), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Select process", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;

    ImGui::TextUnformatted("Choose a process");
    ImGui::SameLine();
    ImGui::TextDisabled("Double-click to attach");
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh")) app.RefreshTargets();
    ImGui::Spacing();

    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##ProcessFilter", "Search process, window title or PID...",
                             app.processFilter, sizeof(app.processFilter));
    ImGui::Spacing();

    const float footerHeight = 54.0f;
    if (ImGui::BeginTable("ProcessTable", 5,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                          ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
                          ImVec2(0, -footerHeight))) {
        ImGui::TableSetupColumn("Process", ImGuiTableColumnFlags_WidthStretch, 0.42f);
        ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, 78.0f);
        ImGui::TableSetupColumn("Arch", ImGuiTableColumnFlags_WidthFixed, 72.0f);
        ImGui::TableSetupColumn("Window", ImGuiTableColumnFlags_WidthStretch, 0.50f);
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 82.0f);
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
    if (ImGui::Button("Attach", ImVec2(150, 38)) && validSelection) {
        AttachTarget(app, app.targets[static_cast<size_t>(app.selectedTarget)]);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(110, 38))) ImGui::CloseCurrentPopup();

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

    ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);
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
    if (ImGui::Button(timedTest ? "Submit result" : "Done", ImVec2(130, 34))) {
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

void DrawSessionStrip(AppState& app) {
    const auto attached = app.sessions.AttachedTargets();
    if (attached.empty()) return;

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
        if (ImGui::SmallButton(label.c_str()))
            ActivateAttachedTarget(app, target);

        if (active) ImGui::PopStyleColor();

        if (ImGui::BeginPopupContextItem("SessionMenu")) {
            if (!active && ImGui::MenuItem("Activate"))
                ActivateAttachedTarget(app, target);
            if (ImGui::MenuItem("Detach")) {
                const bool wasActive = active;
                app.sessions.Detach(target.id);
                if (wasActive) {
                    const auto remaining = app.sessions.AttachedTargets();
                    if (!remaining.empty()) {
                        app.sessions.Activate(remaining.front().id);
                        app.OnAttached(remaining.front(), false);
                    } else {
                        ResetTargetState(app);
                    }
                }
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
}

void DrawQuickToolbar(AppState& app) {
    const bool attached = static_cast<bool>(app.sessions.Active());

    if (ImGui::SmallButton("+ Process"))
        app.ui.requestProcessPicker = true;
    ImGui::SameLine();
    if (ImGui::SmallButton("Sessions"))
        app.workspaces.Select("sessions");

    ImGui::SameLine();
    ImGui::BeginDisabled(!attached);
    if (ImGui::SmallButton("Memory"))
        app.workspaces.Select("memory-browser");
    ImGui::SameLine();
    if (ImGui::SmallButton("Disasm"))
        app.workspaces.Select("disassembly");
    ImGui::SameLine();
    if (ImGui::SmallButton("Debugger"))
        app.workspaces.Select("debugger");
    ImGui::SameLine();
    if (ImGui::SmallButton("Modules"))
        app.workspaces.Select("modules");
    ImGui::SameLine();
    if (ImGui::SmallButton("Runtime"))
        app.workspaces.Select("runtime");
    ImGui::SameLine();
    if (ImGui::SmallButton("Go to"))
        app.requestGoTo = true;
    ImGui::EndDisabled();

    if (attached && app.debuggerModel.Ready()) {
        ImGui::SameLine();
        ImGui::TextDisabled("| debug");
        ImGui::SameLine();
        ImGui::BeginDisabled(!app.ui.mutationAllowed ||
                             app.debuggerModel.CurrentThread() == 0);
        if (ImGui::SmallButton("Pause")) {
            std::string error;
            if (!app.debuggerModel.Pause(&error))
                app.ui.status = "Pause failed: " + error;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Continue")) {
            std::string error;
            if (!app.debuggerModel.Resume(&error))
                app.ui.status = "Continue failed: " + error;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Step")) {
            std::string error;
            if (!app.debuggerModel.Step(2000, &error))
                app.ui.status = "Step failed: " + error;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Over")) {
            std::string error;
            if (!app.debuggerModel.StepOver(5000, &error))
                app.ui.status = "Step over failed: " + error;
        }
        ImGui::EndDisabled();
    }
}

void DrawHeader(AppState& app) {
    const auto session = app.sessions.Active();

    ImGui::SetWindowFontScale(1.22f);
    ImGui::TextUnformatted("CORTEX");
    ImGui::SetWindowFontScale(1.0f);

    ImGui::SameLine(120.0f);
    if (ImGui::Button(session ? "Change process" : "Select process", ImVec2(150, 34))) {
        app.ui.requestProcessPicker = true;
    }

    ImGui::SameLine();
    if (session) {
        const auto& target = session->Target();
        ImGui::Text("%s  |  PID %llu  |  %s  |  %zu session(s)",
                    target.name.c_str(),
                    static_cast<unsigned long long>(target.processId),
                    cortex::target::ArchitectureName(target.architecture),
                    app.sessions.SessionCount());

        const float rightWidth = 240.0f;
        const float available = ImGui::GetContentRegionAvail().x;
        if (available > rightWidth) {
            ImGui::SameLine(ImGui::GetCursorPosX() + available - rightWidth);
        }

        ImGui::Checkbox("Allow writes", &app.ui.mutationAllowed);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Required for edits, freeze and runtime injection. Read-only inspection stays available.");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Detach")) {
            ResetTargetState(app);
        }
    } else {
        ImGui::SameLine();
        ImGui::TextDisabled("No process attached");
    }

    if (app.settings.Values().showAiActivityInTitleBar) {
        ImGui::SameLine();
        if (app.aiActivityModel.Connected()) {
            ImGui::Text("AI %zu session(s) / %zu active",
                        app.aiActivityModel.SessionCount(),
                        app.aiActivityModel.ActiveTaskCount());
        } else {
            ImGui::TextDisabled(app.aiActivityModel.Listening()
                                    ? "AI idle" : "AI listener unavailable");
        }
    }

    DrawSessionStrip(app);

    if (session && !app.payload.Ready()) {
        std::string runtimeReason;
        if (!app.payload.RuntimeSupportAvailable(&runtimeReason)) {
            ImGui::TextDisabled("Runtime support: %s",
                cortex::ui::RuntimeSupportText(runtimeReason).c_str());
        }
    }

    DrawQuickToolbar(app);
    ImGui::Separator();
}

void DrawApp(AppState& app) {
    app.aiActivityModel.Poll(app.settings.Values().aiActivityHistoryLimit);

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
            const bool attached = static_cast<bool>(app.sessions.Active());
            ImGui::BeginDisabled(!attached);
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
                app.workspaces.Select("bottom");
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

    ImGui::TextDisabled("Workspace");
    ImGui::SameLine();
    app.workspaces.DrawPresetButtons();
    ImGui::Separator();

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

} // namespace

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    const int cli = HandleCommandLine();
    if (cli >= 0) return cli;

    const auto launchArgs = CurrentCommandLineArgs();
    const bool windowSmoke =
        std::find(launchArgs.begin(), launchArgs.end(), "--window-smoke-test") !=
        launchArgs.end();

    ImGui_ImplWin32_EnableDpiAwareness();

    WNDCLASSEXW wc{
        sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, hInstance,
        nullptr, nullptr, nullptr, nullptr, L"CortexWindow", nullptr
    };
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowW(
        wc.lpszClassName, L"Cortex",
        WS_OVERLAPPEDWINDOW, 100, 80, 1380, 880,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ShowWindow(hwnd, windowSmoke ? SW_HIDE : SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = windowSmoke ? nullptr : "cortex-ui.ini";

    cortex::ui::ApplyCortexTheme();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(gDevice, gDeviceContext);

    AppState app;
    bool done = false;
    int smokeFrames = 0;
    int windowSmokePreset = 0;
    std::string windowSmokeError;
    const ULONGLONG windowSmokeStarted = GetTickCount64();
    while (!done) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        HandleGlobalShortcuts(app);
        if (windowSmoke && smokeFrames % 3 == 0 &&
            windowSmokePreset < static_cast<int>(IM_ARRAYSIZE(kGuiPresets))) {
            app.workspaces.ApplyPreset(kGuiPresets[windowSmokePreset]);
            ++windowSmokePreset;
        }
        DrawApp(app);

        ImGui::Render();
        if (windowSmoke && windowSmokeError.empty() &&
            !app.workspaces.ValidateOpenWindowsDocked(&windowSmokeError)) {
            std::fprintf(stderr, "window smoke: %s\n", windowSmokeError.c_str());
            done = true;
            smokeFrames = -100;
        }
        constexpr float clearColor[4] = {0.055f, 0.064f, 0.078f, 1.0f};
        gDeviceContext->OMSetRenderTargets(1, &gMainRenderTargetView, nullptr);
        gDeviceContext->ClearRenderTargetView(gMainRenderTargetView, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        const HRESULT presentResult = gSwapChain->Present(windowSmoke ? 0 : 1, 0);
        if (windowSmoke) {
            if (FAILED(presentResult)) {
                std::fprintf(stderr, "window smoke: Present failed (0x%08lX)\n",
                             static_cast<unsigned long>(presentResult));
                done = true;
                smokeFrames = -100;
            } else {
                ++smokeFrames;
                const int requiredFrames =
                    static_cast<int>(IM_ARRAYSIZE(kGuiPresets)) * 3;
                if (smokeFrames >= requiredFrames &&
                    windowSmokePreset >= static_cast<int>(IM_ARRAYSIZE(kGuiPresets)) &&
                    GetTickCount64() - windowSmokeStarted >= 500)
                    done = true;
            }
        }
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    if (windowSmoke) {
        if (smokeFrames < static_cast<int>(IM_ARRAYSIZE(kGuiPresets)) * 3)
            return 5;
        std::puts("PASS: native Win32 D3D11 ImGui window smoke (all presets docked)");
    }
    return 0;
}
