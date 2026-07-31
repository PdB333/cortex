#pragma once

#include <windows.h>
#include <dbghelp.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace diagnostics {

constexpr size_t kBreadcrumbCapacity = 512;
constexpr size_t kBreadcrumbMessageSize = 160;

struct Breadcrumb {
    uint64_t sequence = 0;
    uint64_t timestampMs = 0;
    DWORD threadId = 0;
    uintptr_t caller = 0;
    char category[32]{};
    char message[kBreadcrumbMessageSize]{};
};

namespace detail {
inline std::array<Breadcrumb, kBreadcrumbCapacity> g_breadcrumbs{};
inline std::atomic<uint64_t> g_nextSequence{0};
inline std::atomic_flag g_crashCaptureActive = ATOMIC_FLAG_INIT;
inline PVOID g_vehHandle = nullptr;
inline std::string g_outputDirectory;

inline std::string ModuleDirectory() {
    HMODULE module = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&ModuleDirectory), &module);

    char path[MAX_PATH]{};
    GetModuleFileNameA(module, path, MAX_PATH);
    std::string full(path);
    const size_t slash = full.find_last_of("\\/");
    return slash == std::string::npos ? "." : full.substr(0, slash);
}

inline std::string Hex(uintptr_t value) {
    char buffer[2 + sizeof(uintptr_t) * 2 + 1]{};
#ifdef _WIN64
    std::snprintf(buffer, sizeof(buffer), "0x%llX", static_cast<unsigned long long>(value));
#else
    std::snprintf(buffer, sizeof(buffer), "0x%lX", static_cast<unsigned long>(value));
#endif
    return buffer;
}

inline std::string TimestampSlug() {
    SYSTEMTIME time{};
    GetSystemTime(&time);
    char buffer[40]{};
    std::snprintf(buffer, sizeof(buffer), "%04u%02u%02uT%02u%02u%02u.%03uZ",
                  time.wYear, time.wMonth, time.wDay,
                  time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
    return buffer;
}

inline const char* ExceptionName(DWORD code) {
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

inline bool IsInterestingException(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        case EXCEPTION_DATATYPE_MISALIGNMENT:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        case EXCEPTION_FLT_INVALID_OPERATION:
        case EXCEPTION_FLT_OVERFLOW:
            return true;
        default:
            return false;
    }
}

inline void CopyText(char* destination, size_t destinationSize, const char* source) {
    if (!destination || destinationSize == 0) return;
    destination[0] = '\0';
    if (!source) return;
    std::strncpy(destination, source, destinationSize - 1);
    destination[destinationSize - 1] = '\0';
}

inline void WriteBreadcrumbs(const std::string& path) {
    FILE* file = nullptr;
    fopen_s(&file, path.c_str(), "wb");
    if (!file) return;

    std::fputs("[\n", file);
    const uint64_t next = g_nextSequence.load(std::memory_order_acquire);
    const uint64_t first = next > kBreadcrumbCapacity ? next - kBreadcrumbCapacity : 0;
    bool needsComma = false;

    for (uint64_t sequence = first; sequence < next; ++sequence) {
        const Breadcrumb& entry = g_breadcrumbs[sequence % kBreadcrumbCapacity];
        if (entry.sequence != sequence + 1) continue;
        if (needsComma) std::fputs(",\n", file);
        needsComma = true;
        std::fprintf(file,
                     "  {\"sequence\":%llu,\"timestamp_ms\":%llu,\"thread_id\":%lu,"
                     "\"caller\":\"%s\",\"category\":\"%s\",\"message\":\"%s\"}",
                     static_cast<unsigned long long>(entry.sequence),
                     static_cast<unsigned long long>(entry.timestampMs),
                     static_cast<unsigned long>(entry.threadId),
                     Hex(entry.caller).c_str(), entry.category, entry.message);
    }

    std::fputs("\n]\n", file);
    std::fclose(file);
}

inline void WriteReport(const std::string& directory, PEXCEPTION_POINTERS info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    const uintptr_t instruction = reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress);

    uintptr_t accessedAddress = 0;
    const char* operation = "unknown";
    if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) &&
        info->ExceptionRecord->NumberParameters >= 2) {
        const ULONG_PTR kind = info->ExceptionRecord->ExceptionInformation[0];
        accessedAddress = static_cast<uintptr_t>(info->ExceptionRecord->ExceptionInformation[1]);
        operation = kind == 0 ? "read" : (kind == 1 ? "write" : (kind == 8 ? "execute" : "unknown"));
    }

    char modulePath[MAX_PATH]{};
    HMODULE owner = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(instruction), &owner)) {
        GetModuleFileNameA(owner, modulePath, MAX_PATH);
    }

    FILE* file = nullptr;
    const std::string reportPath = directory + "\\report.json";
    fopen_s(&file, reportPath.c_str(), "wb");
    if (!file) return;

    std::fprintf(file,
                 "{\n"
                 "  \"schema_version\": 1,\n"
                 "  \"process_id\": %lu,\n"
                 "  \"thread_id\": %lu,\n"
                 "  \"exception\": {\n"
                 "    \"code\": \"0x%08lX\",\n"
                 "    \"name\": \"%s\",\n"
                 "    \"instruction\": \"%s\",\n"
                 "    \"operation\": \"%s\",\n"
                 "    \"accessed_address\": \"%s\"\n"
                 "  },\n"
                 "  \"suspected_module_path\": \"%s\"\n"
                 "}\n",
                 static_cast<unsigned long>(GetCurrentProcessId()),
                 static_cast<unsigned long>(GetCurrentThreadId()),
                 static_cast<unsigned long>(code), ExceptionName(code),
                 Hex(instruction).c_str(), operation, Hex(accessedAddress).c_str(), modulePath);
    std::fclose(file);
}

