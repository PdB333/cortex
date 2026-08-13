#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace memory {

constexpr size_t kMaxSingleAccessBytes = 64u * 1024u * 1024u;

inline bool IsValidRange(uintptr_t address, size_t size,
                         size_t maxSize = kMaxSingleAccessBytes) {
    if (address == 0 || size == 0 || size > maxSize) return false;
    const uintptr_t maxAddress = (std::numeric_limits<uintptr_t>::max)();
    return size - 1 <= maxAddress - address;
}

} // namespace memory
