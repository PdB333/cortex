#include "watch.h"
#include "../log.h"

#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <mutex>
#include <deque>
#include <chrono>

namespace watch {

namespace {

constexpr size_t kMaxAllocEvents = 1000;

std::atomic<bool> g_enabled{false};
std::atomic<size_t> g_minSize{0};
std::atomic<bool> g_installed{false};
std::mutex g_installMutex;

std::mutex g_eventsMutex;
std::deque<AllocEvent> g_events;
thread_local bool g_insideAllocWatch = false;

long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void PushEvent(const char* api, uintptr_t address, size_t size, uint32_t protectOrFlags) {
    if (size < g_minSize.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(g_eventsMutex);
    if (g_events.size() >= kMaxAllocEvents) g_events.pop_front();
    g_events.push_back({NowMs(), api, address, size, protectOrFlags});
}

typedef LPVOID(WINAPI* VirtualAlloc_t)(LPVOID, SIZE_T, DWORD, DWORD);
typedef LPVOID(WINAPI* HeapAlloc_t)(HANDLE, DWORD, SIZE_T);

VirtualAlloc_t oVirtualAlloc = nullptr;
HeapAlloc_t oHeapAlloc = nullptr;

LPVOID WINAPI hkVirtualAlloc(LPVOID lpAddress, SIZE_T dwSize, DWORD flAllocationType, DWORD flProtect) {
    LPVOID result = oVirtualAlloc(lpAddress, dwSize, flAllocationType, flProtect);
    if (result && g_enabled.load(std::memory_order_relaxed) && !g_insideAllocWatch) {
        g_insideAllocWatch = true;
        PushEvent("VirtualAlloc", reinterpret_cast<uintptr_t>(result), static_cast<size_t>(dwSize), flProtect);
        g_insideAllocWatch = false;
    }
    return result;
}

LPVOID WINAPI hkHeapAlloc(HANDLE hHeap, DWORD dwFlags, SIZE_T dwBytes) {
    LPVOID result = oHeapAlloc(hHeap, dwFlags, dwBytes);
    if (result && g_enabled.load(std::memory_order_relaxed) && !g_insideAllocWatch) {
        g_insideAllocWatch = true;
        PushEvent("HeapAlloc", reinterpret_cast<uintptr_t>(result), static_cast<size_t>(dwBytes), dwFlags);
        g_insideAllocWatch = false;
    }
    return result;
}

bool InstallHooks() {
    HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
    if (!kernel32) return false;

    void* virtualAllocAddr = reinterpret_cast<void*>(GetProcAddress(kernel32, "VirtualAlloc"));
    void* heapAllocAddr = reinterpret_cast<void*>(GetProcAddress(kernel32, "HeapAlloc"));
    if (!virtualAllocAddr || !heapAllocAddr) {
        dbglog::Line("alloc_watch: GetProcAddress failed");
        return false;
    }

    MH_STATUS c1 = MH_CreateHook(virtualAllocAddr, reinterpret_cast<void*>(&hkVirtualAlloc),
                                  reinterpret_cast<void**>(&oVirtualAlloc));
    MH_STATUS c2 =
        MH_CreateHook(heapAllocAddr, reinterpret_cast<void*>(&hkHeapAlloc), reinterpret_cast<void**>(&oHeapAlloc));
    dbglog::Line("alloc_watch: create VirtualAlloc=%d HeapAlloc=%d", (int)c1, (int)c2);
    if (c1 != MH_OK || c2 != MH_OK) return false;

    MH_STATUS e1 = MH_EnableHook(virtualAllocAddr);
    MH_STATUS e2 = MH_EnableHook(heapAllocAddr);
    dbglog::Line("alloc_watch: enable VirtualAlloc=%d HeapAlloc=%d", (int)e1, (int)e2);
    return e1 == MH_OK && e2 == MH_OK;
}

bool EnsureInstalled() {
    if (g_installed.load(std::memory_order_relaxed)) return true;
    std::lock_guard<std::mutex> lock(g_installMutex);
    if (g_installed.load(std::memory_order_relaxed)) return true;
    if (!InstallHooks()) return false;
    g_installed.store(true, std::memory_order_relaxed);
    return true;
}

} // namespace

bool SetAllocationWatch(bool enabled, size_t minSize) {
    g_minSize.store(minSize, std::memory_order_relaxed);
    if (enabled && !EnsureInstalled()) return false;
    g_enabled.store(enabled, std::memory_order_relaxed);
    return true;
}

bool AllocationWatchEnabled() {
    return g_enabled.load(std::memory_order_relaxed);
}

size_t AllocationWatchMinSize() {
    return g_minSize.load(std::memory_order_relaxed);
}

std::vector<AllocEvent> SnapshotAllocEvents() {
    const bool previousInside = g_insideAllocWatch;
    g_insideAllocWatch = true;
    std::lock_guard<std::mutex> lock(g_eventsMutex);
    std::vector<AllocEvent> out(g_events.begin(), g_events.end());
    g_insideAllocWatch = previousInside;
    return out;
}
std::vector<AllocEvent> DrainAllocEvents() {
    g_insideAllocWatch = true;
    std::lock_guard<std::mutex> lock(g_eventsMutex);
    std::vector<AllocEvent> out(g_events.begin(), g_events.end());
    g_events.clear();
    g_insideAllocWatch = false;
    return out;
}

} // namespace watch
