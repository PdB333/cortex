#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cortex::services {

// A run of consecutive bytes that differ between two captures of the same
// range, as offsets into that range.
struct ByteRun {
    uint64_t offset = 0;
    uint64_t length = 0;
};

// Compares two captures of one range. `beforeValid` / `afterValid` hold one
// flag per `page` bytes (empty = every page readable); a byte on a page
// unreadable in either capture is never reported as changed, since nothing
// is known about it. At most `maxRuns` runs are returned; `truncated` tells
// whether more existed.
inline std::vector<ByteRun> DiffRuns(const std::vector<uint8_t>& before, const std::vector<uint8_t>& after,
                                     const std::vector<uint8_t>& beforeValid, const std::vector<uint8_t>& afterValid,
                                     size_t page, size_t maxRuns, bool* truncated = nullptr) {
    std::vector<ByteRun> runs;
    if (truncated) *truncated = false;
    const size_t size = before.size() < after.size() ? before.size() : after.size();
    auto readable = [&](size_t index) {
        if (page == 0) return true;
        const size_t p = index / page;
        return (beforeValid.empty() || (p < beforeValid.size() && beforeValid[p])) &&
               (afterValid.empty() || (p < afterValid.size() && afterValid[p]));
    };
    for (size_t i = 0; i < size; ++i) {
        if (before[i] == after[i] || !readable(i)) continue;
        if (!runs.empty() && runs.back().offset + runs.back().length == i) {
            ++runs.back().length;
            continue;
        }
        if (runs.size() == maxRuns) {
            if (truncated) *truncated = true;
            break;
        }
        runs.push_back({i, 1});
    }
    return runs;
}

// Index of the first run starting after `offset`, wrapping to the first
// run; runs.size() when there is none.
inline size_t NextRun(const std::vector<ByteRun>& runs, uint64_t offset) {
    if (runs.empty()) return 0;
    for (size_t i = 0; i < runs.size(); ++i)
        if (runs[i].offset > offset) return i;
    return 0;
}

// Bytes covered by the runs.
inline uint64_t ChangedBytes(const std::vector<ByteRun>& runs) {
    uint64_t total = 0;
    for (const auto& run : runs) total += run.length;
    return total;
}

} // namespace cortex::services
