// GUI smoke and integration test modes (cortex --gui-*-suite, --*-smoke).
// Built when CORTEX_WITH_TEST_MODES is on.

#include "desktop_app.h"
#include "ui/address_resolver.h"

#include "services/assembler.h"
#include "services/auto_assembler.h"
#include "services/custom_types.h"
#include "services/memory_tools.h"
#include "services/structure_dissect.h"
#include "services/speedhack.h"
#include "ui/auto_assembler_host.h"
#include "process/remote_memory.h"

#include <fstream>

namespace cortex::desktop {

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
    "scripts", "input", "screenshots", "patches", "events", "trace", "tools",
    "access-finder", "lua"
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

// Value scanner, PE parser, signature generator and pointer scanner against
// the real test target.
bool ExerciseMemoryTools(AppState& app, uint64_t codeAddress, std::string& summary, std::string& error) {
    namespace services = cortex::services;
    const auto session = app.sessions.Active();
    auto modules = app.modules.List(&error);
    if (modules.empty()) {
        if (error.empty()) error = "no_modules";
        return false;
    }
    const auto main = std::find_if(modules.begin(), modules.end(), [](const auto& module) {
        return Lower(module.name).find("cortex_test_target") != std::string::npos;
    });
    const auto& module = main != modules.end() ? *main : modules.front();
    const services::MemoryReader read = [session](uint64_t address, void* buffer, size_t size) {
        return session->ReadMemory(address, buffer, size, nullptr);
    };
    services::PeImage image;
    if (!services::ParsePeImage(read, module.base, image, &error) || image.sections.empty()) {
        error = "pe_parse_failed:" + error;
        return false;
    }
    uint64_t health = 0;
    uint64_t frame = 0;
    uint64_t stepOver = 0;
    uint64_t ticks = 0;
    for (const auto& entry : image.exports) {
        if (entry.name == "g_cortex_health") health = module.base + entry.rva;
        if (entry.name == "g_cortex_frame") frame = module.base + entry.rva;
        if (entry.name == "CortexStepOverCaller") stepOver = module.base + entry.rva;
        if (entry.name == "g_cortex_ticks") ticks = module.base + entry.rva;
    }

    // Find out what writes to the frame counter: a log-mode hardware
    // breakpoint through the access finder workspace.
    if (frame && app.debuggerModel.Ready()) {
        app.ui.mutationAllowed = true;
        app.ui.accessFinderRequests.push_back({frame, 4, true});
        app.workspaces.Select("access-finder");
        size_t hits = 0;
        for (int attempt = 0; attempt < 20 && hits == 0; ++attempt) {
            if (!RenderGuiStable(app, error, true)) return false;
            Sleep(150);
            for (const auto& breakpoint : app.debuggerModel.Breakpoints())
                if (breakpoint.address == frame && breakpoint.kind == "hw_write") hits = breakpoint.hitCount;
            std::vector<DebugBreakpointLogEntry> entries;
            for (const auto& breakpoint : app.debuggerModel.Breakpoints()) {
                if (breakpoint.address != frame) continue;
                std::string logError;
                if (app.debuggerModel.LoadBreakpointLog(breakpoint.id, 0, 16, entries, &logError))
                    hits = std::max(hits, entries.size());
            }
        }
        for (const auto& breakpoint : app.debuggerModel.Breakpoints())
            if (breakpoint.address == frame) app.debuggerModel.RemoveBreakpoint(breakpoint.id, nullptr);
        // Hits still queued for the removed breakpoint must not reach the
        // target: it keeps running.
        Sleep(300);
        app.ui.mutationAllowed = false;
        if (!session->Alive()) {
            error = "target_died_after_access_finder";
            return false;
        }
        if (hits == 0) {
            error = "access_finder_recorded_no_write";
            return false;
        }
        summary += " frame_writes=" + std::to_string(hits);
    }

    size_t scanHits = 0;
    if (health) {
        int32_t value = 0;
        bool read = false;
        for (int attempt = 0; attempt < 5 && !read; ++attempt) {
            read = session->ReadMemory(health, &value, sizeof(value), nullptr);
            if (!read) Sleep(100);
        }
        if (!read) {
            error = "health_read_failed";
            return false;
        }
        services::ScanQuery query;
        query.type = services::ScanDataType::Int32;
        query.value = std::to_string(value);
        services::ScanOptions options;
        options.start = module.base;
        options.stop = module.base + module.size;
        options.writable = services::ScanTristate::Any;
        options.copyOnWrite = services::ScanTristate::Any;
        auto first = services::ValueScanner::FirstScan(session, query, options, &error);
        auto contains = [health](const services::ScanStatePtr& state) {
            return state && std::find(state->addresses.begin(), state->addresses.end(), health) != state->addresses.end();
        };
        if (!contains(first)) {
            error = "value_scan_missed_health:" + error;
            return false;
        }
        query.compare = services::ScanCompare::Unchanged;
        auto unchanged = services::ValueScanner::NextScan(session, first, query, &error);
        query.compare = services::ScanCompare::Changed;
        auto changed = services::ValueScanner::NextScan(session, first, query, &error);
        if (!contains(unchanged) && !contains(changed)) {
            error = "next_scan_lost_health";
            return false;
        }
        scanHits = first->Size();

        // The desktop Lua engine reads the same value through a module+offset
        // expression, without the runtime.
        cortex::application::LuaRunOptions lua;
        lua.session = session;
        lua.modules = [&app]() { return app.modules.List(nullptr); };
        char script[160] = {};
        std::snprintf(script, sizeof(script), "return readInteger(\"%s+%llX\", true)", module.name.c_str(),
                      static_cast<unsigned long long>(health - module.base));
        const auto luaResult = cortex::application::RunDesktopLua(script, lua);
        if (!luaResult.ok || luaResult.returned.empty()) {
            error = "lua_read_failed:" + luaResult.error;
            return false;
        }
        summary += " lua=" + luaResult.returned;

        // Address expressions resolve exports by name, module.Export and
        // user-defined symbols, like Cheat Engine's symbol handler.
        uint64_t resolved = 0;
        std::string expressionError;
        if (!cortex::ui::EvaluateContextAddress(app.ui, "g_cortex_health", resolved, &expressionError) ||
            resolved != health ||
            !cortex::ui::EvaluateContextAddress(app.ui, module.name + ".g_cortex_health", resolved, &expressionError) ||
            resolved != health) {
            error = "expression_export_failed:" + expressionError;
            return false;
        }
        app.ui.userSymbols->Set("smoke_health", health);
        const bool symbolResolved = cortex::ui::EvaluateContextAddress(app.ui, "smoke_health+4", resolved, &expressionError) &&
                                    resolved == health + 4;
        app.ui.userSymbols->Remove("smoke_health");
        if (!symbolResolved) {
            error = "expression_symbol_failed:" + expressionError;
            return false;
        }
        summary += " expressions=ok";

        services::PointerScanOptions pointerOptions;
        pointerOptions.target = health;
        pointerOptions.maxLevel = 2;
        pointerOptions.maxResults = 100;
        pointerOptions.pointerSize = session->Target().architecture == cortex::target::Architecture::X86 ? 4 : 8;
        for (const auto& entry : modules) pointerOptions.modules.push_back({entry.name, entry.base, entry.size});
        services::PointerScanResult pointers;
        if (!services::PointerScanner::Scan(session, pointerOptions, pointers, &error)) {
            error = "pointer_scan_failed:" + error;
            return false;
        }
        summary += " pointers=" + std::to_string(pointers.paths.size());

        // Grouped scan: the two words around the health value are searched
        // for together, the way Cheat Engine matches a structure.
        std::vector<uint8_t> window(0x400);
        const uint64_t windowBase = health - 0x100;
        if (!session->ReadMemory(windowBase, window.data(), window.size(), nullptr)) {
            error = "grouped_read_failed";
            return false;
        }
        uint32_t first32 = 0;
        uint32_t second32 = 0;
        std::memcpy(&first32, window.data() + 0x100, 4);
        std::memcpy(&second32, window.data() + 0x104, 4);
        char pattern[64] = {};
        std::snprintf(pattern, sizeof(pattern), "4:%X 4:%X", first32, second32);
        std::vector<services::GroupedElement> elements;
        if (!services::ParseGroupedScan(pattern, 4, elements, &error) || elements.size() != 2) {
            error = "grouped_parse_failed:" + error;
            return false;
        }
        std::vector<services::GroupedHit> groupedHits;
        services::FindGroupedValues(window.data(), window.size(), windowBase, elements, 32, true, 64, groupedHits);
        const bool foundHealth = std::any_of(groupedHits.begin(), groupedHits.end(),
                                             [health](const auto& hit) { return hit.address == health; });
        if (!foundHealth) {
            error = "grouped_scan_missed_health";
            return false;
        }
        summary += " grouped=" + std::to_string(groupedHits.size());

        // A memory dump of the page holding the health value, read back and
        // compared, then a user-defined type reading the same bytes.
        const std::string dumpPath = "cortex_smoke_dump.bin";
        services::MemoryDumpReport dump;
        const services::MemoryReader dumpReader = [session](uint64_t at, void* buffer, size_t count) {
            return session->ReadMemory(at, buffer, count, nullptr);
        };
        if (!services::DumpMemoryToFile(dumpReader, dumpPath, windowBase, window.size(), dump, &error)) {
            error = "dump_failed:" + error;
            return false;
        }
        uint64_t dumpSize = 0;
        if (!services::FileByteSize(dumpPath, dumpSize, &error) || dumpSize != window.size()) {
            error = "dump_size_mismatch";
            return false;
        }
        std::vector<uint8_t> reread(window.size());
        {
            std::ifstream file(dumpPath, std::ios::binary);
            file.read(reinterpret_cast<char*>(reread.data()), static_cast<std::streamsize>(reread.size()));
            if (!file) {
                error = "dump_read_back_failed";
                return false;
            }
        }
        std::remove(dumpPath.c_str());
        if (std::memcmp(reread.data() + 0x100, &first32, 4) != 0) {
            error = "dump_contents_mismatch";
            return false;
        }
        summary += " dump=" + std::to_string(dump.read);

        services::CustomType tenths;
        tenths.name = "health x10";
        tenths.size = 4;
        tenths.scale = 0.1;
        double scaled = 0;
        if (!services::ReadCustomType(tenths, reread.data() + 0x100, 4, scaled, &error) ||
            scaled < static_cast<double>(first32) * 0.1 - 0.001 ||
            scaled > static_cast<double>(first32) * 0.1 + 0.001) {
            error = "custom_type_failed:" + error;
            return false;
        }
        summary += " customtype=ok";

        // Dissecting the bytes around the health value, and a pointer spider
        // over the module's data, both through the same address predicate.
        const auto regions = session->MemoryRegions();
        const auto plausible = [regions](uint64_t value) {
            for (const auto& region : regions)
                if (region.readable && value >= region.base && value < region.base + region.size) return true;
            return false;
        };
        const auto describe = [&modules](uint64_t value) -> std::string {
            for (const auto& entry : modules)
                if (value >= entry.base && value < entry.base + entry.size) return entry.name;
            return {};
        };
        services::DissectOptions dissectOptions;
        dissectOptions.size = 0x80;
        services::DissectResult dissected;
        if (!services::DissectStructure(dumpReader, {health, health}, dissectOptions, plausible, describe,
                                        dissected, &error) ||
            dissected.fields.empty() || dissected.instances.size() != 2) {
            error = "dissect_failed:" + error;
            return false;
        }
        // The same address twice must agree on every field.
        for (const auto& field : dissected.fields) {
            if (!field.differs) continue;
            error = "dissect_differs_against_itself";
            return false;
        }
        summary += " dissect=" + std::to_string(dissected.fields.size());

        services::SpiderOptions spiderOptions;
        spiderOptions.maxLevel = 2;
        spiderOptions.maxOffset = 0x100;
        spiderOptions.maxNodes = 200;
        std::vector<services::SpiderNode> spidered;
        if (!services::SpiderPointers(dumpReader, module.base, spiderOptions, plausible, describe, spidered,
                                      &error)) {
            error = "spider_failed:" + error;
            return false;
        }
        for (size_t node = 0; node < spidered.size(); ++node) {
            // Every path names as many offsets as the node has levels.
            if (services::SpiderOffsets(spidered, node).size() ==
                static_cast<size_t>(spidered[node].level) + 1)
                continue;
            error = "spider_path_mismatch";
            return false;
        }
        summary += " spider=" + std::to_string(spidered.size());
    }

    std::vector<uint8_t> code(128);
    if (!session->ReadMemory(codeAddress, code.data(), code.size(), nullptr)) code.resize(32);
    services::Signature signature;
    services::SignatureOptions signatureOptions;
    signatureOptions.x64 = session->Target().architecture != cortex::target::Architecture::X86;
    if (!services::GenerateSignature(code.data(), code.size(), nullptr, 0, signatureOptions, signature, &error)) {
        error = "signature_failed:" + error;
        return false;
    }
    summary += " pe_sections=" + std::to_string(image.sections.size()) + " exports=" +
               std::to_string(image.exports.size()) + " scan_hits=" + std::to_string(scanHits) +
               " signature=" + std::to_string(signature.bytes.size()) + "B";

    // Code injection: a trampoline at CortexStepOverCaller (called every
    // frame) that forces the health value, then a restore. Exercises the
    // assembler, instruction relocation and remote allocation end to end.
    if (stepOver && health) {
        const bool x64 = session->Target().architecture != cortex::target::Architecture::X86;
        const uint64_t pid = session->Target().processId;
        const uint32_t magic = 0x2B2B;

        auto evaluate = [health](const std::string& text, uint64_t& value, std::string&) {
            if (text == "health") { value = health; return true; }
            return false;
        };
        auto assemble = [&](const std::string& text, uint64_t at, std::vector<uint8_t>& out) {
            services::AssembleRequest request;
            request.text = text;
            request.address = at;
            request.x64 = x64;
            request.evaluate = evaluate;
            std::string message;
            return services::AssembleLine(request, out, &message);
        };

        std::vector<uint8_t> original;
        if (app.memory.Read(stepOver, 16, original, &error)) {
            uint64_t cave = 0;
            if (cortex::remote_memory::Allocate(pid, 512, stepOver, cave, &error)) {
                std::vector<uint8_t> siteJump;
                char caveHex[24] = {};
                std::snprintf(caveHex, sizeof(caveHex), "%llX", static_cast<unsigned long long>(cave));
                assemble(std::string("jmp ") + caveHex, stepOver, siteJump);
                int steal = static_cast<int>(siteJump.size());
                std::vector<services::DisassemblyInstruction> decoded;
                if (app.disassembly.Decode(stepOver, 8, decoded, nullptr)) {
                    int length = 0;
                    for (const auto& instruction : decoded) {
                        length += static_cast<int>(instruction.bytes.size());
                        if (length >= static_cast<int>(siteJump.size())) break;
                    }
                    steal = std::max(steal, length);
                }
                std::vector<uint8_t> stolen;
                app.memory.Read(stepOver, static_cast<size_t>(steal), stolen, nullptr);

                std::vector<uint8_t> caveBytes;
                char magicHex[16] = {};
                std::snprintf(magicHex, sizeof(magicHex), "%X", magic);
                assemble(std::string("mov dword [health],") + magicHex, cave, caveBytes);
                std::vector<uint8_t> relocated;
                std::string relocateError;
                if (!services::RelocateCode(stolen.data(), stolen.size(), stepOver, cave + caveBytes.size(), x64,
                                            relocated, &relocateError)) {
                    error = "inject_relocate_failed:" + relocateError;
                    cortex::remote_memory::Free(pid, cave, nullptr);
                    return false;
                }
                caveBytes.insert(caveBytes.end(), relocated.begin(), relocated.end());
                std::vector<uint8_t> jumpBack;
                char backHex[24] = {};
                std::snprintf(backHex, sizeof(backHex), "%llX",
                              static_cast<unsigned long long>(stepOver + steal));
                assemble(std::string("jmp ") + backHex, cave + caveBytes.size(), jumpBack);
                caveBytes.insert(caveBytes.end(), jumpBack.begin(), jumpBack.end());

                std::vector<uint8_t> patch = siteJump;
                patch.resize(static_cast<size_t>(steal), 0x90);

                if (!cortex::remote_memory::WriteCode(pid, cave, caveBytes.data(), caveBytes.size(), &error) ||
                    !cortex::remote_memory::WriteCode(pid, stepOver, patch.data(), patch.size(), &error)) {
                    cortex::remote_memory::Free(pid, cave, nullptr);
                    error = "inject_write_failed:" + error;
                    return false;
                }

                bool forced = false;
                for (int attempt = 0; attempt < 40 && !forced; ++attempt) {
                    Sleep(25);
                    uint32_t value = 0;
                    if (session->ReadMemory(health, &value, 4, nullptr) && value == magic) forced = true;
                }

                // Restore and confirm the target keeps running.
                cortex::remote_memory::WriteCode(pid, stepOver, original.data(), original.size(), nullptr);
                Sleep(60);
                cortex::remote_memory::Free(pid, cave, nullptr);
                Sleep(120);
                if (!session->Alive()) {
                    error = "target_died_after_injection";
                    return false;
                }
                if (!forced) {
                    error = "injection_did_not_take_effect";
                    return false;
                }
                summary += " injection=ok";
            }
        }
    }

    // Auto Assembler scripts: a data patch through an exported symbol, then
    // an allocation with registersymbol and dealloc.
    if (health) {
        const bool x64 = session->Target().architecture != cortex::target::Architecture::X86;
        const auto host = cortex::ui::MakeAutoAssemblerHost(app.ui);
        cortex::services::AutoAssembleOptions options;
        options.x64 = x64;

        uint32_t before = 0;
        session->ReadMemory(health, &before, 4, nullptr);
        char script[512] = {};
        std::snprintf(script, sizeof(script),
                      "[ENABLE]\n"
                      "g_cortex_health:\n"
                      "  db 39 30 00 00\n"
                      "[DISABLE]\n"
                      "g_cortex_health:\n"
                      "  db %02X %02X %02X %02X\n",
                      before & 0xFF, (before >> 8) & 0xFF, (before >> 16) & 0xFF, (before >> 24) & 0xFF);

        cortex::services::AutoAssembleResult enabled;
        options.enable = true;
        if (!cortex::services::RunAutoAssembler(script, host, options, enabled, &error)) {
            error = "auto_assembler_enable_failed:" + error;
            return false;
        }
        uint32_t patched = 0;
        session->ReadMemory(health, &patched, 4, nullptr);
        if (patched != 0x3039) {
            error = "auto_assembler_patch_missing";
            return false;
        }
        cortex::services::AutoAssembleResult disabled;
        options.enable = false;
        if (!cortex::services::RunAutoAssembler(script, host, options, disabled, &error)) {
            error = "auto_assembler_disable_failed:" + error;
            return false;
        }
        uint32_t restored = 0;
        session->ReadMemory(health, &restored, 4, nullptr);
        if (restored != before) {
            error = "auto_assembler_restore_failed";
            return false;
        }

        cortex::services::AutoAssembleResult allocated;
        options.enable = true;
        if (!cortex::services::RunAutoAssembler(
                "[ENABLE]\nalloc(smokecave,$100)\nregistersymbol(smokecave)\nsmokecave:\n  ret\n"
                "[DISABLE]\nunregistersymbol(smokecave)\ndealloc(smokecave)\n",
                host, options, allocated, &error)) {
            error = "auto_assembler_alloc_failed:" + error;
            return false;
        }
        if (allocated.allocations.size() != 1 || allocated.registered.size() != 1) {
            error = "auto_assembler_alloc_unexpected";
            return false;
        }
        const uint64_t cave = allocated.allocations[0].second;
        uint8_t caveByte = 0;
        if (!session->ReadMemory(cave, &caveByte, 1, nullptr) || caveByte != 0xC3) {
            error = "auto_assembler_cave_not_written";
            return false;
        }
        // The symbol the script registered must resolve for [DISABLE].
        app.ui.userSymbols->Set("smokecave", cave);
        cortex::services::AutoAssembleResult freed;
        options.enable = false;
        cortex::services::RunAutoAssembler(
            "[ENABLE]\nalloc(smokecave,$100)\n[DISABLE]\nunregistersymbol(smokecave)\ndealloc(smokecave)\n",
            host, options, freed, &error);
        app.ui.userSymbols->Remove("smokecave");
        if (freed.freed.size() != 1) {
            error = "auto_assembler_dealloc_missing";
            return false;
        }
        summary += " autoassembler=ok";
    }

    // Speedhack: the target samples GetTickCount() every frame, so scaling
    // the clock shows up as a faster tick rate.
    if (ticks) {
        const bool x64 = session->Target().architecture != cortex::target::Architecture::X86;
        const uint64_t pid = session->Target().processId;
        cortex::services::SpeedhackHost speedHost;
        speedHost.read = [session](uint64_t address, void* buffer, size_t size) {
            return session->ReadMemory(address, buffer, size, nullptr);
        };
        speedHost.write = [pid](uint64_t address, const void* buffer, size_t size) {
            return cortex::remote_memory::WriteCode(pid, address, buffer, size, nullptr);
        };
        speedHost.allocate = [pid](size_t size, uint64_t nearAddress, uint64_t& address, std::string& message) {
            return cortex::remote_memory::Allocate(pid, size, nearAddress, address, &message);
        };
        speedHost.release = [pid](uint64_t address, std::string& message) {
            return cortex::remote_memory::Free(pid, address, &message);
        };
        speedHost.symbol = [&app](const std::string& name, uint64_t& value) {
            return cortex::ui::ContextSymbols(app.ui).Resolve(name, value);
        };

        auto tickRate = [&](int milliseconds) {
            uint32_t first = 0;
            uint32_t last = 0;
            session->ReadMemory(ticks, &first, 4, nullptr);
            Sleep(static_cast<DWORD>(milliseconds));
            session->ReadMemory(ticks, &last, 4, nullptr);
            return static_cast<double>(last - first) / milliseconds;
        };

        const double normal = tickRate(400);
        cortex::services::SpeedhackState speed;
        std::string speedError;
        if (!cortex::services::InstallSpeedhack(speedHost, x64, 8.0, speed, &speedError)) {
            error = "speedhack_install_failed:" + speedError;
            return false;
        }
        const double fast = tickRate(400);
        cortex::services::RemoveSpeedhack(speedHost, speed, nullptr);
        Sleep(200);
        const double restored = tickRate(400);
        if (!session->Alive()) {
            error = "target_died_after_speedhack";
            return false;
        }
        // The scaled clock must run clearly faster, and go back to normal.
        if (!(fast > normal * 3.0) || !(restored < fast / 2.0)) {
            char detail[128] = {};
            std::snprintf(detail, sizeof(detail), "speedhack_rate_unexpected:%.2f/%.2f/%.2f", normal, fast, restored);
            error = detail;
            return false;
        }
        summary += " speedhack=ok";
    }

    app.ui.toolsTabRequest = "pointers";
    app.ui.NavigateTo("tools", health ? health : codeAddress);
    return RenderGuiStable(app, error, true);
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
    std::string toolsSummary;
    if (!ExerciseMemoryTools(app, address, toolsSummary, error)) {
        std::fprintf(stderr, "gui attached suite: memory tools failed: %s\n", error.c_str());
        return 12;
    }
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
        "PASS: GUI attached suite pid=%lu address=0x%llX bytes=%zu instructions=%zu%s\n",
        static_cast<unsigned long>(pid),
        static_cast<unsigned long long>(address),
        bytes.size(), decoded.size(), toolsSummary.c_str());
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


}  // namespace cortex::desktop