inline void WriteMiniDump(const std::string& directory, PEXCEPTION_POINTERS info) {
    const std::string dumpPath = directory + "\\crash.dmp";
    HANDLE file = CreateFileA(dumpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    MINIDUMP_EXCEPTION_INFORMATION exceptionInfo{};
    exceptionInfo.ThreadId = GetCurrentThreadId();
    exceptionInfo.ExceptionPointers = info;
    exceptionInfo.ClientPointers = FALSE;

    const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
        MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
        MiniDumpWithIndirectlyReferencedMemory);

    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type,
                      &exceptionInfo, nullptr, nullptr);
    CloseHandle(file);
}

inline LONG CALLBACK ExceptionObserver(PEXCEPTION_POINTERS info) {
    if (!info || !info->ExceptionRecord || !IsInterestingException(info->ExceptionRecord->ExceptionCode))
        return EXCEPTION_CONTINUE_SEARCH;

    if (g_crashCaptureActive.test_and_set(std::memory_order_acq_rel))
        return EXCEPTION_CONTINUE_SEARCH;

    const std::string directory = g_outputDirectory + "\\crash_" + TimestampSlug() + "_" +
                                  std::to_string(GetCurrentProcessId());
    CreateDirectoryA(directory.c_str(), nullptr);

    WriteReport(directory, info);
    WriteBreadcrumbs(directory + "\\breadcrumbs.json");
    WriteMiniDump(directory, info);

    return EXCEPTION_CONTINUE_SEARCH;
}
} // namespace detail

inline void BreadcrumbLog(const char* category, const char* message, uintptr_t caller = 0) {
    const uint64_t index = detail::g_nextSequence.fetch_add(1, std::memory_order_acq_rel);
    Breadcrumb entry{};
    entry.sequence = index + 1;
    entry.timestampMs = GetTickCount64();
    entry.threadId = GetCurrentThreadId();
    entry.caller = caller;
    detail::CopyText(entry.category, sizeof(entry.category), category ? category : "user");
    detail::CopyText(entry.message, sizeof(entry.message), message ? message : "");
    detail::g_breadcrumbs[index % kBreadcrumbCapacity] = entry;
    std::atomic_thread_fence(std::memory_order_release);
}

inline bool Init() {
    if (detail::g_vehHandle) return true;
    detail::g_outputDirectory = detail::ModuleDirectory() + "\\cortex_crashes";
    CreateDirectoryA(detail::g_outputDirectory.c_str(), nullptr);
    detail::g_vehHandle = AddVectoredExceptionHandler(0, detail::ExceptionObserver);
    BreadcrumbLog("diagnostics", "crash observer initialized");
    return detail::g_vehHandle != nullptr;
}

inline void Shutdown() {
    BreadcrumbLog("diagnostics", "crash observer shutdown");
    if (detail::g_vehHandle) {
        RemoveVectoredExceptionHandler(detail::g_vehHandle);
        detail::g_vehHandle = nullptr;
    }
}

} // namespace diagnostics

extern "C" __declspec(dllexport) void CortexDiagBreadcrumb(const char* category, const char* message) {
    diagnostics::BreadcrumbLog(category, message,
                               reinterpret_cast<uintptr_t>(_ReturnAddress()));
}
