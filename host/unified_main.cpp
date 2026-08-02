#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

// These entry points are the existing tool mains, renamed per-source by CMake.
// Keeping each implementation in its own translation unit avoids anonymous
// namespace collisions and preserves the already-tested behavior.
int CortexServeMain(int argc, char** argv);
int CortexInjectMain(int argc, char** argv);
int CortexMcpMain(int argc, char** argv);
int CortexDiagnoseMain(int argc, char** argv);
int CortexSymbolizeMain(int argc, char** argv);

namespace {

using EntryPoint = int (*)(int, char**);

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return value;
}

void PrintUsage(FILE* stream = stdout) {
    std::fputs(
        "Cortex unified host\n\n"
        "Usage:\n"
        "  cortex_host serve --pid <pid> | --process <game.exe> [server options]\n"
        "  cortex_host inject <process-name-or-pid> [cortex_core.dll]\n"
        "  cortex_host diagnose --pid <pid> [diagnostic options]\n"
        "  cortex_host analyze <crash-directory>\n"
        "  cortex_host symbolize --image <dll-or-exe> --rva <hex> [symbol options]\n"
        "  cortex_host mcp [--host 127.0.0.1] [--port 6969] [--token-file file]\n\n"
        "Compatibility:\n"
        "  cortex_host --pid <pid> ... still starts the external HTTP server.\n\n"
        "Commands:\n"
        "  serve       External controller, memory scanner and HTTP API\n"
        "  inject      Inject cortex_core.dll into a matching process\n"
        "  diagnose    Watch crashes, freezes and write external dumps\n"
        "  analyze     Analyze an existing crash/freeze directory\n"
        "  symbolize   Resolve a PE module RVA through PDB or DWARF tools\n"
        "  mcp         Run the stdio-to-HTTP MCP bridge\n",
        stream);
}

int Forward(EntryPoint entry, const char* programName,
            int argc, char** argv, int firstArgument,
            const std::vector<std::string>& injected = {}) {
    std::vector<std::string> storage;
    storage.reserve(1 + injected.size() +
                    static_cast<size_t>((std::max)(0, argc - firstArgument)));
    storage.emplace_back(programName ? programName : "cortex_host");
    storage.insert(storage.end(), injected.begin(), injected.end());
    for (int index = firstArgument; index < argc; ++index)
        storage.emplace_back(argv[index] ? argv[index] : "");

    std::vector<char*> forwarded;
    forwarded.reserve(storage.size() + 1);
    for (std::string& value : storage) forwarded.push_back(value.data());
    forwarded.push_back(nullptr);
    return entry(static_cast<int>(storage.size()), forwarded.data());
}

bool LooksLikeLegacyServeInvocation(const char* argument) {
    if (!argument || !*argument) return false;
    if (argument[0] == '-') return true;
    const std::string value = Lower(argument);
    return value.find(".exe") != std::string::npos;
}

} // namespace

int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
                 SEM_NOOPENFILEERRORBOX);

    if (argc <= 1) {
        PrintUsage();
        return 0;
    }

    const std::string command = Lower(argv[1] ? argv[1] : "");
    if (command == "--help" || command == "-h" || command == "help") {
        PrintUsage();
        return 0;
    }
    if (command == "--version" || command == "version") {
        std::puts("cortex_host unified diagnostics tool");
        return 0;
    }

    if (command == "serve" || command == "server" || command == "scan")
        return Forward(CortexServeMain, "cortex_host serve", argc, argv, 2);
    if (command == "inject" || command == "injector")
        return Forward(CortexInjectMain, "cortex_host inject", argc, argv, 2);
    if (command == "diagnose" || command == "diagnostics" || command == "watch")
        return Forward(CortexDiagnoseMain, "cortex_host diagnose", argc, argv, 2);
    if (command == "analyze" || command == "analyse") {
        if (argc < 3) {
            std::fputs("cortex_host analyze: missing crash directory\n", stderr);
            return 2;
        }
        return Forward(CortexDiagnoseMain, "cortex_host analyze", argc, argv, 2,
                       {"--analyze"});
    }
    if (command == "symbolize" || command == "symbolise" || command == "symbols")
        return Forward(CortexSymbolizeMain, "cortex_host symbolize", argc, argv, 2);
    if (command == "mcp" || command == "mcp-bridge")
        return Forward(CortexMcpMain, "cortex_host mcp", argc, argv, 2);

    // Preserve the original cortex_host command line so existing scripts that
    // pass --pid/--process directly do not break during the consolidation.
    if (LooksLikeLegacyServeInvocation(argv[1]))
        return CortexServeMain(argc, argv);

    std::fprintf(stderr, "Unknown Cortex command: %s\n\n", argv[1]);
    PrintUsage(stderr);
    return 2;
}
