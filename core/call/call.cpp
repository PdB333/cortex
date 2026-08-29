#include "call.h"

#include <windows.h>
#include <csetjmp>
#include <cstdio>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>

namespace remotecall {
namespace {

thread_local jmp_buf g_jmpBuf;
thread_local bool g_inCall = false;
thread_local DWORD g_lastExceptionCode = 0;

PVOID g_vehHandle = nullptr;
std::atomic<bool> g_shuttingDown{false};
std::atomic<DWORD> g_gameThreadId{0};
std::atomic<ULONGLONG> g_lastGameThreadPumpMs{0};
std::atomic<uint32_t> g_activeCalls{0};

struct PendingCall {
    uintptr_t address = 0;
    std::vector<uintptr_t> args;
    Convention convention = Convention::Cdecl;
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    bool cancelled = false;
    CallResult result;
};

std::mutex g_frameMutex;
std::deque<std::shared_ptr<PendingCall>> g_frameQueue;
constexpr size_t kMaxFrameQueue = 64;
constexpr size_t kMaxCallsPerFrame = 8;

std::mutex g_threadHookMutex;
std::map<DWORD, std::shared_ptr<PendingCall>> g_threadHookRequests;

LONG CALLBACK CallVEH(PEXCEPTION_POINTERS info) {
    if (g_inCall) {
        g_lastExceptionCode = info->ExceptionRecord->ExceptionCode;
        longjmp(g_jmpBuf, 1);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

bool IsExecutableAddress(uintptr_t address) {
    if (!address) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const DWORD p = mbi.Protect & 0xff;
    return p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ ||
           p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}

#define CORTEX_DEFINE_DISPATCH(CONV, SUFFIX)                                                                     \
    typedef uintptr_t(CONV *Fn0_##SUFFIX)();                                                                     \
    typedef uintptr_t(CONV *Fn1_##SUFFIX)(uintptr_t);                                                            \
    typedef uintptr_t(CONV *Fn2_##SUFFIX)(uintptr_t, uintptr_t);                                                 \
    typedef uintptr_t(CONV *Fn3_##SUFFIX)(uintptr_t, uintptr_t, uintptr_t);                                      \
    typedef uintptr_t(CONV *Fn4_##SUFFIX)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);                           \
    typedef uintptr_t(CONV *Fn5_##SUFFIX)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);                \
    typedef uintptr_t(CONV *Fn6_##SUFFIX)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);     \
    typedef uintptr_t(CONV *Fn7_##SUFFIX)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t,      \
                                           uintptr_t);                                                           \
    typedef uintptr_t(CONV *Fn8_##SUFFIX)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t,      \
                                           uintptr_t, uintptr_t);                                                \
    uintptr_t Dispatch_##SUFFIX(void* addr, const std::vector<uintptr_t>& a) {                                  \
        switch (a.size()) {                                                                                      \
            case 0: return reinterpret_cast<Fn0_##SUFFIX>(addr)();                                               \
            case 1: return reinterpret_cast<Fn1_##SUFFIX>(addr)(a[0]);                                           \
            case 2: return reinterpret_cast<Fn2_##SUFFIX>(addr)(a[0], a[1]);                                     \
            case 3: return reinterpret_cast<Fn3_##SUFFIX>(addr)(a[0], a[1], a[2]);                               \
            case 4: return reinterpret_cast<Fn4_##SUFFIX>(addr)(a[0], a[1], a[2], a[3]);                         \
            case 5: return reinterpret_cast<Fn5_##SUFFIX>(addr)(a[0], a[1], a[2], a[3], a[4]);                   \
            case 6: return reinterpret_cast<Fn6_##SUFFIX>(addr)(a[0], a[1], a[2], a[3], a[4], a[5]);             \
            case 7: return reinterpret_cast<Fn7_##SUFFIX>(addr)(a[0], a[1], a[2], a[3], a[4], a[5], a[6]);       \
            case 8: return reinterpret_cast<Fn8_##SUFFIX>(addr)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]); \
            default: return 0;                                                                                   \
        }                                                                                                        \
    }

#ifdef _WIN64
// Windows x64 has one native calling convention.  Avoid declaring x86-only
// thiscall/fastcall function-pointer types because MinGW rejects/ignores them
// inconsistently depending on architecture/toolchain.
CORTEX_DEFINE_DISPATCH(, Native)
#else
CORTEX_DEFINE_DISPATCH(__cdecl, Cdecl)
CORTEX_DEFINE_DISPATCH(__stdcall, Stdcall)
CORTEX_DEFINE_DISPATCH(__thiscall, Thiscall)
CORTEX_DEFINE_DISPATCH(__fastcall, Fastcall)
#endif

#undef CORTEX_DEFINE_DISPATCH

void Finish(const std::shared_ptr<PendingCall>& request, CallResult result) {
    std::lock_guard<std::mutex> lock(request->mutex);
    request->result = std::move(result);
    request->done = true;
    request->cv.notify_all();
}

CallResult WaitFor(const std::shared_ptr<PendingCall>& request,
                   uint32_t timeoutMs,
                   const char* timeoutError) {
    if (timeoutMs == 0) timeoutMs = 1;
    std::unique_lock<std::mutex> lock(request->mutex);
    if (!request->cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return request->done; })) {
        request->cancelled = true;
        CallResult timeout;
        timeout.error = timeoutError;
        timeout.threadId = 0;
        timeout.durationMs = timeoutMs;
        return timeout;
    }
    return request->result;
}

LRESULT CALLBACK ThreadDispatchHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code >= 0 && !g_shuttingDown.load(std::memory_order_acquire)) {
        const DWORD tid = GetCurrentThreadId();
        std::shared_ptr<PendingCall> request;
        {
            std::lock_guard<std::mutex> lock(g_threadHookMutex);
            auto it = g_threadHookRequests.find(tid);
            if (it != g_threadHookRequests.end()) {
                request = it->second;
                g_threadHookRequests.erase(it);
            }
        }
        if (request) {
            bool cancelled = false;
            {
                std::lock_guard<std::mutex> lock(request->mutex);
                cancelled = request->cancelled;
            }
            if (!cancelled) Finish(request, Invoke(request->address, request->args, request->convention));
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

HMODULE OwnModule() {
    HMODULE module = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&ThreadDispatchHook), &module);
    return module;
}

} // namespace

void Init() {
    if (g_vehHandle) return;
    g_shuttingDown.store(false, std::memory_order_release);
    g_vehHandle = AddVectoredExceptionHandler(1, CallVEH);
}

bool Shutdown() {
    g_shuttingDown.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(g_frameMutex);
        while (!g_frameQueue.empty()) {
            auto request = std::move(g_frameQueue.front());
            g_frameQueue.pop_front();
            CallResult stopped;
            stopped.error = "call_scheduler_shutdown";
            Finish(request, std::move(stopped));
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_threadHookMutex);
        for (auto& [tid, request] : g_threadHookRequests) {
            CallResult stopped;
            stopped.error = "call_scheduler_shutdown";
            Finish(request, std::move(stopped));
        }
        g_threadHookRequests.clear();
    }

    const ULONGLONG deadline = GetTickCount64() + 2000;
    while (g_activeCalls.load(std::memory_order_acquire) != 0 && GetTickCount64() < deadline) Sleep(1);
    if (g_activeCalls.load(std::memory_order_acquire) != 0) return false;

    if (g_vehHandle) {
        RemoveVectoredExceptionHandler(g_vehHandle);
        g_vehHandle = nullptr;
    }
    g_gameThreadId.store(0, std::memory_order_release);
    g_lastGameThreadPumpMs.store(0, std::memory_order_release);
    return true;
}

CallResult Invoke(uintptr_t address, const std::vector<uintptr_t>& args, Convention conv) {
    CallResult result;
    result.threadId = GetCurrentThreadId();
    const ULONGLONG started = GetTickCount64();
    if (g_shuttingDown.load(std::memory_order_acquire)) {
        result.error = "call_scheduler_shutting_down";
        return result;
    }
    if (args.size() > 8) {
        result.error = "too_many_args_max_8";
        return result;
    }
    if (!IsExecutableAddress(address)) {
        result.error = "address_not_executable";
        return result;
    }
    g_activeCalls.fetch_add(1, std::memory_order_acq_rel);
void* addr = reinterpret_cast<void*>(address);
    g_lastExceptionCode = 0;
    g_inCall = true;
    if (setjmp(g_jmpBuf) == 0) {
        uintptr_t ret = 0;
#ifdef _WIN64
        (void)conv;
        ret = Dispatch_Native(addr, args);
#else
        switch (conv) {
            case Convention::Stdcall: ret = Dispatch_Stdcall(addr, args); break;
            case Convention::Thiscall: ret = Dispatch_Thiscall(addr, args); break;
            case Convention::Fastcall: ret = Dispatch_Fastcall(addr, args); break;
            default: ret = Dispatch_Cdecl(addr, args); break;
        }
#endif
        g_inCall = false;
        result.ok = true;
        result.returnValue = static_cast<uint64_t>(ret);
    } else {
        g_inCall = false;
        result.exceptionCode = g_lastExceptionCode;
        char buf[64];
        snprintf(buf, sizeof(buf), "exception_0x%08lX_during_call",
                 static_cast<unsigned long>(g_lastExceptionCode));
        result.error = buf;
    }
    result.durationMs = GetTickCount64() - started;
    g_activeCalls.fetch_sub(1, std::memory_order_acq_rel);
    return result;
}

void PumpGameThread() {
    if (g_shuttingDown.load(std::memory_order_acquire)) return;
    const DWORD tid = GetCurrentThreadId();
    g_gameThreadId.store(tid, std::memory_order_release);
    g_lastGameThreadPumpMs.store(GetTickCount64(), std::memory_order_release);

    for (size_t i = 0; i < kMaxCallsPerFrame; ++i) {
        std::shared_ptr<PendingCall> request;
        {
            std::lock_guard<std::mutex> lock(g_frameMutex);
            if (g_frameQueue.empty()) break;
            request = std::move(g_frameQueue.front());
            g_frameQueue.pop_front();
        }
        bool cancelled = false;
        {
            std::lock_guard<std::mutex> lock(request->mutex);
            cancelled = request->cancelled;
        }
        if (!cancelled) Finish(request, Invoke(request->address, request->args, request->convention));
    }
}

CallResult InvokeOnGameThread(uintptr_t address,
                              const std::vector<uintptr_t>& args,
                              Convention conv,
                              uint32_t timeoutMs) {
    if (g_shuttingDown.load(std::memory_order_acquire)) return {false, 0, "call_scheduler_shutting_down"};
    const DWORD observed = g_gameThreadId.load(std::memory_order_acquire);
    if (observed != 0 && observed == GetCurrentThreadId()) return Invoke(address, args, conv);

    auto request = std::make_shared<PendingCall>();
    request->address = address;
    request->args = args;
    request->convention = conv;
    {
        std::lock_guard<std::mutex> lock(g_frameMutex);
        if (g_frameQueue.size() >= kMaxFrameQueue) return {false, 0, "game_thread_queue_full"};
        g_frameQueue.push_back(request);
    }
    return WaitFor(request, timeoutMs, observed == 0 ? "game_thread_not_observed_before_timeout" : "game_thread_call_timeout");
}

CallResult InvokeOnThread(uint32_t threadId,
                          uintptr_t address,
                          const std::vector<uintptr_t>& args,
                          Convention conv,
                          uint32_t timeoutMs) {
    if (!threadId) return {false, 0, "thread_id_required"};
    if (threadId == GetCurrentThreadId()) return Invoke(address, args, conv);
    if (threadId == g_gameThreadId.load(std::memory_order_acquire))
        return InvokeOnGameThread(address, args, conv, timeoutMs);
    if (g_shuttingDown.load(std::memory_order_acquire)) return {false, 0, "call_scheduler_shutting_down"};

    HANDLE thread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, threadId);
    if (!thread) return {false, 0, "thread_not_found_or_not_accessible"};
    DWORD exitCode = 0;
    const bool live = GetExitCodeThread(thread, &exitCode) && exitCode == STILL_ACTIVE;
    CloseHandle(thread);
    if (!live) return {false, 0, "thread_not_running"};

    auto request = std::make_shared<PendingCall>();
    request->address = address;
    request->args = args;
    request->convention = conv;
    {
        std::lock_guard<std::mutex> lock(g_threadHookMutex);
        if (g_threadHookRequests.count(threadId)) return {false, 0, "thread_call_already_pending"};
        g_threadHookRequests[threadId] = request;
    }

    HHOOK hook = SetWindowsHookExA(WH_GETMESSAGE, ThreadDispatchHook, OwnModule(), threadId);
    if (!hook) {
        std::lock_guard<std::mutex> lock(g_threadHookMutex);
        g_threadHookRequests.erase(threadId);
        return {false, 0, "thread_message_hook_install_failed"};
    }
    if (!PostThreadMessageA(threadId, WM_NULL, 0, 0)) {
        UnhookWindowsHookEx(hook);
        std::lock_guard<std::mutex> lock(g_threadHookMutex);
        g_threadHookRequests.erase(threadId);
        return {false, 0, "thread_has_no_message_queue"};
    }

    CallResult result = WaitFor(request, timeoutMs, "thread_call_timeout");
    UnhookWindowsHookEx(hook);
    {
        std::lock_guard<std::mutex> lock(g_threadHookMutex);
        auto it = g_threadHookRequests.find(threadId);
        if (it != g_threadHookRequests.end() && it->second == request) g_threadHookRequests.erase(it);
    }
    return result;
}

uint32_t GameThreadId() { return g_gameThreadId.load(std::memory_order_acquire); }
uint64_t LastGameThreadPumpMs() { return g_lastGameThreadPumpMs.load(std::memory_order_acquire); }
size_t PendingGameThreadCalls() {
    std::lock_guard<std::mutex> lock(g_frameMutex);
    return g_frameQueue.size();
}

} // namespace remotecall


