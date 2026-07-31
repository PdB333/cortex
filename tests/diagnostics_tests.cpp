#define CORTEX_DIAGNOSTICS_TESTING 1

#include "../core/diagnostics/diagnostics.h"

#include <windows.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace {
int g_failures = 0;

void Check(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++g_failures;
}

std::string ReadFile(const char* path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

void TestJsonEscaping() {
    char escaped[128]{};
    Check(diagnostics::testing::EscapeJson("quote=\" slash=\\ line=\n", escaped, sizeof(escaped)),
          "JSON escaping should fit");
    Check(std::strcmp(escaped, "quote=\\\" slash=\\\\ line=\\n") == 0,
          "JSON escaping should encode control characters");

    char tiny[4]{};
    Check(!diagnostics::testing::EscapeJson("long", tiny, sizeof(tiny)),
          "JSON escaping should report truncation");
}

void TestBreadcrumbRing() {
    diagnostics::testing::ResetState();
    for (size_t i = 0; i < diagnostics::kBreadcrumbCapacity + 10; ++i) {
        char message[32]{};
        std::snprintf(message, sizeof(message), "event-%llu", static_cast<unsigned long long>(i));
        diagnostics::BreadcrumbLog("test", message, 0x1234);
    }

    std::array<diagnostics::Breadcrumb, diagnostics::kBreadcrumbCapacity> entries{};
    uint64_t dropped = 0;
    const size_t count = diagnostics::SnapshotBreadcrumbs(entries.data(), entries.size(), &dropped);
    Check(count == diagnostics::kBreadcrumbCapacity, "ring should retain its full capacity");
    Check(dropped == 10, "ring should report overwritten entries");
    Check(std::strcmp(entries.front().message, "event-10") == 0,
          "ring should return entries oldest-to-newest");
    Check(std::strcmp(entries.back().message, "event-521") == 0,
          "ring should retain the newest event");
}

void TestCrashContext() {
    EXCEPTION_RECORD record{};
    CONTEXT registers{};
    EXCEPTION_POINTERS pointers{&record, &registers};

    record.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    record.ExceptionAddress = reinterpret_cast<void*>(&TestCrashContext);
    record.NumberParameters = 2;
    record.ExceptionInformation[0] = 0;
    record.ExceptionInformation[1] = 0x28;
#ifdef _WIN64
    registers.Rax = 0x1111;
    registers.Rip = reinterpret_cast<DWORD64>(&TestCrashContext);
#else
    registers.Eax = 0x1111;
    registers.Eip = reinterpret_cast<DWORD>(&TestCrashContext);
#endif

    diagnostics::CrashContext context{};
    Check(diagnostics::testing::BuildCrashContext(&pointers, context),
          "crash context should build from valid exception pointers");
    Check(context.exceptionCode == EXCEPTION_ACCESS_VIOLATION,
          "crash context should preserve exception code");
    Check(context.accessedAddress == 0x28, "crash context should preserve accessed address");
    Check(context.accessType == diagnostics::AccessType::Read,
          "crash context should decode read access");
    Check(context.moduleBase != 0 && context.moduleRva != 0,
          "crash context should resolve module and RVA");
    Check(context.moduleName[0] != '\0', "crash context should include module name");
}

void TestReportWriting() {
    char temp[MAX_PATH]{};
    GetTempPathA(MAX_PATH, temp);
    char directory[MAX_PATH]{};
    std::snprintf(directory, sizeof(directory), "%scortex_diag_test_%lu",
                  temp, static_cast<unsigned long>(GetCurrentProcessId()));
    CreateDirectoryA(directory, nullptr);

    diagnostics::CrashContext context{};
    context.processId = GetCurrentProcessId();
    context.threadId = GetCurrentThreadId();
    context.exceptionCode = EXCEPTION_ACCESS_VIOLATION;
    context.instruction = 0x12345678;
    context.accessedAddress = 0x28;
    context.accessType = diagnostics::AccessType::Read;
    context.moduleBase = 0x12340000;
    context.moduleRva = 0x5678;
    std::strcpy(context.moduleName, "test\"mod.dll");
    std::strcpy(context.modulePath, "C:\\mods\\test\"mod.dll");

    Check(diagnostics::testing::WriteReport(directory, context, true, false, ERROR_WRITE_FAULT),
          "report should be written");
    char reportPath[MAX_PATH]{};
    std::snprintf(reportPath, sizeof(reportPath), "%s\\report.json", directory);
    const std::string report = ReadFile(reportPath);
    Check(report.find("EXCEPTION_ACCESS_VIOLATION") != std::string::npos,
          "report should name the exception");
    Check(report.find("test\\\"mod.dll") != std::string::npos,
          "report should JSON-escape module names");
    Check(report.find("\"rva\":\"0x") != std::string::npos,
          "report should include module RVA");

    DeleteFileA(reportPath);
    RemoveDirectoryA(directory);
}
} // namespace

int main() {
    TestJsonEscaping();
    TestBreadcrumbRing();
    TestCrashContext();
    TestReportWriting();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d diagnostics test(s) failed\n", g_failures);
        return 1;
    }
    std::puts("diagnostics tests passed");
    return 0;
}
