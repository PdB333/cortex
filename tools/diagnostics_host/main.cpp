#include "../../host/diagnostics/external_host.h"
#include "../../host/diagnostics/analyzer.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

struct Options {
    DWORD processId = 0;
    std::string outputDirectory = "cortex_crashes";
    std::string heartbeatSource = "render";
    DWORD hangTimeoutMs = 5000;
    DWORD pollMs = 250;
    bool fullDump = false;
    bool once = false;
    std::string analyzeDirectory;
};

void PrintUsage() {
    std::cout
        << "Cortex external diagnostics host\n\n"
        << "Watch a process:\n"
        << "  cortex_diag_host --pid <pid> [--output <dir>] [--heartbeat <source>]\n"
        << "                   [--hang-ms 5000] [--poll-ms 250] [--full-dump] [--once]\n\n"
        << "Analyze an existing report:\n"
        << "  cortex_diag_host --analyze <crash-directory>\n";
}

bool Parse(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--help" || argument == "-h") return false;
        if (argument == "--pid" && i + 1 < argc) options.processId = std::stoul(argv[++i]);
        else if (argument == "--output" && i + 1 < argc) options.outputDirectory = argv[++i];
        else if (argument == "--heartbeat" && i + 1 < argc) options.heartbeatSource = argv[++i];
        else if (argument == "--hang-ms" && i + 1 < argc) options.hangTimeoutMs = std::stoul(argv[++i]);
        else if (argument == "--poll-ms" && i + 1 < argc) options.pollMs = std::stoul(argv[++i]);
        else if (argument == "--full-dump") options.fullDump = true;
        else if (argument == "--once") options.once = true;
        else if (argument == "--analyze" && i + 1 < argc) options.analyzeDirectory = argv[++i];
        else {
            std::cerr << "Unknown or incomplete argument: " << argument << '\n';
            return false;
        }
    }
    return !options.analyzeDirectory.empty() || options.processId != 0;
}

std::string Join(const std::string& directory, const char* name) {
    if (directory.empty()) return name ? name : "";
    if (directory.back() == '\\' || directory.back() == '/') return directory + (name ? name : "");
    return directory + "\\" + (name ? name : "");
}

void WriteExternalReport(const std::string& directory, const char* kind,
                         DWORD processId, const hostdiag::DumpResult& dump,
                         const hostdiag::SharedSnapshot* shared,
                         uint64_t heartbeatAge, bool heartbeatFound,
                         bool windowFound, bool windowResponsive,
                         const std::string& threadError) {
    FILE* file = nullptr;
    const std::string path = Join(directory, kind && std::string(kind) == "hang"
        ? "hang_report.json" : "external_report.json");
    fopen_s(&file, path.c_str(), "wb");
    if (!file) return;
    std::fprintf(file,
                 "{\n  \"schema_version\":1,\n  \"kind\":\"%s\",\n  \"process_id\":%lu,\n"
                 "  \"host_pointer_size\":%llu,\n  \"shared_available\":%s,\n"
                 "  \"target_pointer_size\":%u,\n  \"same_bitness\":%s,\n"
                 "  \"dump\":{\"success\":%s,\"error\":%lu,\"path\":\"%s\"},\n"
                 "  \"heartbeat\":{\"found\":%s,\"age_ms\":%llu},\n"
                 "  \"window\":{\"found\":%s,\"responsive\":%s},\n"
                 "  \"thread_capture_error\":\"%s\"\n}\n",
                 kind ? kind : "crash", static_cast<unsigned long>(processId),
                 static_cast<unsigned long long>(sizeof(void*)),
                 shared && shared->available ? "true" : "false",
                 shared ? shared->pointerSize : 0,
                 shared && shared->sameBitness ? "true" : "false",
                 dump.success ? "true" : "false", static_cast<unsigned long>(dump.error),
                 dump.path.c_str(), heartbeatFound ? "true" : "false",
                 static_cast<unsigned long long>(heartbeatAge),
                 windowFound ? "true" : "false", windowResponsive ? "true" : "false",
                 threadError.c_str());
    std::fclose(file);
}

void AnalyzeAndWrite(const std::string& directory) {
    const auto findings = hostdiag::AnalyzeCrashDirectory(directory);
    std::string error;
    if (!hostdiag::WriteAnalysisReport(directory, findings, error))
        std::cerr << "analysis failed: " << error << '\n';
}

std::string ResolveCrashDirectory(const Options& options) {
    std::string directory;
    for (int i = 0; i < 40; ++i) {
        if (hostdiag::FindNewestCrashDirectory(options.outputDirectory, options.processId, directory))
            return directory;
        Sleep(50);
    }
    return hostdiag::MakeCaptureDirectory(options.outputDirectory, "external_crash", options.processId);
}

