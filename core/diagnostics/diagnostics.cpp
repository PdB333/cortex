#include "diagnostics.h"

#include "../config.h"

#include <dbghelp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

namespace diagnostics {
namespace {

std::array<Breadcrumb, kBreadcrumbCapacity> g_breadcrumbs{};
std::atomic<uint64_t> g_nextSequence{0};
std::atomic_flag g_breadcrumbLock = ATOMIC_FLAG_INIT;
std::atomic_flag g_crashCaptureActive = ATOMIC_FLAG_INIT;
std::atomic<bool> g_enabled{false};
Options g_options{};
LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = nullptr;

class SpinGuard {
public:
    explicit SpinGuard(std::atomic_flag& flag) : flag_(flag) {
        while (flag_.test_and_set(std::memory_order_acquire)) Sleep(0);
    }
    ~SpinGuard() { flag_.clear(std::memory_order_release); }
private:
    std::atomic_flag& flag_;
};

void CopyText(char* destination, size_t destinationSize, const char* source) {
    if (!destination || destinationSize == 0) return;
    destination[0] = '\0';
    if (!source) return;
    std::strncpy(destination, source, destinationSize - 1);
    destination[destinationSize - 1] = '\0';
}

const char* BaseName(const char* path) {
    if (!path) return "";
    const char* slash = std::strrchr(path, '\\');
    const char* forward = std::strrchr(path, '/');
    const char* last = slash;
    if (!last || (forward && forward > last)) last = forward;
    return last ? last + 1 : path;
}

bool EscapeJsonImpl(const char* input, char* output, size_t outputSize) {
    if (!output || outputSize == 0) return false;
    output[0] = '\0';
    if (!input) return true;

    size_t used = 0;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(input); *p; ++p) {
        char encoded[7]{};
        const char* bytes = encoded;
        size_t count = 0;
        switch (*p) {
            case '"': encoded[0] = '\\'; encoded[1] = '"'; count = 2; break;
            case '\\': encoded[0] = '\\'; encoded[1] = '\\'; count = 2; break;
            case '\b': encoded[0] = '\\'; encoded[1] = 'b'; count = 2; break;
            case '\f': encoded[0] = '\\'; encoded[1] = 'f'; count = 2; break;
            case '\n': encoded[0] = '\\'; encoded[1] = 'n'; count = 2; break;
            case '\r': encoded[0] = '\\'; encoded[1] = 'r'; count = 2; break;
            case '\t': encoded[0] = '\\'; encoded[1] = 't'; count = 2; break;
            default:
                if (*p < 0x20) {
                    std::snprintf(encoded, sizeof(encoded), "\\u%04X", static_cast<unsigned>(*p));
                    count = 6;
                } else {
                    encoded[0] = static_cast<char>(*p);
                    count = 1;
                }
                break;
        }
        if (used + count + 1 > outputSize) {
            output[used] = '\0';
            return false;
        }
        std::memcpy(output + used, bytes, count);
        used += count;
    }
    output[used] = '\0';
    return true;
}

const char* ExceptionName(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION: return "EXCEPTION_ACCESS_VIOLATION";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "EXCEPTION_ILLEGAL_INSTRUCTION";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
        case EXCEPTION_STACK_OVERFLOW: return "EXCEPTION_STACK_OVERFLOW";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "EXCEPTION_DATATYPE_MISALIGNMENT";
        case EXCEPTION_PRIV_INSTRUCTION: return "EXCEPTION_PRIV_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR: return "EXCEPTION_IN_PAGE_ERROR";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_FLT_INVALID_OPERATION: return "EXCEPTION_FLT_INVALID_OPERATION";
        case EXCEPTION_FLT_OVERFLOW: return "EXCEPTION_FLT_OVERFLOW";
        default: return "UNKNOWN_EXCEPTION";
    }
}

const char* AccessName(AccessType access) {
    switch (access) {
        case AccessType::Read: return "read";
        case AccessType::Write: return "write";
        case AccessType::Execute: return "execute";
        default: return "unknown";
    }
}

