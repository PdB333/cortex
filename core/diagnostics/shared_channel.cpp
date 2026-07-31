#include "shared_channel.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

namespace diagnostics {
namespace shared_detail {

HANDLE g_mapping = nullptr;
HANDLE g_event = nullptr;
CortexDiagSharedState* g_state = nullptr;
LPTOP_LEVEL_EXCEPTION_FILTER g_previousSharedFilter = nullptr;
std::atomic<bool> g_sharedRunning{false};
std::thread g_heartbeatWorker;

void BuildSharedName(char* output, size_t outputSize, const char* prefix, DWORD pid) {
    if (!output || outputSize == 0) return;
    std::snprintf(output, outputSize, "%s%lu", prefix, static_cast<unsigned long>(pid));
}

void CopySharedText(char* destination, size_t destinationSize, const char* source) {
    if (!destination || destinationSize == 0) return;
    destination[0] = '\0';
    if (!source) return;
    std::strncpy(destination, source, destinationSize - 1);
    destination[destinationSize - 1] = '\0';
}

bool TrySharedLock(unsigned attempts = 256) {
    if (!g_state) return false;
    for (unsigned i = 0; i < attempts; ++i) {
        if (InterlockedCompareExchange(&g_state->lock, 1, 0) == 0) return true;
        YieldProcessor();
    }
    return false;
}

void SharedUnlock() {
    if (g_state) InterlockedExchange(&g_state->lock, 0);
}

int FindHeartbeatLocked(const char* source) {
    const LONG count = (std::min)(g_state->heartbeat_count,
                                  static_cast<LONG>(CORTEX_DIAG_MAX_HEARTBEATS));
    for (LONG i = 0; i < count; ++i) {
        if (std::strncmp(g_state->heartbeats[i].source, source,
                         CORTEX_DIAG_HEARTBEAT_NAME_SIZE) == 0) return static_cast<int>(i);
    }
    return -1;
}

void HeartbeatWorker() {
    while (g_sharedRunning.load(std::memory_order_acquire)) {
        SharedHeartbeat("cortex_core");
        for (int i = 0; i < 10 && g_sharedRunning.load(std::memory_order_acquire); ++i) Sleep(50);
    }
}

LONG WINAPI SharedUnhandledFilter(PEXCEPTION_POINTERS pointers) {
    SharedPublishCrash(pointers);
    return g_previousSharedFilter ? g_previousSharedFilter(pointers) : EXCEPTION_CONTINUE_SEARCH;
}

} // namespace shared_detail

bool SharedChannelInit() {
    bool expected = false;
    if (!shared_detail::g_sharedRunning.compare_exchange_strong(expected, true)) return true;

    const DWORD pid = GetCurrentProcessId();
    char mappingName[96]{};
    char eventName[96]{};
    shared_detail::BuildSharedName(mappingName, sizeof(mappingName), CORTEX_DIAG_MAPPING_PREFIX, pid);
    shared_detail::BuildSharedName(eventName, sizeof(eventName), CORTEX_DIAG_EVENT_PREFIX, pid);

    shared_detail::g_mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                                  0, sizeof(CortexDiagSharedState), mappingName);
    if (!shared_detail::g_mapping) {
        shared_detail::g_sharedRunning.store(false, std::memory_order_release);
        return false;
    }
    shared_detail::g_state = static_cast<CortexDiagSharedState*>(
        MapViewOfFile(shared_detail::g_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(CortexDiagSharedState)));
    if (!shared_detail::g_state) {
        CloseHandle(shared_detail::g_mapping);
        shared_detail::g_mapping = nullptr;
        shared_detail::g_sharedRunning.store(false, std::memory_order_release);
        return false;
    }
    std::memset(shared_detail::g_state, 0, sizeof(CortexDiagSharedState));
    shared_detail::g_state->magic = CORTEX_DIAG_SHARED_MAGIC;
    shared_detail::g_state->version = CORTEX_DIAG_SHARED_VERSION;
    shared_detail::g_state->process_id = pid;
    shared_detail::g_state->pointer_size = sizeof(void*);
    InterlockedExchange64(&shared_detail::g_state->started_tick_ms,
                          static_cast<LONG64>(GetTickCount64()));

    shared_detail::g_event = CreateEventA(nullptr, FALSE, FALSE, eventName);
    if (!shared_detail::g_event) {
        UnmapViewOfFile(shared_detail::g_state);
        CloseHandle(shared_detail::g_mapping);
        shared_detail::g_state = nullptr;
        shared_detail::g_mapping = nullptr;
        shared_detail::g_sharedRunning.store(false, std::memory_order_release);
        return false;
    }