int AnalyzeOnly(const Options& options) {
    const auto findings = hostdiag::AnalyzeCrashDirectory(options.analyzeDirectory);
    std::string error;
    if (!hostdiag::WriteAnalysisReport(options.analyzeDirectory, findings, error)) {
        std::cerr << error << '\n';
        return 4;
    }
    for (const auto& finding : findings)
        std::cout << finding.confidence << ": " << finding.title << '\n';
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!Parse(argc, argv, options)) {
        PrintUsage();
        return argc > 1 ? 2 : 0;
    }
    if (!options.analyzeDirectory.empty()) return AnalyzeOnly(options);

    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                 FALSE, options.processId);
    if (!process) {
        std::cerr << "OpenProcess failed: " << GetLastError() << '\n';
        return 3;
    }

    hostdiag::SharedClient shared;
    std::string sharedError;
    shared.Open(options.processId, sharedError);
    if (!shared.IsOpen())
        std::cerr << "shared channel unavailable for now: " << sharedError << '\n';

    LONG lastCrashSequence = 0;
    uint64_t lastHeartbeatSequence = 0;
    uint64_t unresponsiveSince = 0;
    bool hangCaptured = false;

    while (hostdiag::IsProcessAlive(process)) {
        if (!shared.IsOpen()) {
            shared.Open(options.processId, sharedError);
            Sleep(options.pollMs);
        } else {
            shared.Wait(options.pollMs);
        }

        hostdiag::SharedSnapshot snapshot;
        std::string snapshotError;
        const bool haveShared = shared.IsOpen() && shared.Snapshot(snapshot, snapshotError);
        if (haveShared && snapshot.crash.sequence != 0 &&
            snapshot.crash.sequence != lastCrashSequence) {
            lastCrashSequence = snapshot.crash.sequence;
            const std::string directory = ResolveCrashDirectory(options);
            const std::string dumpPath = Join(directory, "external_crash.dmp");
            const hostdiag::DumpResult dump = hostdiag::WriteProcessDump(
                options.processId, dumpPath,
                snapshot.sameBitness ? &snapshot.crash : nullptr, options.fullDump);
            bool windowFound = false;
            const bool responsive = hostdiag::IsWindowResponsive(options.processId, windowFound);
            bool heartbeatFound = false;
            const uint64_t age = hostdiag::HeartbeatAgeMs(snapshot, options.heartbeatSource,
                                                          GetTickCount64(), heartbeatFound);
            WriteExternalReport(directory, "crash", options.processId, dump, &snapshot,
                                age, heartbeatFound, windowFound, responsive, "");
            Sleep(300);
            AnalyzeAndWrite(directory);
            std::cout << "external crash capture: " << directory << '\n';
            if (options.once) break;
        }

        bool windowFound = false;
        const bool responsive = hostdiag::IsWindowResponsive(options.processId, windowFound);
        const uint64_t now = GetTickCount64();
        if (windowFound && !responsive) {
            if (!unresponsiveSince) unresponsiveSince = now;
        } else {
            unresponsiveSince = 0;
        }

        bool heartbeatFound = false;
        uint64_t heartbeatAge = 0;
        uint64_t currentHeartbeatSequence = 0;
        if (haveShared) {
            heartbeatAge = hostdiag::HeartbeatAgeMs(snapshot, options.heartbeatSource,
                                                    now, heartbeatFound);
            for (const auto& heartbeat : snapshot.heartbeats) {
                if (_stricmp(heartbeat.source.c_str(), options.heartbeatSource.c_str()) == 0) {
                    currentHeartbeatSequence = heartbeat.sequence;
                    break;
                }
            }
        }
        if (currentHeartbeatSequence != lastHeartbeatSequence) {
            lastHeartbeatSequence = currentHeartbeatSequence;
            hangCaptured = false;
        }

        const bool heartbeatStale = heartbeatFound && heartbeatAge >= options.hangTimeoutMs;
        const bool windowStale = unresponsiveSince && now - unresponsiveSince >= options.hangTimeoutMs;
        const bool hangConfirmed = windowStale && (!heartbeatFound || heartbeatStale);
        if (hangConfirmed && !hangCaptured) {
            hangCaptured = true;
            const std::string directory = hostdiag::MakeCaptureDirectory(
                options.outputDirectory, "hang", options.processId);
            const hostdiag::DumpResult dump = hostdiag::WriteProcessDump(
                options.processId, Join(directory, "hang.dmp"), nullptr, options.fullDump);
            std::string threadError;
            hostdiag::CaptureThreads(options.processId, Join(directory, "threads.json"),
                                     nullptr, threadError);
            WriteExternalReport(directory, "hang", options.processId, dump,
                                haveShared ? &snapshot : nullptr, heartbeatAge, heartbeatFound,
                                windowFound, responsive, threadError);
            AnalyzeAndWrite(directory);
            std::cout << "hang capture: " << directory << '\n';
            if (options.once) break;
        }

        if (options.once && !haveShared) break;
    }

    CloseHandle(process);
    return 0;
}
