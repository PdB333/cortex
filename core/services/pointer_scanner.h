#pragma once

#include "target/session.h"
#include "services/value_scanner.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace cortex::services {

// Pointer scanner in the style of Cheat Engine: finds chains that start at a
// static address (module+offset) and reach a target address through up to
// maxLevel dereferences, so a value found by a scan can be found again after
// the game restarts. Works from outside the process; nothing is injected.
//
// A path reads: a0 = module + baseOffset, then a(i+1) = [a(i)] + offsets[i];
// the last address is the target.

struct PointerModule {
    std::string name;
    uint64_t base = 0;
    uint64_t size = 0;
};

struct PointerPath {
    uint32_t module = 0;        // index into PointerScanResult::modules
    uint64_t baseOffset = 0;
    std::vector<uint32_t> offsets;
};

struct PointerScanOptions {
    uint64_t target = 0;
    int maxLevel = 5;
    uint32_t maxOffset = 0x1000;
    size_t maxResults = 100000;
    unsigned pointerSize = 8;
    bool alignedOnly = true;
    bool includeMapped = false;
    uint64_t maxVisits = 50000000;   // bounds the search on huge processes
    std::vector<PointerModule> modules;  // static bases
};

struct PointerScanResult {
    uint64_t target = 0;
    unsigned pointerSize = 8;
    std::vector<PointerModule> modules;
    std::vector<PointerPath> paths;
    uint64_t pointersIndexed = 0;
    bool truncated = false;
    double milliseconds = 0.0;
};

class PointerScanner {
public:
    static bool Scan(const target::SessionPtr& session, const PointerScanOptions& options,
                     PointerScanResult& result, std::string* error = nullptr,
                     const std::atomic_bool* cancelled = nullptr, ScanProgress* progress = nullptr);

    // Follows a path with the modules' current bases (matched by name).
    static bool Resolve(const target::SessionPtr& session, const PointerScanResult& result,
                        const PointerPath& path, const std::vector<PointerModule>& currentModules,
                        uint64_t& address);

    // Keeps the paths that now lead to newTarget (Cheat Engine's rescan).
    static size_t Rescan(const target::SessionPtr& session, PointerScanResult& result, uint64_t newTarget,
                         const std::vector<PointerModule>& currentModules);

    // "game.exe"+1A2B0 -> 10 -> 48
    static std::string Format(const PointerScanResult& result, const PointerPath& path);

    static bool Save(const std::string& file, const PointerScanResult& result, std::string* error = nullptr);
    static bool Load(const std::string& file, PointerScanResult& result, std::string* error = nullptr);
};

} // namespace cortex::services
