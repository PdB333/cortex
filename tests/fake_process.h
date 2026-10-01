#pragma once

// In-memory stand-in for a target process, shared by the scanner and memory
// tool tests. Regions hold real bytes; pages can be marked unreadable.

#include "target/session.h"

#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <vector>

namespace cortex::tests {

using target::Capability;
using target::MemoryRegion;
using target::MemoryRegionType;

class FakeProcess final : public cortex::target::Session {
public:
    struct Region {
        MemoryRegion info;
        std::vector<uint8_t> bytes;
        std::vector<bool> unreadablePages;
    };

    FakeProcess() {
        target_.id = "fake";
        target_.name = "fake.exe";
        target_.capabilities.Add(Capability::MemoryRead).Add(Capability::MemoryScan).Add(Capability::MemoryWrite);
    }

    Region& Add(uint64_t base, uint64_t size, bool writable = true, bool executable = false,
                MemoryRegionType type = MemoryRegionType::Private, bool copyOnWrite = false) {
        Region region;
        region.info.base = base;
        region.info.size = size;
        region.info.readable = true;
        region.info.writable = writable;
        region.info.executable = executable;
        region.info.copyOnWrite = copyOnWrite;
        region.info.type = type;
        region.bytes.assign(static_cast<size_t>(size), 0);
        region.unreadablePages.assign(static_cast<size_t>((size + 4095) / 4096), false);
        regions_.push_back(std::move(region));
        return regions_.back();
    }

    template <typename T>
    void Put(uint64_t address, T value) { Write(address, &value, sizeof(T)); }

    void Write(uint64_t address, const void* data, size_t size) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& region : regions_) {
            if (address >= region.info.base && address + size <= region.info.base + region.info.size) {
                std::memcpy(region.bytes.data() + (address - region.info.base), data, size);
                return;
            }
        }
        std::cerr << "fake write outside memory" << std::endl;
        ++writeFailures;
    }

    const cortex::target::TargetDescriptor& Target() const override { return target_; }
    const cortex::target::CapabilitySet& Capabilities() const override { return target_.capabilities; }
    bool Alive() const override { return true; }

    bool ReadMemory(uint64_t address, void* buffer, size_t size, size_t* bytesRead) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (bytesRead) *bytesRead = 0;
        for (const auto& region : regions_) {
            const uint64_t end = region.info.base + region.info.size;
            if (address < region.info.base || address + size > end) continue;
            const uint64_t offset = address - region.info.base;
            for (uint64_t page = offset / 4096; page <= (offset + size - 1) / 4096; ++page)
                if (region.unreadablePages[static_cast<size_t>(page)]) return false;
            std::memcpy(buffer, region.bytes.data() + offset, size);
            if (bytesRead) *bytesRead = size;
            return true;
        }
        return false;
    }

    bool WriteMemory(uint64_t address, const void* buffer, size_t size, size_t* written) override {
        Write(address, buffer, size);
        if (written) *written = size;
        return true;
    }

    std::vector<MemoryRegion> MemoryRegions() const override {
        std::vector<MemoryRegion> result;
        for (const auto& region : regions_) result.push_back(region.info);
        return result;
    }

public:
    int writeFailures = 0;

private:
    cortex::target::TargetDescriptor target_;
    std::vector<Region> regions_;
    mutable std::mutex mutex_;
};

} // namespace cortex::tests
