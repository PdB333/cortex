#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "../debugger/debugger.h"

namespace watch {

struct WatchInfo {
    int id;
    uintptr_t address;
    std::string type;
    std::string label;
};

struct ChangeEvent {
    int watch_id;
    uintptr_t address;
    std::string label;
    std::string old_value;
    std::string new_value;
    long long timestamp_ms;
};

// Starts the background polling thread. Idempotent; call once during DLL
// init. Unlike freeze, watches are not persisted across sessions -- they're
// a short-lived debugging aid, not a standing cheat.
void Init();
void Shutdown();

// Registers `address` (interpreted as `type`, one of the numeric
// memscan/memory types: i8/u8/.../float/double) to be polled roughly every
// kIntervalMs. A change in value pushes a ChangeEvent into a bounded queue,
// so the AI can ask "did anything change?" via DrainEvents() instead of
// re-reading the address itself every few seconds and diffing client-side.
int Add(uintptr_t address, const std::string& type, const std::string& label);
bool Remove(int id);
std::vector<WatchInfo> List();

// Returns and clears all change events accumulated since the last call.
std::vector<ChangeEvent> DrainEvents();

struct AllocEvent {
    long long timestamp_ms;
    std::string api; // "VirtualAlloc" | "HeapAlloc"
    uintptr_t address; // 0 if the call failed (no event pushed in that case)
    size_t size;
    uint32_t protect_or_flags; // VirtualAlloc's flProtect, or HeapAlloc's dwFlags
};

// Hooks VirtualAlloc/HeapAlloc (lazily, on first call with enabled=true) via
// MinHook so every allocation >= minSize made by the host process while
// enabled is recorded. Disabled by default -- these APIs are called
// extremely frequently by any CRT/game allocator, so tracking is gated
// behind an atomic flag checked before taking the event-queue lock, keeping
// the steady-state cost of an installed-but-disabled hook to one relaxed
// atomic load per call. Must be called after MH_Initialize(). Returns false
// only if the lazy hook install itself fails (module/proc lookup or MinHook
// error); toggling `enabled`/`minSize` on an already-installed hook always
// succeeds.
bool SetAllocationWatch(bool enabled, size_t minSize);
bool AllocationWatchEnabled();
size_t AllocationWatchMinSize();

// Returns and clears all allocation events accumulated since the last call.
std::vector<AllocEvent> DrainAllocEvents();
// Returns a non-destructive snapshot for desktop/observability consumers.
std::vector<AllocEvent> SnapshotAllocEvents();

struct PageWatchInfo {
    int id;
    uintptr_t address;
    size_t size;
    std::string label;
};

struct PageAccessEvent {
    long long timestamp_ms;
    int watch_id;
    uintptr_t address; // exact faulting address (not just the page base)
    std::string access; // "read" | "write" | "execute"
    std::string label;
    uint32_t thread_id;
    uintptr_t instruction;
    size_t access_size;
    dbg::Registers registers;
    std::vector<uintptr_t> stack;
    std::vector<uint8_t> before;
    std::vector<uint8_t> after;
};

// Registers a memory-access breakpoint on [address, address+size) using the
// PAGE_GUARD memory-breakpoint technique (the same one x64dbg/Cheat Engine
// use): the covered pages are marked guarded via VirtualProtect, and a
// second, independent AddVectoredExceptionHandler callback (chained after
// debugger.cpp's own INT3/hardware-breakpoint handler, not modifying it)
// watches for EXCEPTION_GUARD_PAGE. On a hit it records the access, re-arms
// just that page's guard bit (the guard bit is single-shot -- Windows clears
// it after the first fault), and resumes. Faults on regions this handler
// doesn't own are passed through via EXCEPTION_CONTINUE_SEARCH untouched, so
// unrelated guard-page uses elsewhere in the process (notably Windows' own
// stack-growth guard page) are unaffected. Overlapping watches that share a
// physical page are not fully supported (the raw PAGE_GUARD bit on a shared
// page reflects whichever watch touched it last) -- acceptable for a
// debugging aid, not a correctness-critical feature. Returns -1 if any
// covered page isn't committed/queryable.
int AddPageWatch(uintptr_t address, size_t size, const std::string& label);
bool RemovePageWatch(int id);
std::vector<PageWatchInfo> ListPageWatches();

// Returns and clears all page-access events accumulated since the last call.
std::vector<PageAccessEvent> DrainPageAccessEvents();
// Returns a non-destructive snapshot without stealing events from MCP clients.
std::vector<PageAccessEvent> SnapshotPageAccessEvents();

} // namespace watch
