#include "external_host.h"

#include <dbghelp.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace hostdiag {
namespace {

void BuildObjectName(char* output, size_t outputSize, const char* prefix, DWORD pid) {
    std::snprintf(output, outputSize, "%s%lu", prefix, static_cast<unsigned long>(pid));
}

bool LockShared(CortexDiagSharedState* state) {
    if (!state) return false;
    for (int i = 0; i < 512; ++i) {
        if (InterlockedCompareExchange(&state->lock, 1, 0) == 0) return true;
        YieldProcessor();
    }
    return false;
}

void UnlockShared(CortexDiagSharedState* state) {
    if (state) InterlockedExchange(&state->lock, 0);
}

bool EnsureDirectoryTree(const std::string& path) {
    if (path.empty()) return false;
    std::string current;
    current.reserve(path.size());
    for (size_t i = 0; i < path.size(); ++i) {
        const char c = path[i];
        current.push_back(c);
        if (c != '\\' && c != '/') continue;
        if (current.size() <= 3) continue;
        CreateDirectoryA(current.c_str(), nullptr);
    }
    if (CreateDirectoryA(path.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

struct WindowProbe {
    DWORD processId = 0;
    bool found = false;
    bool responsive = false;
};

BOOL CALLBACK ProbeWindow(HWND window, LPARAM parameter) {
    auto* probe = reinterpret_cast<WindowProbe*>(parameter);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid != probe->processId || !IsWindowVisible(window) || GetWindow(window, GW_OWNER)) return TRUE;
    probe->found = true;
    DWORD_PTR result = 0;
    probe->responsive = SendMessageTimeoutA(window, WM_NULL, 0, 0,
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 250, &result) != 0;
    return FALSE;
}

} // namespace

SharedClient::~SharedClient() { Close(); }

bool SharedClient::Open(DWORD processId, std::string& error) {
    Close();
    char mappingName[96]{};
    char eventName[96]{};
    BuildObjectName(mappingName, sizeof(mappingName), CORTEX_DIAG_MAPPING_PREFIX, processId);
    BuildObjectName(eventName, sizeof(eventName), CORTEX_DIAG_EVENT_PREFIX, processId);
    mapping_ = OpenFileMappingA(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, mappingName);
    if (!mapping_) {
        error = "shared_mapping_not_found:" + std::to_string(GetLastError());
        return false;
    }
    state_ = static_cast<CortexDiagSharedState*>(
        MapViewOfFile(mapping_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(CortexDiagSharedState)));
    if (!state_) {
        error = "shared_mapping_map_failed:" + std::to_string(GetLastError());
        CloseHandle(mapping_);
        mapping_ = nullptr;
        return false;
    }
    event_ = OpenEventA(SYNCHRONIZE, FALSE, eventName);
    if (!event_) {
        error = "shared_event_not_found:" + std::to_string(GetLastError());
        UnmapViewOfFile(state_);
        CloseHandle(mapping_);
        state_ = nullptr;
        mapping_ = nullptr;
        return false;
    }
    error.clear();
    return true;
}

void SharedClient::Close() {
    if (event_) CloseHandle(event_);
    if (state_) UnmapViewOfFile(state_);
    if (mapping_) CloseHandle(mapping_);
    event_ = nullptr;
    state_ = nullptr;
    mapping_ = nullptr;
}

bool SharedClient::Wait(DWORD timeoutMs) const {
    return event_ && WaitForSingleObject(event_, timeoutMs) == WAIT_OBJECT_0;
}

bool SharedClient::Snapshot(SharedSnapshot& output, std::string& error) const {
    output = {};
    if (!state_) {
        error = "shared_channel_not_open";
        return false;
    }
    if (!LockShared(state_)) {
        error = "shared_channel_busy";
        return false;
    }
    if (state_->magic != CORTEX_DIAG_SHARED_MAGIC ||
        state_->version != CORTEX_DIAG_SHARED_VERSION) {
        UnlockShared(state_);
        error = "shared_protocol_mismatch";
        return false;
    }
    output.available = true;
    output.ready = InterlockedCompareExchange(&state_->ready, 1, 1) == 1;
    output.processId = state_->process_id;
    output.pointerSize = state_->pointer_size;
    output.sameBitness = state_->pointer_size == sizeof(void*);
    output.startedTickMs = static_cast<uint64_t>(state_->started_tick_ms);
    output.lastCoreHeartbeatMs = static_cast<uint64_t>(state_->last_core_heartbeat_ms);
    CortexDiagSharedCrash crashCopy{};
    std::memcpy(&crashCopy, const_cast<const CortexDiagSharedCrash*>(&state_->crash),
                sizeof(crashCopy));
    output.crash = crashCopy;
    const LONG count = (std::min)(state_->heartbeat_count,
                                  static_cast<LONG>(CORTEX_DIAG_MAX_HEARTBEATS));
    output.heartbeats.reserve(static_cast<size_t>(count));
    for (LONG i = 0; i < count; ++i) {
        const CortexDiagSharedHeartbeat& source = state_->heartbeats[i];
        HeartbeatSnapshot heartbeat;
        heartbeat.source.assign(source.source,
            strnlen(source.source, CORTEX_DIAG_HEARTBEAT_NAME_SIZE));
        heartbeat.threadId = source.thread_id;
        heartbeat.lastTickMs = static_cast<uint64_t>(source.last_tick_ms);
        heartbeat.sequence = static_cast<uint64_t>(source.sequence);
        output.heartbeats.push_back(std::move(heartbeat));
    }
    UnlockShared(state_);
    error.clear();
    return true;
}

std::string MakeCaptureDirectory(const std::string& root, const char* prefix, DWORD processId) {
    EnsureDirectoryTree(root);
    SYSTEMTIME time{};
    GetLocalTime(&time);
    char name[160]{};
    std::snprintf(name, sizeof(name), "%s_%04u%02u%02u_%02u%02u%02u_%03u_%lu",
                  prefix ? prefix : "capture", time.wYear, time.wMonth, time.wDay,
                  time.wHour, time.wMinute, time.wSecond, time.wMilliseconds,
                  static_cast<unsigned long>(processId));
    std::string directory = root;
    if (!directory.empty() && directory.back() != '\\' && directory.back() != '/') directory += '\\';
    directory += name;
    EnsureDirectoryTree(directory);
    return directory;
}

bool FindNewestCrashDirectory(const std::string& root, DWORD processId, std::string& output) {
    char pattern[MAX_PATH * 2]{};
    std::snprintf(pattern, sizeof(pattern), "%s\\crash_*_%lu", root.c_str(),
                  static_cast<unsigned long>(processId));
    WIN32_FIND_DATAA data{};
    HANDLE find = FindFirstFileA(pattern, &data);
    if (find == INVALID_HANDLE_VALUE) return false;
    std::string newest;
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
        if (newest.empty() || newest < data.cFileName) newest = data.cFileName;
    } while (FindNextFileA(find, &data));
    FindClose(find);
    if (newest.empty()) return false;
    output = root + "\\" + newest;
    return true;
}

DumpResult WriteProcessDump(DWORD processId, const std::string& path,
                            const CortexDiagSharedCrash* crash, bool fullMemory) {
    DumpResult result;
    result.path = path;
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE,
                                 FALSE, processId);
    if (!process) {
        result.error = GetLastError();
        return result;
    }
    HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        result.error = GetLastError();
        CloseHandle(process);
        return result;
    }
    MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
        MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
        MiniDumpWithIndirectlyReferencedMemory);
    if (fullMemory) type = static_cast<MINIDUMP_TYPE>(type | MiniDumpWithFullMemory);

    MINIDUMP_EXCEPTION_INFORMATION exceptionInfo{};
    EXCEPTION_RECORD record{};
    CONTEXT context{};
    EXCEPTION_POINTERS pointers{};
    MINIDUMP_EXCEPTION_INFORMATION* exceptionPointer = nullptr;
    if (crash && crash->sequence != 0 && crash->thread_id != 0) {
        record.ExceptionCode = crash->exception_code;
        record.ExceptionAddress = reinterpret_cast<PVOID>(static_cast<uintptr_t>(crash->exception_address));
        if (crash->accessed_address) {
            record.NumberParameters = 2;
            record.ExceptionInformation[0] = crash->access_type == CORTEX_DIAG_ACCESS_WRITE ? 1 :
                                             crash->access_type == CORTEX_DIAG_ACCESS_EXECUTE ? 8 : 0;
            record.ExceptionInformation[1] = static_cast<ULONG_PTR>(crash->accessed_address);
        }
        context = crash->context;
        pointers.ExceptionRecord = &record;
        pointers.ContextRecord = &context;
        exceptionInfo.ThreadId = crash->thread_id;
        exceptionInfo.ExceptionPointers = &pointers;
        exceptionInfo.ClientPointers = FALSE;
        exceptionPointer = &exceptionInfo;
    }
    result.success = MiniDumpWriteDump(process, processId, file, type,
                                       exceptionPointer, nullptr, nullptr) != FALSE;
    if (!result.success) result.error = GetLastError();
    FlushFileBuffers(file);
    CloseHandle(file);
    CloseHandle(process);
    return result;
}

