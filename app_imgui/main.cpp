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
#include <string>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

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
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return -1;
    int result = -1;
    if (argc > 1) {
        const std::wstring arg = argv[1] ? argv[1] : L"";
        if (arg == L"--version" || arg == L"version") {
            WriteStdout("cortex 0.8.0-dev-imgui\n");
            result = 0;
        } else if (arg == L"--help" || arg == L"-h" || arg == L"help") {
            WriteStdout(
                "Cortex lightweight UI preview\n\n"
                "Usage:\n"
                "  cortex              Launch the GUI\n"
                "  cortex --version    Print version\n"
                "  cortex --help       Print this help\n");
            result = 0;
        }
    }
    LocalFree(argv);
    return result;
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
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("CortexShell", nullptr, flags);
    ImGui::PopStyleVar(2);

    DrawHeader(app);

    const float statusHeight = ImGui::GetTextLineHeightWithSpacing() + 12.0f;
    ImGui::BeginChild("WorkspaceBody", ImVec2(0, -statusHeight), ImGuiChildFlags_None);
    app.workspaces.DrawNavigation();
    app.workspaces.DrawActive(app.ui);
    ImGui::EndChild();

    ImGui::Separator();
    ImGui::TextDisabled("%s", app.ui.status.c_str());

    DrawProcessPicker(app);
    ImGui::End();
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