void HexValue(uintptr_t value, char* output, size_t outputSize) {
#ifdef _WIN64
    std::snprintf(output, outputSize, "0x%016llX", static_cast<unsigned long long>(value));
#else
    std::snprintf(output, outputSize, "0x%08lX", static_cast<unsigned long>(value));
#endif
}

void TimestampSlug(char* output, size_t outputSize) {
    SYSTEMTIME time{};
    GetSystemTime(&time);
    std::snprintf(output, outputSize, "%04u%02u%02uT%02u%02u%02u.%03uZ",
                  time.wYear, time.wMonth, time.wDay,
                  time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
}

bool EnsureDirectory(const char* path) {
    if (!path || !*path) return false;
    if (CreateDirectoryA(path, nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

bool BuildCrashContextImpl(PEXCEPTION_POINTERS info, CrashContext& output) {
    if (!info || !info->ExceptionRecord || !info->ContextRecord) return false;
    output = {};
    output.schemaVersion = 1;
    output.processId = GetCurrentProcessId();
    output.threadId = GetCurrentThreadId();
    output.exceptionCode = info->ExceptionRecord->ExceptionCode;
    output.instruction = reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress);
    output.registers = *info->ContextRecord;

    if ((output.exceptionCode == EXCEPTION_ACCESS_VIOLATION ||
         output.exceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
        info->ExceptionRecord->NumberParameters >= 2) {
        const ULONG_PTR kind = info->ExceptionRecord->ExceptionInformation[0];
        output.accessedAddress = static_cast<uintptr_t>(info->ExceptionRecord->ExceptionInformation[1]);
        output.accessType = kind == 0 ? AccessType::Read :
                            kind == 1 ? AccessType::Write :
                            kind == 8 ? AccessType::Execute : AccessType::Unknown;
    }

    HMODULE module = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(output.instruction), &module)) {
        output.moduleBase = reinterpret_cast<uintptr_t>(module);
        output.moduleRva = output.instruction - output.moduleBase;
        GetModuleFileNameA(module, output.modulePath, static_cast<DWORD>(sizeof(output.modulePath)));
        CopyText(output.moduleName, sizeof(output.moduleName), BaseName(output.modulePath));
    }
    return true;
}

size_t SnapshotBreadcrumbsImpl(Breadcrumb* output, size_t capacity, uint64_t* dropped) {
    if (dropped) *dropped = 0;
    if (!output || capacity == 0) return 0;
    SpinGuard guard(g_breadcrumbLock);
    const uint64_t next = g_nextSequence.load(std::memory_order_acquire);
    const uint64_t retained = (std::min)(next, static_cast<uint64_t>(kBreadcrumbCapacity));
    const uint64_t first = next - retained;
    const size_t count = static_cast<size_t>((std::min)(retained, static_cast<uint64_t>(capacity)));
    const uint64_t selectedFirst = next - count;
    if (dropped) *dropped = selectedFirst;
    for (size_t i = 0; i < count; ++i)
        output[i] = g_breadcrumbs[(selectedFirst + i) % kBreadcrumbCapacity];
    return count;
}

bool WriteBreadcrumbs(const char* directory) {
    std::array<Breadcrumb, kBreadcrumbCapacity> snapshot{};
    uint64_t dropped = 0;
    const size_t count = SnapshotBreadcrumbsImpl(snapshot.data(), snapshot.size(), &dropped);

    char path[kCrashPathSize]{};
    std::snprintf(path, sizeof(path), "%s\\breadcrumbs.json", directory);
    FILE* file = nullptr;
    fopen_s(&file, path, "wb");
    if (!file) return false;

    std::fprintf(file, "{\n  \"capacity\": %llu,\n  \"dropped\": %llu,\n  \"entries\": [\n",
                 static_cast<unsigned long long>(kBreadcrumbCapacity),
                 static_cast<unsigned long long>(dropped));
    for (size_t i = 0; i < count; ++i) {
        char category[kBreadcrumbCategorySize * 6 + 1]{};
        char message[kBreadcrumbMessageSize * 6 + 1]{};
        EscapeJsonImpl(snapshot[i].category, category, sizeof(category));
        EscapeJsonImpl(snapshot[i].message, message, sizeof(message));
        char caller[32]{};
        HexValue(snapshot[i].caller, caller, sizeof(caller));
        std::fprintf(file,
                     "    {\"sequence\":%llu,\"timestamp_ms\":%llu,\"thread_id\":%lu,"
                     "\"caller\":\"%s\",\"category\":\"%s\",\"message\":\"%s\"}%s\n",
                     static_cast<unsigned long long>(snapshot[i].sequence),
                     static_cast<unsigned long long>(snapshot[i].timestampMs),
                     static_cast<unsigned long>(snapshot[i].threadId),
                     caller, category, message, i + 1 == count ? "" : ",");
    }
    std::fputs("  ]\n}\n", file);
    const bool ok = std::ferror(file) == 0;
    std::fclose(file);
    return ok;
}

bool WriteReportImpl(const char* directory, const CrashContext& context,
                     bool dumpAttempted, bool dumpWritten, DWORD dumpError) {
    char path[kCrashPathSize]{};
    std::snprintf(path, sizeof(path), "%s\\report.json", directory);
    FILE* file = nullptr;
    fopen_s(&file, path, "wb");
    if (!file) return false;

    char instruction[32]{}, accessed[32]{}, base[32]{}, rva[32]{};
    HexValue(context.instruction, instruction, sizeof(instruction));
    HexValue(context.accessedAddress, accessed, sizeof(accessed));
    HexValue(context.moduleBase, base, sizeof(base));
    HexValue(context.moduleRva, rva, sizeof(rva));
    char moduleName[MAX_PATH * 6 + 1]{}, modulePath[kCrashPathSize * 2]{};
    EscapeJsonImpl(context.moduleName, moduleName, sizeof(moduleName));
    EscapeJsonImpl(context.modulePath, modulePath, sizeof(modulePath));

    std::fprintf(file,
                 "{\n"
                 "  \"schema_version\": %u,\n"
                 "  \"process_id\": %lu,\n"
                 "  \"thread_id\": %lu,\n"
                 "  \"architecture\": \"%s\",\n"
                 "  \"exception\": {\n"
                 "    \"code\": \"0x%08lX\",\n"
                 "    \"name\": \"%s\",\n"
                 "    \"instruction\": \"%s\",\n"
                 "    \"operation\": \"%s\",\n"
                 "    \"accessed_address\": \"%s\"\n"
                 "  },\n"
                 "  \"module\": {\"name\":\"%s\",\"path\":\"%s\","
                 "\"base\":\"%s\",\"rva\":\"%s\",\"named_address\":\"%s+%s\"},\n"
#ifdef _WIN64
                 "  \"registers\": {\"rax\":\"0x%016llX\",\"rbx\":\"0x%016llX\","
                 "\"rcx\":\"0x%016llX\",\"rdx\":\"0x%016llX\","
                 "\"rsi\":\"0x%016llX\",\"rdi\":\"0x%016llX\","
                 "\"rbp\":\"0x%016llX\",\"rsp\":\"0x%016llX\","
                 "\"r8\":\"0x%016llX\",\"r9\":\"0x%016llX\","
                 "\"r10\":\"0x%016llX\",\"r11\":\"0x%016llX\","
                 "\"r12\":\"0x%016llX\",\"r13\":\"0x%016llX\","
                 "\"r14\":\"0x%016llX\",\"r15\":\"0x%016llX\","
                 "\"rip\":\"0x%016llX\",\"eflags\":\"0x%08lX\"},\n"
#else
                 "  \"registers\": {\"eax\":\"0x%08lX\",\"ebx\":\"0x%08lX\","
                 "\"ecx\":\"0x%08lX\",\"edx\":\"0x%08lX\","
                 "\"esi\":\"0x%08lX\",\"edi\":\"0x%08lX\","
                 "\"ebp\":\"0x%08lX\",\"esp\":\"0x%08lX\","
                 "\"eip\":\"0x%08lX\",\"eflags\":\"0x%08lX\"},\n"
#endif
                 "  \"minidump\": {\"attempted\":%s,\"written\":%s,\"error\":%lu}\n"
                 "}\n",
                 context.schemaVersion,
                 static_cast<unsigned long>(context.processId),
                 static_cast<unsigned long>(context.threadId),
#ifdef _WIN64
                 "x64",
#else
                 "x86",
#endif
                 static_cast<unsigned long>(context.exceptionCode), ExceptionName(context.exceptionCode),
                 instruction, AccessName(context.accessType), accessed,
                 moduleName, modulePath, base, rva, moduleName, rva,
#ifdef _WIN64
                 static_cast<unsigned long long>(context.registers.Rax),
                 static_cast<unsigned long long>(context.registers.Rbx),
                 static_cast<unsigned long long>(context.registers.Rcx),
                 static_cast<unsigned long long>(context.registers.Rdx),
                 static_cast<unsigned long long>(context.registers.Rsi),
                 static_cast<unsigned long long>(context.registers.Rdi),
                 static_cast<unsigned long long>(context.registers.Rbp),
                 static_cast<unsigned long long>(context.registers.Rsp),
                 static_cast<unsigned long long>(context.registers.R8),
                 static_cast<unsigned long long>(context.registers.R9),
                 static_cast<unsigned long long>(context.registers.R10),
                 static_cast<unsigned long long>(context.registers.R11),
                 static_cast<unsigned long long>(context.registers.R12),
                 static_cast<unsigned long long>(context.registers.R13),
                 static_cast<unsigned long long>(context.registers.R14),
                 static_cast<unsigned long long>(context.registers.R15),
                 static_cast<unsigned long long>(context.registers.Rip),
                 static_cast<unsigned long>(context.registers.EFlags),
#else
                 static_cast<unsigned long>(context.registers.Eax),
                 static_cast<unsigned long>(context.registers.Ebx),
                 static_cast<unsigned long>(context.registers.Ecx),
                 static_cast<unsigned long>(context.registers.Edx),
                 static_cast<unsigned long>(context.registers.Esi),
                 static_cast<unsigned long>(context.registers.Edi),
                 static_cast<unsigned long>(context.registers.Ebp),
                 static_cast<unsigned long>(context.registers.Esp),
                 static_cast<unsigned long>(context.registers.Eip),
                 static_cast<unsigned long>(context.registers.EFlags),
#endif
                 dumpAttempted ? "true" : "false", dumpWritten ? "true" : "false",
                 static_cast<unsigned long>(dumpError));
    const bool ok = std::ferror(file) == 0;
    std::fclose(file);
    return ok;
}

bool WriteMiniDump(const char* directory, PEXCEPTION_POINTERS info, DWORD& error) {
    error = ERROR_SUCCESS;
    char path[kCrashPathSize]{};
    std::snprintf(path, sizeof(path), "%s\\crash.dmp", directory);
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        return false;
    }

    MINIDUMP_EXCEPTION_INFORMATION exceptionInfo{};
    exceptionInfo.ThreadId = GetCurrentThreadId();
    exceptionInfo.ExceptionPointers = info;
    exceptionInfo.ClientPointers = FALSE;
    const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
        MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
        MiniDumpWithIndirectlyReferencedMemory);
    const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type,
                                      &exceptionInfo, nullptr, nullptr);
    if (!ok) error = GetLastError();
    FlushFileBuffers(file);
    CloseHandle(file);
    return ok != FALSE;
}

