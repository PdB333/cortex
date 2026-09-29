// Unit tests for the speedhack hooks against a fake process: the timing
// functions are patched with a jump, the cave carries the multiplier and a
// working trampoline, and removing it puts everything back.

#include "services/speedhack.h"

#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

using cortex::services::InstallSpeedhack;
using cortex::services::RemoveSpeedhack;
using cortex::services::SpeedhackHost;
using cortex::services::SpeedhackState;
using cortex::services::UpdateSpeedhack;

int failures = 0;

#define CHECK(condition)                                                              \
    do {                                                                              \
        if (!(condition)) {                                                           \
            ++failures;                                                               \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #condition \
                      << std::endl;                                                   \
        }                                                                             \
    } while (false)

// x86 targets live below 4 GB, so the fake process is placed accordingly.
constexpr uint64_t kKernelBase64 = 0x7FF800000000ull;
constexpr uint64_t kKernelBase32 = 0x70000000ull;

// A plausible prologue: sub rsp,28 / call rel32 / add rsp,28 / ret.
const uint8_t kPrologue[] = {0x48, 0x83, 0xEC, 0x28, 0xE8, 0x10, 0x00, 0x00, 0x00,
                            0x48, 0x83, 0xC4, 0x28, 0xC3};

struct FakeTarget {
    std::map<uint64_t, std::vector<uint8_t>> blocks;
    uint64_t kernelBase = kKernelBase64;
    uint64_t nextAllocation = kKernelBase64 + 0x100000;
    std::vector<uint64_t> freed;

    uint64_t GetTickCount() const { return kernelBase + 0x1000; }
    uint64_t GetTickCount64() const { return kernelBase + 0x1100; }
    uint64_t QueryPerformanceCounter() const { return kernelBase + 0x1200; }

    explicit FakeTarget(bool x64 = true) {
        kernelBase = x64 ? kKernelBase64 : kKernelBase32;
        nextAllocation = kernelBase + 0x100000;
        std::vector<uint8_t> kernel(0x4000, 0xCC);
        for (uint64_t function : {GetTickCount(), GetTickCount64(), QueryPerformanceCounter()})
            std::memcpy(kernel.data() + (function - kernelBase), kPrologue, sizeof(kPrologue));
        blocks[kernelBase] = std::move(kernel);
    }

    std::vector<uint8_t>* Find(uint64_t address, size_t size, size_t& offset) {
        for (auto& block : blocks) {
            if (address < block.first || address + size > block.first + block.second.size()) continue;
            offset = static_cast<size_t>(address - block.first);
            return &block.second;
        }
        return nullptr;
    }

    SpeedhackHost Host() {
        SpeedhackHost host;
        host.read = [this](uint64_t address, void* buffer, size_t size) {
            size_t offset = 0;
            auto* block = Find(address, size, offset);
            if (!block) return false;
            std::memcpy(buffer, block->data() + offset, size);
            return true;
        };
        host.write = [this](uint64_t address, const void* buffer, size_t size) {
            size_t offset = 0;
            auto* block = Find(address, size, offset);
            if (!block) return false;
            std::memcpy(block->data() + offset, buffer, size);
            return true;
        };
        host.allocate = [this](size_t size, uint64_t, uint64_t& address, std::string&) {
            address = nextAllocation;
            nextAllocation += 0x10000;
            blocks[address] = std::vector<uint8_t>(size, 0);
            return true;
        };
        host.release = [this](uint64_t address, std::string&) {
            freed.push_back(address);
            blocks.erase(address);
            return true;
        };
        host.symbol = [this](const std::string& name, uint64_t& value) {
            if (name == "kernel32.GetTickCount") value = GetTickCount();
            else if (name == "kernel32.GetTickCount64") value = GetTickCount64();
            else if (name == "kernel32.QueryPerformanceCounter") value = QueryPerformanceCounter();
            else return false;  // winmm is not loaded
            return true;
        };
        return host;
    }

    uint8_t ReadU8(uint64_t address) {
        size_t offset = 0;
        auto* block = Find(address, 1, offset);
        return block ? (*block)[offset] : 0;
    }
    uint32_t ReadU32(uint64_t address) {
        uint32_t value = 0;
        size_t offset = 0;
        if (auto* block = Find(address, 4, offset)) std::memcpy(&value, block->data() + offset, 4);
        return value;
    }
    double ReadDouble(uint64_t address) {
        double value = 0;
        size_t offset = 0;
        if (auto* block = Find(address, 8, offset)) std::memcpy(&value, block->data() + offset, 8);
        return value;
    }
};