    InterlockedExchange(&shared_detail::g_state->ready, 1);
    SharedHeartbeat("cortex_core");
    shared_detail::g_previousSharedFilter = SetUnhandledExceptionFilter(shared_detail::SharedUnhandledFilter);
    try {
        shared_detail::g_heartbeatWorker = std::thread(shared_detail::HeartbeatWorker);
    } catch (...) {
        SetUnhandledExceptionFilter(shared_detail::g_previousSharedFilter);
        shared_detail::g_previousSharedFilter = nullptr;
        InterlockedExchange(&shared_detail::g_state->ready, 0);
        CloseHandle(shared_detail::g_event);
        UnmapViewOfFile(shared_detail::g_state);
        CloseHandle(shared_detail::g_mapping);
        shared_detail::g_event = nullptr;
        shared_detail::g_state = nullptr;
        shared_detail::g_mapping = nullptr;
        shared_detail::g_sharedRunning.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

void SharedChannelShutdown() {
    if (!shared_detail::g_sharedRunning.exchange(false, std::memory_order_acq_rel)) return;
    if (shared_detail::g_heartbeatWorker.joinable()) shared_detail::g_heartbeatWorker.join();
    SetUnhandledExceptionFilter(shared_detail::g_previousSharedFilter);
    shared_detail::g_previousSharedFilter = nullptr;
    if (shared_detail::g_state) InterlockedExchange(&shared_detail::g_state->ready, 0);
    if (shared_detail::g_event) CloseHandle(shared_detail::g_event);
    if (shared_detail::g_state) UnmapViewOfFile(shared_detail::g_state);
    if (shared_detail::g_mapping) CloseHandle(shared_detail::g_mapping);
    shared_detail::g_event = nullptr;
    shared_detail::g_state = nullptr;
    shared_detail::g_mapping = nullptr;
}

bool IsSharedChannelReady() {
    return shared_detail::g_state &&
           InterlockedCompareExchange(&shared_detail::g_state->ready, 1, 1) == 1;
}

void SharedHeartbeat(const char* source) {
    if (!shared_detail::g_state || !source || !*source) return;
    const LONG64 now = static_cast<LONG64>(GetTickCount64());
    if (std::strcmp(source, "cortex_core") == 0)
        InterlockedExchange64(&shared_detail::g_state->last_core_heartbeat_ms, now);
    if (!shared_detail::TrySharedLock()) return;
    int index = shared_detail::FindHeartbeatLocked(source);
    if (index < 0) {
        LONG count = shared_detail::g_state->heartbeat_count;
        if (count < static_cast<LONG>(CORTEX_DIAG_MAX_HEARTBEATS)) {
            index = static_cast<int>(count);
            shared_detail::CopySharedText(shared_detail::g_state->heartbeats[index].source,
                                          CORTEX_DIAG_HEARTBEAT_NAME_SIZE, source);
            InterlockedExchange(&shared_detail::g_state->heartbeat_count, count + 1);
        }
    }
    if (index >= 0) {
        CortexDiagSharedHeartbeat& heartbeat = shared_detail::g_state->heartbeats[index];
        heartbeat.thread_id = GetCurrentThreadId();
        InterlockedExchange64(&heartbeat.last_tick_ms, now);
        InterlockedIncrement64(&heartbeat.sequence);
    }
    shared_detail::SharedUnlock();
}

void SharedPublishCrash(PEXCEPTION_POINTERS pointers) {
    if (!shared_detail::g_state || !pointers || !pointers->ExceptionRecord ||
        !pointers->ContextRecord) return;
    if (!shared_detail::TrySharedLock(1024)) return;
    CortexDiagSharedCrash& crash = shared_detail::g_state->crash;
    crash.thread_id = GetCurrentThreadId();
    crash.exception_code = pointers->ExceptionRecord->ExceptionCode;
    crash.exception_address = reinterpret_cast<uint64_t>(pointers->ExceptionRecord->ExceptionAddress);
    crash.accessed_address = 0;
    crash.access_type = CORTEX_DIAG_ACCESS_UNKNOWN;
    if ((crash.exception_code == EXCEPTION_ACCESS_VIOLATION ||
         crash.exception_code == EXCEPTION_IN_PAGE_ERROR) &&
        pointers->ExceptionRecord->NumberParameters >= 2) {
        crash.accessed_address = static_cast<uint64_t>(pointers->ExceptionRecord->ExceptionInformation[1]);
        switch (pointers->ExceptionRecord->ExceptionInformation[0]) {
            case 0: crash.access_type = CORTEX_DIAG_ACCESS_READ; break;
            case 1: crash.access_type = CORTEX_DIAG_ACCESS_WRITE; break;
            case 8: crash.access_type = CORTEX_DIAG_ACCESS_EXECUTE; break;
            default: break;
        }
    }
    crash.context = *pointers->ContextRecord;
    MemoryBarrier();
    InterlockedIncrement(&crash.sequence);
    shared_detail::SharedUnlock();
    if (shared_detail::g_event) SetEvent(shared_detail::g_event);
}

#ifdef CORTEX_DIAGNOSTICS_TESTING
namespace testing {
const CortexDiagSharedState* SharedStateForTesting() { return shared_detail::g_state; }
void ResetSharedChannel() { SharedChannelShutdown(); }
} // namespace testing
#endif

} // namespace diagnostics

extern "C" __declspec(dllexport) void CortexDiagHeartbeat(const char* source) {
    diagnostics::SharedHeartbeat(source);
}
