#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace provenance {

struct Range {
    uint64_t id;
    uintptr_t base;
    size_t size;
    std::string owner;
    std::string label;
    bool transient;
};

// Registers memory owned by Cortex. Scanners exclude these exact byte ranges
// by default so their result buffers, HTTP scratch space, and code caves can
// never become candidates in a later scan pass.
uint64_t Register(uintptr_t base, size_t size, const std::string& owner,
                  const std::string& label, bool transient = false);
bool Unregister(uint64_t id);
bool Resize(uint64_t id, uintptr_t base, size_t size);
bool Contains(uintptr_t address, size_t size = 1);
std::vector<Range> List();

// Lazily records the injected Cortex module image itself.
void EnsureCoreModuleRegistered();

class ScopedRange {
public:
    ScopedRange() = default;
    ScopedRange(uintptr_t base, size_t size, const std::string& owner, const std::string& label);
    ~ScopedRange();
    ScopedRange(const ScopedRange&) = delete;
    ScopedRange& operator=(const ScopedRange&) = delete;
    ScopedRange(ScopedRange&& other) noexcept;
    ScopedRange& operator=(ScopedRange&& other) noexcept;
    void Reset(uintptr_t base = 0, size_t size = 0);

private:
    uint64_t id_ = 0;
};

} // namespace provenance
