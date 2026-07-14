#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <optional>

namespace memory {

// Reads `size` bytes at `address` into `out`. Uses ReadProcessMemory on the
// current process's pseudo-handle, which returns FALSE on an invalid access
// instead of raising a structured exception -- safe to call on arbitrary
// AI-supplied addresses without crashing the host game.
bool ReadBytes(uintptr_t address, size_t size, std::vector<uint8_t>& out);

// Writes `data` to `address`. Returns false (without crashing) if the address
// is not writable.
bool WriteBytes(uintptr_t address, const std::vector<uint8_t>& data);

// Reads a bounded, possibly-not-null-terminated C string (stops at the first
// NUL or after max_len bytes, whichever comes first).
std::optional<std::string> ReadString(uintptr_t address, size_t max_len);

} // namespace memory
