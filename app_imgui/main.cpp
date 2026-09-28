// Cortex desktop entry point: Win32 window, Direct3D 11 device and the
// frame loop. Everything else lives in desktop_ui.cpp, cli.cpp and
// smoke_modes.cpp.

#include "desktop_app.h"

#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <d3d11.h>
#include <windows.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

using namespace cortex::desktop;

ID3D11Device* gDevice = nullptr;
ID3D11DeviceContext* gDeviceContext = nullptr;
IDXGISwapChain* gSwapChain = nullptr;
ID3D11RenderTargetView* gMainRenderTargetView = nullptr;

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

// Set by WM_DPICHANGED and applied between frames, never while ImGui is
// building a frame.
float gPendingDpiScale = 0.0f;
// WM_HOTKEY ids received since the last frame.
std::vector<int> gHotkeyIds;

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
        case WM_DPICHANGED: {
            gPendingDpiScale = static_cast<float>(HIWORD(wParam)) / 96.0f;
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_HOTKEY:
            gHotkeyIds.push_back(static_cast<int>(wParam));
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}


}  // namespace

void* cortex::desktop::NativeRenderDevice() { return gDevice; }

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    const int cli = HandleCommandLine();
    if (cli >= 0) return cli;

    const auto launchArgs = CurrentCommandLineArgs();
    const bool windowSmoke =
        std::find(launchArgs.begin(), launchArgs.end(), "--window-smoke-test") !=
        launchArgs.end();

    ImGui_ImplWin32_EnableDpiAwareness();

    // Class icons for the title bar, taskbar and Alt+Tab (the executable's
    // own icon only covers Explorer).
    auto loadIcon = [hInstance](int width, int height) {
        return static_cast<HICON>(LoadImageW(hInstance, L"IDI_CORTEX_ICON", IMAGE_ICON,
                                             width, height, LR_DEFAULTCOLOR));
    };
    WNDCLASSEXW wc{
        sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, hInstance,
        loadIcon(GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON)),
        LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)), nullptr, nullptr, L"CortexWindow",
        loadIcon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON))
    };
    RegisterClassExW(&wc);

    // Size the first window for the primary monitor's DPI so a 150% or 200%
    // display does not open a cramped window.
    const float startupScale = ImGui_ImplWin32_GetDpiScaleForMonitor(
        MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
    const float windowScale = startupScale > 0.0f ? startupScale : 1.0f;
    HWND hwnd = CreateWindowW(
        wc.lpszClassName, L"Cortex",
        WS_OVERLAPPEDWINDOW, 100, 80,
        static_cast<int>(1380 * windowScale), static_cast<int>(880 * windowScale),
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
    // The window layout is per-user state; keep it out of the working
    // directory (see UserDataDirectory).
    static std::string layoutPath;
    if (!windowSmoke) {
        const std::filesystem::path appDirectory = std::filesystem::u8path(ExecutableDirectory());
        layoutPath = cortex::services::UserDataFile(
            appDirectory, cortex::services::UserDataDirectory(appDirectory), "cortex-ui.ini").u8string();
    }
    io.IniFilename = windowSmoke ? nullptr : layoutPath.c_str();

    cortex::ui::LoadFonts();
    cortex::ui::ApplyDpiScale(ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd));

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(gDevice, gDeviceContext);

    AppState app;
    app.window = hwnd;
    if (!windowSmoke) {
        app.ui.runInBackground = [&app](std::string label, std::function<void()> work) {
            // A task started while another runs (not reachable from the UI,
            // which is hidden meanwhile) simply runs inline.
            if (app.background.Active()) {
                work();
                return;
            }
            app.background.Start(std::move(label), std::move(work));
        };
    }
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

        if (!windowSmoke) {
            // Settings hotkeys plus the per-entry hotkeys of the address list.
            auto bindings = app.settings.Values().hotkeys;
            bindings.insert(app.ui.entryHotkeys.begin(), app.ui.entryHotkeys.end());
            app.ui.hotkeyFailures = app.hotkeys.Apply(hwnd, bindings);
            for (const int id : gHotkeyIds) {
                if (app.background.Active()) break;
                for (const auto& action : app.hotkeys.ActionsFor(id)) DispatchCommand(app, action);
            }
        }
        gHotkeyIds.clear();

        if (gPendingDpiScale > 0.0f) {
            cortex::ui::ApplyDpiScale(gPendingDpiScale);
            gPendingDpiScale = 0.0f;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        if (!app.background.Active()) HandleGlobalShortcuts(app);
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
