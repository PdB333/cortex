#include "ui/debugger_workspace.h"
#include "ui/disassembly_workspace.h"
#include "ui/memory_browser_workspace.h"
#include "ui/memory_workspace.h"
#include "ui/modules_workspace.h"
#include "ui/runtime_workspace.h"
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
#include <filesystem>
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
    cortex::ui::UiContext ui;
    cortex::ui::WorkspaceRegistry workspaces;

    std::vector<cortex::target::TargetDescriptor> targets;
    int selectedTarget = -1;
    char processFilter[160] = {};

    AppState()
        : sessions(catalog),
          memory(sessions),
          modules(sessions),
          disassembly(sessions),
          debugger(sessions),
          payload(sessions, ExecutableDirectory()) {
        catalog.AddBackend(std::make_shared<cortex::target::LocalBackend>());

        ui.sessions = &sessions;
        ui.memory = &memory;
        ui.modules = &modules;
        ui.disassembly = &disassembly;
        ui.debugger = &debugger;
        ui.payload = &payload;

        workspaces.Add<cortex::ui::MemoryWorkspace>();
        workspaces.Add<cortex::ui::MemoryBrowserWorkspace>();
        workspaces.Add<cortex::ui::DisassemblyWorkspace>();
        workspaces.Add<cortex::ui::ModulesWorkspace>();
        workspaces.Add<cortex::ui::DebuggerWorkspace>();
        workspaces.Add<cortex::ui::RuntimeWorkspace>();
        workspaces.ApplyPreset(cortex::ui::WorkspacePreset::Memory);
    }

    void OnAttached(const cortex::target::TargetDescriptor& target) {
        payload.Reset();
        ui.mutationAllowed = false;
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
    if (ImGui::BeginTable("ProcessTable", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                          ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
                          ImVec2(0, -footerHeight))) {
        ImGui::TableSetupColumn("Process", ImGuiTableColumnFlags_WidthStretch, 0.42f);
        ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, 78.0f);
        ImGui::TableSetupColumn("Arch", ImGuiTableColumnFlags_WidthFixed, 72.0f);
        ImGui::TableSetupColumn("Window", ImGuiTableColumnFlags_WidthStretch, 0.58f);
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
        ImGui::Text("%s  |  PID %llu  |  %s",
                    target.name.c_str(),
                    static_cast<unsigned long long>(target.processId),
                    cortex::target::ArchitectureName(target.architecture));

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
            app.sessions.Detach();
            app.payload.Reset();
            app.ui.mutationAllowed = false;
            app.ui.status = "Detached";
            app.ui.requestWorkspace = "memory";
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
                app.sessions.Detach();
                app.payload.Reset();
                app.ui.mutationAllowed = false;
                app.ui.status = "Detached";
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