void TestInstallUpdateRemove() {
    FakeTarget target;
    auto host = target.Host();
    SpeedhackState state;
    std::string error;

    const bool ok = InstallSpeedhack(host, true, 2.5, state, &error);
    CHECK(ok);
    if (!ok) {
        std::cerr << "  install error: " << error << std::endl;
        return;
    }
    // The three kernel32 functions are hooked; winmm is not loaded.
    CHECK(state.hooks.size() == 3);
    CHECK(state.skipped.size() == 1 && state.skipped[0].find("timeGetTime") != std::string::npos);
    CHECK(state.active && state.multiplier == 2.5);

    for (const auto& hook : state.hooks) {
        // The function starts with a jmp into its cave.
        CHECK(target.ReadU8(hook.address) == 0xE9);
        const int32_t relative = static_cast<int32_t>(target.ReadU32(hook.address + 1));
        const uint64_t hookAddress = static_cast<uint64_t>(static_cast<int64_t>(hook.address + 5) + relative);
        CHECK(hookAddress > hook.cave && hookAddress < hook.cave + 512);
        // The replaced prologue is whole instructions (4 + 5 = 9 here).
        CHECK(hook.original.size() == 9);
        // The multiplier sits at the start of the cave.
        CHECK(target.ReadDouble(hook.cave) == 2.5);
        // The trampoline holds the relocated prologue: sub rsp,28 first.
        CHECK(target.ReadU8(hook.cave + 24) == 0x48);
        CHECK(target.ReadU8(hook.cave + 25) == 0x83);
    }

    CHECK(UpdateSpeedhack(host, state, 0.25, &error));
    for (const auto& hook : state.hooks) CHECK(target.ReadDouble(hook.cave) == 0.25);
    CHECK(state.multiplier == 0.25);

    const auto caves = state.hooks;
    CHECK(RemoveSpeedhack(host, state, &error));
    CHECK(!state.active && state.hooks.empty());
    CHECK(target.freed.size() == caves.size());
    for (const auto& hook : caves)
        for (size_t i = 0; i < hook.original.size(); ++i)
            CHECK(target.ReadU8(hook.address + i) == kPrologue[i]);
}

void TestX86SkipsTheCounter() {
    FakeTarget target(false);
    auto host = target.Host();
    SpeedhackState state;
    std::string error;
    if (!InstallSpeedhack(host, false, 2.0, state, &error)) {
        std::cerr << "  x86 install error: " << error << std::endl;
        for (const auto& skipped : state.skipped) std::cerr << "  skipped: " << skipped << std::endl;
    }
    CHECK(state.active);
    // Only the 32-bit tick function is hooked on x86.
    CHECK(state.hooks.size() == 1 && state.hooks[0].function == "kernel32.GetTickCount");
    bool notedCounter = false;
    for (const auto& skipped : state.skipped)
        notedCounter |= skipped.find("QueryPerformanceCounter") != std::string::npos;
    CHECK(notedCounter);
    CHECK(RemoveSpeedhack(host, state, &error));
}

void TestErrors() {
    FakeTarget target;
    auto host = target.Host();
    SpeedhackState state;
    std::string error;
    CHECK(!InstallSpeedhack(host, true, 0.0, state, &error));
    CHECK(error.find("greater than zero") != std::string::npos);

    SpeedhackHost empty = host;
    empty.symbol = [](const std::string&, uint64_t&) { return false; };
    SpeedhackState none;
    CHECK(!InstallSpeedhack(empty, true, 2.0, none, &error));
    CHECK(error.find("no timing function") != std::string::npos);

    CHECK(!UpdateSpeedhack(host, none, 2.0, &error));

    CHECK(InstallSpeedhack(host, true, 2.0, state, &error));
    CHECK(!InstallSpeedhack(host, true, 3.0, state, &error));
    CHECK(error.find("already installed") != std::string::npos);
    CHECK(RemoveSpeedhack(host, state, &error));
}

} // namespace

int main() {
    TestInstallUpdateRemove();
    TestX86SkipsTheCounter();
    TestErrors();
    if (failures) {
        std::cerr << failures << " check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "speedhack tests passed" << std::endl;
    return 0;
}
