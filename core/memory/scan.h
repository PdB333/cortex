#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <variant>

namespace memscan {

enum class Filter {
    Exact,
    Changed,
    Unchanged,
    Increased,
    Decreased,
    IncreasedBy,
    DecreasedBy,
    GreaterThan,
    LessThan,
    Between,
};

std::optional<Filter> ParseFilter(const std::string& s);

using ScanScalar = std::variant<int64_t, uint64_t, double>;

struct ScanResult {
    uintptr_t address;
    ScanScalar value;
    std::string type; // only set when the owning session's type is "all"
};

struct SessionInfo {
    int id;
    std::string type;
    size_t count;
};

struct ScanOptions {
    // Region selection, ANDed together when more than one is set (matches
    // Cheat Engine's "Writable"/"Executable"/"Copy on write" checkboxes,
    // which narrow the scan when checked together rather than union). When
    // none are set, every committed non-guarded readable page is eligible --
    // the broadest possible scan.
    bool writableOnly = true;
    bool executableOnly = false;
    bool copyOnWriteOnly = false;
    // Byte stride between checked offsets, in absolute address terms (same
    // anchoring as the existing typeSize-aligned walk). 0 means "use the
    // type's natural size" (current/default behavior).
    uint32_t alignment = 0;
    // Suspends every other thread in the process for the duration of the
    // scan so a fast-moving value (a counter incrementing every frame)
    // can't shift mid-scan and produce an inconsistent snapshot. The calling
    // (HTTP worker) thread is never suspended.
    bool pauseProcess = false;
    // Reject exact address ranges registered as Cortex-owned. Enabled by
    // default to prevent an injected scanner from discovering its own
    // buffers and returning self-referential false positives.
    bool excludeCortex = true;
};

// Starts a new scan session over i8/u8/i16/u16/i32/u32/i64/u64/float/double
// values (or "all" of them simultaneously, see below) across the process's
// memory. If `value` is unset, every candidate's current value is recorded
// as a baseline for a later ScanNext with Filter::Changed/Unchanged/
// Increased/Decreased -- mirrors Cheat Engine's "Unknown initial value"
// first scan. `rangeStart`/`rangeEnd`, if set, restrict the scan to that
// address range (useful to stay well clear of the candidate cap on a
// targeted "unknown value" scan). `type == "all"` scans every numeric type
// at every byte offset and requires `value` to be set (an "all types,
// unknown value" scan would multiply the already-huge unknown-value
// candidate set by 10 for near-zero benefit, so it's rejected outright).
// Returns the new session id, or -1 if `type` is not recognized, or if
// `type == "all"` without a `value`.
// Values are passed as text so i64/u64 scans keep their full 64-bit precision.
// JSON callers may still send ordinary numbers; routes_scan.cpp preserves the
// original token text before handing it here. Strings are recommended for
// integers outside JavaScript's exact range (+/-2^53).
int ScanNew(const std::string& type, std::optional<std::string> value,
            std::optional<uintptr_t> rangeStart, std::optional<uintptr_t> rangeEnd,
            size_t& outCount, bool& outTruncated, const ScanOptions& options = ScanOptions());

// Narrows an existing session's candidates against `filter`. `value` is used
// by Exact/IncreasedBy/DecreasedBy/GreaterThan/LessThan/Between (as the lower
// bound for Between); `value2` is used only by Between (the upper bound).
// `pauseProcess` mirrors ScanNew's option, applied for the re-read pass.
// Returns false if the session doesn't exist.
bool ScanNext(int sessionId, Filter filter, std::optional<std::string> value, std::optional<std::string> value2,
              size_t& outCount, bool pauseProcess = false);

// Returns up to `limit` (capped at 1000) of the session's current candidates
// starting at `offset`, with freshly re-read values. Returns false if the
// session doesn't exist.
bool ScanResults(int sessionId, size_t offset, size_t limit, std::vector<ScanResult>& out, size_t& outTotal);

bool ScanReset(int sessionId);

std::vector<SessionInfo> ScanList();

// Returns addresses present in the candidate set of every session id listed
// in `sessionIds` -- e.g. recoupling the surviving addresses of two
// independently-run scans (a common Cheat Engine trick: scan for one value,
// scan again with a completely different filter chain in a second session,
// then intersect). Returns false if fewer than 2 ids are given or any id
// doesn't exist.
bool ScanIntersect(const std::vector<int>& sessionIds, std::vector<uintptr_t>& outAddresses);

// AOB pattern scan. `pattern` is space-separated hex bytes with "??" as a
// wildcard byte (e.g. "48 8B ?? 05 90"). If `moduleName` is non-empty,
// restricts the search to that module's image; otherwise searches every
// loaded module. Returns matched start addresses (capped).
std::vector<uintptr_t> AobScan(const std::string& pattern, const std::string& moduleName, bool& outTruncated);

struct PointerHit {
    uintptr_t address; // where the pointer-sized value was found
    int64_t offset;    // target - value_at(address); 0 = points exactly at target
};

// Reverse pointer scan a la Cheat Engine's "find what points to this
// address": scans committed/writable memory for pointer-sized values V with
// target - maxOffset <= V <= target (V is typically the base of some struct/
// allocation, target a field a few bytes into it). Returns the address of
// each matching pointer *slot* (not what it points to), capped like AobScan.
std::vector<PointerHit> FindPointersTo(uintptr_t target, uint32_t maxOffset, bool& outTruncated);

struct PointerPathResult {
    std::string module;
    int64_t base_offset;
    std::vector<int64_t> offsets; // same shape/order as project::SetPointerPath's `offsets`
};

// Multi-level pointer scan a la Cheat Engine's "Pointer scan for this
// address": searches backward from `target` through chains of deref+offset,
// looking for a path rooted at a *static* address (inside a loaded module's
// image) -- unlike a raw heap/stack address, module+offset stays valid
// across game restarts, which is the actual point of this (a single-level
// FindPointersTo result is almost always a heap address that moves on the
// next launch). Bounded by `maxDepth` (levels of indirection, hard-capped)
// and `maxOffset` (how far before a value the actual struct field can sit,
// at each level) to keep the search tractable on a process with millions of
// candidate pointer slots. Returns at most a capped number of complete
// paths; each result's fields plug directly into
// project::SetPointerPath(name, module, base_offset, offsets, ...).
std::vector<PointerPathResult> PointerScan(uintptr_t target, int maxDepth, uint32_t maxOffset, bool& outTruncated);

enum class StringEncoding { Ascii, Utf16 };

struct StringHit {
    uintptr_t address;
    std::string value;
};

// Scans readable memory (not just writable -- literal strings usually live
// in a module's read-only .rdata) for printable runs of at least
// `minLength` characters, optionally restricted to `moduleName` and/or
// filtered to those containing `contains` (case-insensitive). `encoding`
// selects plain ASCII/Latin1 bytes vs UTF-16LE (2 bytes/char, high byte 0) --
// most game strings (item names, dialogue keys, file paths) are one or the
// other. Capped like the other scans.
std::vector<StringHit> StringScan(size_t minLength, const std::string& contains, const std::string& moduleName,
                                   StringEncoding encoding, bool& outTruncated);

struct CodeCave {
    uintptr_t address;
    size_t size;
};

// Finds runs of at least `minSize` consecutive padding bytes (0x00 or 0xCC,
// the two byte values compilers/linkers use to pad between functions/
// sections) inside committed executable regions -- a la Cheat Engine's "Scan
// for code caves". These runs are safe to overwrite with a detour/jump since
// they aren't reachable code. Restricted to `moduleName`'s image if given,
// otherwise every loaded module's executable regions. Capped like the other
// scans.
std::vector<CodeCave> FindCodeCaves(size_t minSize, const std::string& moduleName, bool& outTruncated);

struct MemoryRegionInfo {
    uintptr_t base;
    size_t size;
    std::string protect;    // e.g. "rwx", "r--", "rw-"
    std::string state;      // "commit" | "reserve" | "free"
    std::string type;       // "image" | "private" | "mapped"
    std::string moduleName; // set if `base` falls inside a loaded module's image, else empty
};

// Walks the full address space via VirtualQuery, a la Cheat Engine's
// "Browse memory region" / the region list backing its scan-options range
// picker. Returns every region regardless of protection (including MEM_FREE
// gaps) so a caller can see the full layout, not just what's scannable.
std::vector<MemoryRegionInfo> ListRegions();

} // namespace memscan
