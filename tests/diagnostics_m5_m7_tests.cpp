#define CORTEX_DIAGNOSTICS_TESTING 1

#include "../core/diagnostics/shared_channel.h"
#include "../host/diagnostics/external_host.h"
#include "../host/diagnostics/analyzer.h"

#include <windows.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {
int g_failures = 0;

void Check(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++g_failures;
}

void WriteText(const std::string& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
}

std::string ReadText(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

bool HasFinding(const std::vector<hostdiag::Finding>& findings, const char* id) {
    for (const auto& finding : findings) if (finding.id == id) return true;
    return false;
}

void TestSharedChannel() {
    diagnostics::testing::ResetSharedChannel();
    Check(diagnostics::SharedChannelInit(), "shared channel should initialize");
    Check(diagnostics::IsSharedChannelReady(), "shared channel should report ready");

    diagnostics::SharedHeartbeat("render");
    hostdiag::SharedClient client;
    std::string error;
    Check(client.Open(GetCurrentProcessId(), error), "external client should open current process channel");

    hostdiag::SharedSnapshot snapshot;
    Check(client.Snapshot(snapshot, error), "external client should read shared state");
    Check(snapshot.available && snapshot.ready, "shared snapshot should be available and ready");
    Check(snapshot.sameBitness, "test client and shared channel should have the same bitness");
    bool renderFound = false;
    for (const auto& heartbeat : snapshot.heartbeats) {
        if (heartbeat.source == "render") {
            renderFound = true;
            Check(heartbeat.sequence >= 1, "render heartbeat should have a sequence");
        }
    }
    Check(renderFound, "render heartbeat should be visible to external client");

    EXCEPTION_RECORD record{};
    record.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    record.ExceptionAddress = reinterpret_cast<PVOID>(&TestSharedChannel);
    record.NumberParameters = 2;
    record.ExceptionInformation[0] = 0;
    record.ExceptionInformation[1] = 8;
    CONTEXT context{};
    RtlCaptureContext(&context);
    EXCEPTION_POINTERS pointers{&record, &context};
    diagnostics::SharedPublishCrash(&pointers);
    Check(client.Wait(1000), "external crash event should be signaled");
    Check(client.Snapshot(snapshot, error), "crash snapshot should be readable");
    Check(snapshot.crash.sequence >= 1, "crash sequence should advance");
    Check(snapshot.crash.exception_code == EXCEPTION_ACCESS_VIOLATION,
          "exception code should cross the shared channel");
    Check(snapshot.crash.accessed_address == 8,
          "accessed address should cross the shared channel");
    Check(snapshot.crash.access_type == CORTEX_DIAG_ACCESS_READ,
          "access type should cross the shared channel");

    bool heartbeatFound = false;
    const uint64_t age = hostdiag::HeartbeatAgeMs(snapshot, "render", GetTickCount64(), heartbeatFound);
    Check(heartbeatFound && age < 2000, "heartbeat age should be recent");

    client.Close();
    diagnostics::SharedChannelShutdown();
}

void TestExternalCapture() {
    char temp[MAX_PATH]{};
    GetTempPathA(MAX_PATH, temp);
    char directory[MAX_PATH]{};
    std::snprintf(directory, sizeof(directory), "%scortex_diag_m5_%lu",
                  temp, static_cast<unsigned long>(GetCurrentProcessId()));
    CreateDirectoryA(directory, nullptr);
    const std::string root = directory;
    const std::string dumpPath = root + "\\self.dmp";
    const std::string threadsPath = root + "\\threads.json";

    const hostdiag::DumpResult dump = hostdiag::WriteProcessDump(
        GetCurrentProcessId(), dumpPath, nullptr, false);
    Check(dump.success, "external dump writer should dump the current test process");
    WIN32_FILE_ATTRIBUTE_DATA dumpData{};
    Check(GetFileAttributesExA(dumpPath.c_str(), GetFileExInfoStandard, &dumpData) != FALSE &&
          (dumpData.nFileSizeHigh != 0 || dumpData.nFileSizeLow != 0),
          "external dump should be non-empty");

    std::vector<hostdiag::ThreadSnapshot> threads;
    std::string error;
    Check(hostdiag::CaptureThreads(GetCurrentProcessId(), threadsPath, &threads, error),
          "thread snapshot should be written");
    Check(!threads.empty(), "thread snapshot should contain at least one thread");
    bool currentThreadRecorded = false;
    for (const auto& thread : threads) {
        if (thread.threadId == GetCurrentThreadId()) {
            currentThreadRecorded = true;
            Check(thread.error == ERROR_BUSY,
                  "the capture worker must never suspend its own current thread");
        }
    }
    Check(currentThreadRecorded, "current thread should be listed as intentionally skipped");
    const std::string threadJson = ReadText(threadsPath);
    Check(threadJson.find("\"threads\"") != std::string::npos,
          "threads.json should have the expected schema");

    DeleteFileA(dumpPath.c_str());
    DeleteFileA(threadsPath.c_str());
    RemoveDirectoryA(root.c_str());
}

void TestAnalyzer() {
    char temp[MAX_PATH]{};
    GetTempPathA(MAX_PATH, temp);
    char directory[MAX_PATH]{};
    std::snprintf(directory, sizeof(directory), "%scortex_diag_m7_%lu",
                  temp, static_cast<unsigned long>(GetCurrentProcessId()));
    CreateDirectoryA(directory, nullptr);
    const std::string root = directory;

    WriteText(root + "\\report.json",
              "{\"exception_name\":\"EXCEPTION_ACCESS_VIOLATION\","
              "\"exception_code\":\"0xC0000005\",\"accessed_address\":\"0x8\"}");
    WriteText(root + "\\hooks.json",
              "{\"hooks\":[{\"status\":\"overlap_conflict\",\"max_recursion_depth\":5}]}");
    WriteText(root + "\\values.json",
              "{\"values\":[{\"name\":\"player\",\"value\":\"nullptr\"}]}");
    WriteText(root + "\\build_info.json",
              "{\"modules\":[{\"exact_match\":false,\"verification\":\"pdb_guid_mismatch\"}]}");
    WriteText(root + "\\hang_report.json", "{\"kind\":\"hang\"}");

    const auto findings = hostdiag::AnalyzeCrashDirectory(root);
    Check(HasFinding(findings, "null_dereference"), "analyzer should detect near-null access");
    Check(HasFinding(findings, "overlapping_hooks"), "analyzer should detect overlapping hooks");
    Check(HasFinding(findings, "recursive_hook"), "analyzer should detect recursive hooks");
    Check(HasFinding(findings, "recorded_null_value"), "analyzer should report recorded null values");
    Check(HasFinding(findings, "symbol_mismatch"), "analyzer should detect symbol mismatch");
    Check(HasFinding(findings, "hang_snapshot"), "analyzer should recognize hang artifacts");

    std::string error;
    Check(hostdiag::WriteAnalysisReport(root, findings, error),
          "analysis reports should be written");
    const std::string analysis = ReadText(root + "\\analysis.json");
    Check(analysis.find("cortex_local_rules") != std::string::npos,
          "analysis.json should identify the local rule engine");
    Check(analysis.find("null_dereference") != std::string::npos,
          "analysis.json should contain findings");

    DeleteFileA((root + "\\report.json").c_str());
    DeleteFileA((root + "\\hooks.json").c_str());
    DeleteFileA((root + "\\values.json").c_str());
    DeleteFileA((root + "\\build_info.json").c_str());
    DeleteFileA((root + "\\hang_report.json").c_str());
    DeleteFileA((root + "\\analysis.json").c_str());
    DeleteFileA((root + "\\analysis.txt").c_str());
    RemoveDirectoryA(root.c_str());
}
} // namespace

int main() {
    TestSharedChannel();
    TestExternalCapture();
    TestAnalyzer();
    if (g_failures) {
        std::fprintf(stderr, "%d milestone 5-7 diagnostic test(s) failed\n", g_failures);
        return 1;
    }
    std::puts("diagnostics milestones 5-7 tests passed");
    return 0;
}
