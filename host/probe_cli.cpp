#include "probe_cli.h"
#include "diagnostics/external_host.h"

#include <windows.h>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <string>

namespace {
using json = nlohmann::json;

struct Options { DWORD pid = 0; std::string heartbeat = "render"; };

void Usage(FILE* out) {
    std::fputs("Usage: cortex probe --pid <pid> [--heartbeat source]\n", out);
}

bool Parse(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i] ? argv[i] : "";
        if (arg == "--pid" && i + 1 < argc) {
            try {
                size_t used = 0;
                const unsigned long value = std::stoul(argv[++i], &used, 10);
                if (!value || used != std::string(argv[i]).size()) return false;
                options.pid = static_cast<DWORD>(value);
            } catch (...) { return false; }
        } else if (arg == "--heartbeat" && i + 1 < argc) {
            options.heartbeat = argv[++i] ? argv[i] : "";
            if (options.heartbeat.empty()) return false;
        } else if (arg == "--help" || arg == "-h") {
            Usage(stdout);
            return false;
        } else return false;
    }
    return options.pid != 0;
}

int Run(const Options& options) {
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, options.pid);
    if (!process) {
        std::puts(json{{"schema_version",1},{"ok",false},{"process_id",options.pid},
                       {"error","open_process_failed"},{"win32_error",GetLastError()}}.dump(2).c_str());
        return 3;
    }

    const bool alive = hostdiag::IsProcessAlive(process);
    bool windowFound = false;
    const bool responsive = alive && hostdiag::IsWindowResponsive(options.pid, windowFound);
    hostdiag::SharedClient shared;
    std::string openError;
    const bool opened = shared.Open(options.pid, openError);
    hostdiag::SharedSnapshot snapshot;
    std::string snapshotError;
    const bool haveSnapshot = opened && shared.Snapshot(snapshot, snapshotError);
    const uint64_t now = GetTickCount64();
    bool heartbeatFound = false;
    const uint64_t age = haveSnapshot
        ? hostdiag::HeartbeatAgeMs(snapshot, options.heartbeat, now, heartbeatFound) : 0;

    json heartbeats = json::array();
    if (haveSnapshot) {
        for (const auto& hb : snapshot.heartbeats) {
            json item{{"source",hb.source},{"thread_id",hb.threadId},{"sequence",hb.sequence},
                      {"last_tick_ms",hb.lastTickMs}};
            item["age_ms"] = (hb.lastTickMs && now >= hb.lastTickMs) ? json(now - hb.lastTickMs) : json(nullptr);
            heartbeats.push_back(std::move(item));
        }
    }
    json sharedState{{"available",haveSnapshot},{"ready",haveSnapshot && snapshot.ready},
                     {"same_bitness",haveSnapshot && snapshot.sameBitness},
                     {"pointer_size",haveSnapshot ? snapshot.pointerSize : 0},
                     {"started_tick_ms",haveSnapshot ? snapshot.startedTickMs : 0},
                     {"last_core_heartbeat_ms",haveSnapshot ? snapshot.lastCoreHeartbeatMs : 0},
                     {"heartbeats",std::move(heartbeats)}};
    sharedState["core_heartbeat_age_ms"] =
        (haveSnapshot && snapshot.lastCoreHeartbeatMs && now >= snapshot.lastCoreHeartbeatMs)
        ? json(now - snapshot.lastCoreHeartbeatMs) : json(nullptr);

    json requested{{"source",options.heartbeat},{"found",heartbeatFound}};
    requested["age_ms"] = heartbeatFound ? json(age) : json(nullptr);
    json errors = json::object();
    if (!openError.empty()) errors["shared_open"] = openError;
    if (!snapshotError.empty()) errors["shared_snapshot"] = snapshotError;

    std::puts(json{{"schema_version",1},{"ok",alive},{"process_id",options.pid},{"process_alive",alive},
                   {"host_pointer_size",sizeof(void*)},{"window",{{"found",windowFound},{"responsive",responsive}}},
                   {"shared",std::move(sharedState)},{"heartbeat",std::move(requested)},
                   {"errors",std::move(errors)}}.dump(2).c_str());
    CloseHandle(process);
    return alive ? 0 : 5;
}
} // namespace

int CortexProbeMain(int argc, char** argv) {
    if (argc == 2 && argv[1] && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
        Usage(stdout);
        return 0;
    }
    Options options;
    if (!Parse(argc, argv, options)) {
        Usage(stderr);
        return 2;
    }
    return Run(options);
}