LONG WINAPI UnhandledExceptionFilterImpl(PEXCEPTION_POINTERS info) {
    if (!g_enabled.load(std::memory_order_acquire) || !info)
        return g_previousFilter ? g_previousFilter(info) : EXCEPTION_CONTINUE_SEARCH;
    if (g_crashCaptureActive.test_and_set(std::memory_order_acq_rel))
        return g_previousFilter ? g_previousFilter(info) : EXCEPTION_CONTINUE_SEARCH;

    CrashContext context{};
    if (BuildCrashContextImpl(info, context)) {
        char timestamp[48]{};
        TimestampSlug(timestamp, sizeof(timestamp));
        char directory[kCrashPathSize]{};
        std::snprintf(directory, sizeof(directory), "%s\\crash_%s_%lu",
                      g_options.outputDirectory.c_str(), timestamp,
                      static_cast<unsigned long>(context.processId));
        if (EnsureDirectory(directory)) {
            WriteBreadcrumbs(directory);
            DWORD dumpError = ERROR_SUCCESS;
            const bool attempted = g_options.writeMinidump;
            const bool written = attempted && WriteMiniDump(directory, info, dumpError);
            WriteReportImpl(directory, context, attempted, written, dumpError);
        }
    }

    return g_previousFilter ? g_previousFilter(info) : EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

bool Init(const Options& options) {
    if (g_enabled.load(std::memory_order_acquire)) return true;
    g_options = options;
    if (!g_options.enabled) return true;
    if (g_options.outputDirectory.empty())
        g_options.outputDirectory = config::GetModuleDir() + "\\cortex_crashes";
    if (!EnsureDirectory(g_options.outputDirectory.c_str())) return false;
    g_crashCaptureActive.clear(std::memory_order_release);
    g_previousFilter = SetUnhandledExceptionFilter(UnhandledExceptionFilterImpl);
    g_enabled.store(true, std::memory_order_release);
    BreadcrumbLog("diagnostics", "crash reporter initialized");
    return true;
}

void Shutdown() {
    if (!g_enabled.exchange(false, std::memory_order_acq_rel)) return;
    BreadcrumbLog("diagnostics", "crash reporter shutdown");
    SetUnhandledExceptionFilter(g_previousFilter);
    g_previousFilter = nullptr;
}

bool IsEnabled() { return g_enabled.load(std::memory_order_acquire); }

void BreadcrumbLog(const char* category, const char* message, uintptr_t caller) {
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    SpinGuard guard(g_breadcrumbLock);
    const uint64_t index = g_nextSequence.fetch_add(1, std::memory_order_relaxed);
    Breadcrumb& entry = g_breadcrumbs[index % kBreadcrumbCapacity];
    entry = {};
    entry.sequence = index + 1;
    entry.timestampMs = GetTickCount64();
    entry.threadId = GetCurrentThreadId();
    entry.caller = caller;
    CopyText(entry.category, sizeof(entry.category), category ? category : "user");
    CopyText(entry.message, sizeof(entry.message), message ? message : "");
}

size_t SnapshotBreadcrumbs(Breadcrumb* output, size_t capacity, uint64_t* dropped) {
    return SnapshotBreadcrumbsImpl(output, capacity, dropped);
}

#ifdef CORTEX_DIAGNOSTICS_TESTING
namespace testing {
void ResetState() {
    SpinGuard guard(g_breadcrumbLock);
    g_breadcrumbs = {};
    g_nextSequence.store(0, std::memory_order_release);
    g_crashCaptureActive.clear(std::memory_order_release);
    g_enabled.store(true, std::memory_order_release);
}
bool EscapeJson(const char* input, char* output, size_t outputSize) {
    return EscapeJsonImpl(input, output, outputSize);
}
bool BuildCrashContext(PEXCEPTION_POINTERS info, CrashContext& output) {
    return BuildCrashContextImpl(info, output);
}
bool WriteReport(const char* directory, const CrashContext& context,
                 bool dumpAttempted, bool dumpWritten, DWORD dumpError) {
    return WriteReportImpl(directory, context, dumpAttempted, dumpWritten, dumpError);
}
} // namespace testing
#endif

} // namespace diagnostics

extern "C" __declspec(dllexport) void CortexDiagBreadcrumb(const char* category,
                                                            const char* message) {
#if defined(_MSC_VER)
    const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
#elif defined(__GNUC__)
    const uintptr_t caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
#else
    const uintptr_t caller = 0;
#endif
    diagnostics::BreadcrumbLog(category, message, caller);
}
