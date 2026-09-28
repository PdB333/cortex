// Command-line modes of cortex.exe (help, version, bundled tools and GUI
// test modes).

#include "desktop_app.h"
#include "../host/cli_entry_points.h"


namespace cortex::desktop {

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

static int ForwardCli(CliEntryPoint entry, const char* programName,
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

static void PrintCliUsage(FILE* out = stdout) {
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
        "  cortex inject <target> [dll]   Load the x86/x64 runtime matching the target\n"
        "  cortex probe --pid <pid>       Inspect target/runtime health\n"
        "  cortex diagnose --pid <pid>    Watch crash/hang diagnostics\n"
        "  cortex analyze <directory>     Analyze a crash directory\n"
        "  cortex symbolize [options]     Resolve PE symbols offline\n",
        out);
}

static std::optional<int> RunCliMode(std::vector<std::string>& args) {
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
#if CORTEX_WITH_TEST_MODES
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
#else
    if (command == "--smoke-test" || command.rfind("--gui-", 0) == 0 ||
        (command.size() > 6 && command.compare(command.size() - 6, 6, "-smoke") == 0)) {
        std::fprintf(stderr, "cortex: %s is a test mode that is not included in this build\n",
                     command.c_str());
        return 2;
    }
#endif
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

}  // namespace cortex::desktop
