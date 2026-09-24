#include "application/actions_model.h"
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
#include "application/snapshots_model.h"
#include "application/structures_model.h"
#include "application/symbols_model.h"
#include "application/watches_model.h"
#include "application/scripts_model.h"
#include "application/screenshot_model.h"
#include "ui/actions_workspace.h"
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
    const HRESULT result = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        featureLevels, 2, D3D11_SDK_VERSION, &sd,
        &gSwapChain, &gDevice, &featureLevel, &gDeviceContext);

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
    cortex::application::NetworkModel networkModel;
    cortex::application::DiagnosticsModel diagnosticsModel;
    cortex::application::ScriptsModel scriptsModel;
    cortex::application::InputModel inputModel;
    cortex::application::ScreenshotModel screenshotModel;
    cortex::application::RuntimeEventsModel runtimeEventsModel;
    cortex::application::PatchesModel patchesModel;
    cortex::ui::UiContext ui;
    cortex::ui::WorkspaceRegistry workspaces;

    std::vector<cortex::target::TargetDescriptor> targets;
    int selectedTarget = -1;
    char processFilter[160] = {};
    bool requestCommandPalette = false;
    bool requestGoTo = false;
    char commandFilter[160] = {};
    char goToExpression[160] = {};

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
          diagnosticsModel(payload),
          scriptsModel(payload),
          inputModel(payload),
          screenshotModel(payload),
          runtimeEventsModel(payload),
          patchesModel(payload) {
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
        ui.networkModel = &networkModel;
        ui.diagnosticsModel = &diagnosticsModel;
        ui.scriptsModel = &scriptsModel;
        ui.inputModel = &inputModel;
        ui.screenshotModel = &screenshotModel;
        ui.runtimeEventsModel = &runtimeEventsModel;
        ui.patchesModel = &patchesModel;
        ui.nativeRenderDevice = gDevice;

        workspaces.Add<cortex::ui::OverviewWorkspace>();
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
        workspaces.ApplyPreset(cortex::ui::WorkspacePreset::Memory);
    }

    void OnAttached(const cortex::target::TargetDescriptor& target) {
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
        ui.mutationAllowed = false;
        ui.ResetNavigation();
        ui.status = "Attached to " + target.name;
        ui.requestWorkspace = "memory";
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
    {"View: Memory scanner / addresses", CommandAction::ViewMemory},
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

    ImGui::Separator();
}

void DrawApp(AppState& app) {
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
    ImGui::DockSpace(dockspaceId, dockSize, ImGuiDockNodeFlags_None);

    ImGui::Separator();
    ImGui::TextDisabled("[%s]  %s",
                        cortex::ui::WorkspaceRegistry::PresetName(app.workspaces.Preset()),
                        app.ui.status.c_str());

    DrawProcessPicker(app);
    DrawCommandPalette(app);
    DrawGoTo(app);
    ImGui::End();

    app.workspaces.DrawDockWindows(app.ui);
}

} // namespace

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    const int cli = HandleCommandLine();
    if (cli >= 0) return cli;

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

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = "cortex-ui.ini";

    cortex::ui::ApplyCortexTheme();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(gDevice, gDeviceContext);

    AppState app;
    bool done = false;
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
        DrawApp(app);

        ImGui::Render();
        constexpr float clearColor[4] = {0.055f, 0.064f, 0.078f, 1.0f};
        gDeviceContext->OMSetRenderTargets(1, &gMainRenderTargetView, nullptr);
        gDeviceContext->ClearRenderTargetView(gMainRenderTargetView, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        gSwapChain->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}