bool CaptureThreads(DWORD processId, const std::string& path,
                    std::vector<ThreadSnapshot>* snapshots, std::string& error) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        error = "thread_snapshot_failed:" + std::to_string(GetLastError());
        return false;
    }
    FILE* file = nullptr;
    fopen_s(&file, path.c_str(), "wb");
    if (!file) {
        CloseHandle(snapshot);
        error = "thread_output_open_failed";
        return false;
    }
    std::fputs("{\n  \"threads\":[\n", file);
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    bool first = true;
    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID != processId) continue;
            ThreadSnapshot thread;
            thread.threadId = entry.th32ThreadID;
            HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                       THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
            if (!handle) {
                thread.error = GetLastError();
            } else {
                const DWORD suspended = SuspendThread(handle);
                if (suspended == static_cast<DWORD>(-1)) {
                    thread.error = GetLastError();
                } else {
                    thread.suspendCount = suspended;
                    CONTEXT context{};
                    context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                    if (GetThreadContext(handle, &context)) {
#ifdef _WIN64
                        thread.instruction = context.Rip;
                        thread.stackPointer = context.Rsp;
                        thread.framePointer = context.Rbp;
#else
                        thread.instruction = context.Eip;
                        thread.stackPointer = context.Esp;
                        thread.framePointer = context.Ebp;
#endif
                    } else {
                        thread.error = GetLastError();
                    }
                    ResumeThread(handle);
                }
                CloseHandle(handle);
            }
            if (snapshots) snapshots->push_back(thread);
            if (!first) std::fputs(",\n", file);
            first = false;
            std::fprintf(file,
                         "    {\"thread_id\":%lu,\"instruction\":\"0x%llX\","
                         "\"stack_pointer\":\"0x%llX\",\"frame_pointer\":\"0x%llX\","
                         "\"previous_suspend_count\":%lu,\"error\":%lu}",
                         static_cast<unsigned long>(thread.threadId),
                         static_cast<unsigned long long>(thread.instruction),
                         static_cast<unsigned long long>(thread.stackPointer),
                         static_cast<unsigned long long>(thread.framePointer),
                         static_cast<unsigned long>(thread.suspendCount),
                         static_cast<unsigned long>(thread.error));
        } while (Thread32Next(snapshot, &entry));
    }
    std::fputs("\n  ]\n}\n", file);
    const bool ok = std::ferror(file) == 0;
    std::fclose(file);
    CloseHandle(snapshot);
    error = ok ? std::string() : "thread_output_write_failed";
    return ok;
}

bool IsWindowResponsive(DWORD processId, bool& windowFound) {
    WindowProbe probe;
    probe.processId = processId;
    EnumWindows(ProbeWindow, reinterpret_cast<LPARAM>(&probe));
    windowFound = probe.found;
    return !probe.found || probe.responsive;
}

bool IsProcessAlive(HANDLE process) {
    return process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
}

uint64_t HeartbeatAgeMs(const SharedSnapshot& snapshot, const std::string& source,
                        uint64_t nowTickMs, bool& found) {
    found = false;
    if (source.empty() || _stricmp(source.c_str(), "cortex_core") == 0) {
        if (!snapshot.lastCoreHeartbeatMs) return 0;
        found = true;
        return nowTickMs >= snapshot.lastCoreHeartbeatMs
            ? nowTickMs - snapshot.lastCoreHeartbeatMs : 0;
    }
    for (const HeartbeatSnapshot& heartbeat : snapshot.heartbeats) {
        if (_stricmp(heartbeat.source.c_str(), source.c_str()) != 0) continue;
        found = true;
        return nowTickMs >= heartbeat.lastTickMs ? nowTickMs - heartbeat.lastTickMs : 0;
    }
    return 0;
}

} // namespace hostdiag
