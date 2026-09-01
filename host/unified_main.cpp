#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "../mcp_bridge/policy.h"
#include "diagnostics/external_host.h"
#include "probe_cli.h"

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
using json = nlohmann::json;

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
        "  cortex_host probe --pid <pid> [--heartbeat render]\n"
        "  cortex_host diagnose --pid <pid> [diagnostic options]\n"
        "  cortex_host analyze <crash-directory>\n"
        "  cortex_host symbolize --image <dll-or-exe> --rva <hex> [symbol options]\n"
        "  cortex_host mcp [--process <name>|--pid <pid>] [--transport native|http] [--tools compact|all] [--token-file file]\n\n"
        "Compatibility:\n"
        "  cortex_host --pid <pid> ... still starts the external HTTP server.\n\n"
        "Commands:\n"
        "  serve       External controller, memory scanner and HTTP API\n"
        "  inject      Inject cortex_core.dll into a matching process\n"
        "  probe       Read-only process/shared-channel health report as JSON\n"
        "  diagnose    Watch crashes, freezes and write external dumps\n"
        "  analyze     Analyze an existing crash/freeze directory\n"
        "  symbolize   Resolve a PE module RVA through PDB or DWARF tools\n"
        "  mcp         Run the local stdio MCP bridge (native pipe by default)\n",
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

bool ValidateMcpArguments(int argc, char** argv, int firstArgument) {
    std::string host = "127.0.0.1";
    int port = 6969;
    for (int index = firstArgument; index < argc; ++index) {
        const std::string arg = argv[index] ? argv[index] : "";
        if (arg == "--host") {
            if (index + 1 >= argc) return false;
            host = argv[++index] ? argv[index] : "";
        } else if (arg == "--port") {
            if (index + 1 >= argc) return false;
            const std::string raw = argv[++index] ? argv[index] : "";
            try {
                size_t consumed = 0;
                const int parsed = std::stoi(raw, &consumed, 10);
                if (consumed != raw.size()) return false;
                port = parsed;
            } catch (...) {
                return false;
            }
        }
    }
    return mcp_bridge::policy::IsLoopbackHost(host) && mcp_bridge::policy::IsValidPort(port);
}

struct ProbeOptions {
    DWORD processId = 0;
    std::string heartbeatSource = "render";
};

bool ParseProbeArguments(int argc, char** argv, int firstArgument, ProbeOptions& options) {
    for (int index = firstArgument; index < argc; ++index) {
        const std::string arg = argv[index] ? argv[index] : "";
        if (arg == "--pid") {
            if (index + 1 >= argc) return false;
            const std::string raw = argv[++index] ? argv[index] : "";
            try {
                size_t consumed = 0;
                const unsigned long parsed = std::stoul(raw, &consumed, 10);
                if (consumed != raw.size() || parsed == 0) return false;
                options.processId = static_cast<DWORD>(parsed);
            } catch (...) {
                return false;
            }
        } else if (arg == "--heartbeat") {
            if (index + 1 >= argc) return false;
            options.heartbeatSource = argv[++index] ? argv[index] : "";
            if (options.heartbeatSource.empty()) return false;
        } else {
            return false;
        }
    }
    return options.processId != 0;
}

int RunProbe(const ProbeOptions& options) {
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                 FALSE, options.processId);
    if (!process) {
        const DWORD error = GetLastError();
        const json output{{"schema_version", 1},
                          {"ok", false},
                          {"process_id", options.processId},
                          {"error", "open_process_failed"},
                          {"win32_error", error}};
        std::puts(output.dump(2).c_str());
        return 3;
    }

    const bool processAlive = hostdiag::IsProcessAlive(process);
    bool windowFound = false;
    const bool windowResponsive = processAlive &&
        hostdiag::IsWindowResponsive(options.processId, windowFound);

    hostdiag::SharedClient shared;
    std::string openError;
    const bool sharedOpen = shared.Open(options.processId, openError);
    hostdiag::SharedSnapshot snapshot;
    std::string snapshotError;
    const bool haveSnapshot = sharedOpen && shared.Snapshot(snapshot, snapshotError);

    const uint64_t now = GetTickCount64();
    bool heartbeatFound = false;
    uint64_t heartbeatAge = 0;
    if (haveSnapshot) {
        heartbeatAge = hostdiag::HeartbeatAgeMs(snapshot, options.heartbeatSource,
                                                now, heartbeatFound);
    }

    json allHeartbeats = json::array();
    if (haveSnapshot) {
        for (const auto& heartbeat : snapshot.heartbeats) {
            json item{{"source", heartbeat.source},
                      {"thread_id", heartbeat.threadId},
                      {"sequence", heartbeat.sequence},
                      {"last_tick_ms", heartbeat.lastTickMs}};
            if (heartbeat.lastTickMs && now >= heartbeat.lastTickMs)
                item["age_ms"] = now - heartbeat.lastTickMs;
            else
                item["age_ms"] = nullptr;
            allHeartbeats.push_back(std::move(item));
        }
    }

    json sharedState{{"available", haveSnapshot},
                     {"ready", haveSnapshot && snapshot.ready},
                     {"same_bitness", haveSnapshot && snapshot.sameBitness},
                     {"pointer_size", haveSnapshot ? snapshot.pointerSize : 0},
                     {"started_tick_ms", haveSnapshot ? snapshot.startedTickMs : 0},
                     {"last_core_heartbeat_ms", haveSnapshot ? snapshot.lastCoreHeartbeatMs : 0},
                     {"heartbeats", std::move(allHeartbeats)}};
    if (haveSnapshot && snapshot.lastCoreHeartbeatMs && now >= snapshot.lastCoreHeartbeatMs)
        sharedState["core_heartbeat_age_ms"] = now - snapshot.lastCoreHeartbeatMs;
    else
        sharedState["core_heartbeat_age_ms"] = nullptr;

    json requestedHeartbeat{{"source", options.heartbeatSource}, {"found", heartbeatFound}};
    requestedHeartbeat["age_ms"] = heartbeatFound ? json(heartbeatAge) : json(nullptr);

    json errors = json::object();
    if (!openError.empty()) errors["shared_open"] = openError;
    if (!snapshotError.empty()) errors["shared_snapshot"] = snapshotError;

    const json output{{"schema_version", 1},
                      {"ok", processAlive},
                      {"process_id", options.processId},
                      {"process_alive", processAlive},
                      {"host_pointer_size", sizeof(void*)},
                      {"window", {{"found", windowFound}, {"responsive", windowResponsive}}},
                      {"shared", std::move(sharedState)},
                      {"heartbeat", std::move(requestedHeartbeat)},
                      {"errors", std::move(errors)}};
    std::puts(output.dump(2).c_str());
    CloseHandle(process);
    return processAlive ? 0 : 5;
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
    if (command == "probe")
        return Forward(CortexProbeMain, "cortex_host probe", argc, argv, 2);
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
    if (command == "mcp" || command == "mcp-bridge") {
        if (!ValidateMcpArguments(argc, argv, 2)) {
            std::fputs("cortex_host mcp: --host must be loopback and --port must be 1..65535\n", stderr);
            return 2;
        }
        return Forward(CortexMcpMain, "cortex_host mcp", argc, argv, 2);
    }

    // Preserve the original cortex_host command line so existing scripts that
    // pass --pid/--process directly do not break during the consolidation.
    if (LooksLikeLegacyServeInvocation(argv[1]))
        return CortexServeMain(argc, argv);

    std::fprintf(stderr, "Unknown Cortex command: %s\n\n", argv[1]);
    PrintUsage(stderr);
    return 2;
}
