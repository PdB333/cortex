#pragma once

#include "model.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cortex::target {

enum class MemoryRegionType : uint8_t {
    Unknown = 0,
    Private,
    Image,
    Mapped
};

struct MemoryRegion {
    uint64_t base = 0;
    uint64_t size = 0;
    bool readable = false;
    bool writable = false;
    bool executable = false;
    bool copyOnWrite = false;
    MemoryRegionType type = MemoryRegionType::Unknown;
    // Native protection flags (PAGE_* on Windows), 0 when not reported.
    uint32_t protection = 0;
};

class Session {
public:
    virtual ~Session() = default;

    virtual const TargetDescriptor& Target() const = 0;
    virtual const CapabilitySet& Capabilities() const = 0;
    virtual bool Alive() const = 0;

    virtual bool ReadMemory(uint64_t address, void* buffer, size_t size, size_t* bytesRead = nullptr) const = 0;
    virtual bool WriteMemory(uint64_t address, const void* buffer, size_t size, size_t* bytesWritten = nullptr) = 0;
    virtual std::vector<MemoryRegion> MemoryRegions() const = 0;
};

using SessionPtr = std::shared_ptr<Session>;

} // namespace cortex::target
