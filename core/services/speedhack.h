#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace cortex::services {

// Cheat Engine's speedhack: the target's timing functions are hooked so
// they report a scaled clock, which makes a game run faster or slower.
// Each hook calls the real function through a trampoline, then rescales
// the result around the value seen on the first call, so time never jumps
// backwards when the multiplier changes.
//
// x64 targets get GetTickCount, GetTickCount64, QueryPerformanceCounter and
// timeGetTime. x86 targets get the 32-bit tick functions; scaling the
// 64-bit performance counter there needs 64-bit arithmetic the hook does
// not carry, so it is left alone and reported.

struct SpeedhackHost {
    std::function<bool(uint64_t address, void* buffer, size_t size)> read;
    std::function<bool(uint64_t address, const void* buffer, size_t size)> write;
    std::function<bool(size_t size, uint64_t nearAddress, uint64_t& address, std::string& error)> allocate;
    std::function<bool(uint64_t address, std::string& error)> release;
    // Resolves "kernel32.GetTickCount" and friends.
    std::function<bool(const std::string& name, uint64_t& value)> symbol;
};

struct SpeedhackHook {
    std::string function;                 // kernel32.GetTickCount
    uint64_t address = 0;                 // the hooked function
    uint64_t cave = 0;                    // data and hook code
    std::vector<uint8_t> original;        // bytes replaced at `address`
};

struct SpeedhackState {
    std::vector<SpeedhackHook> hooks;
    std::vector<std::string> skipped;     // functions that could not be hooked
    double multiplier = 1.0;
    bool active = false;
};

// Hooks the timing functions and starts scaling by `multiplier` (1.0 is
// normal speed). Fails only when no function at all could be hooked.
bool InstallSpeedhack(const SpeedhackHost& host, bool x64, double multiplier, SpeedhackState& state,
                      std::string* error = nullptr);

// Changes the multiplier of hooks that are already installed.
bool UpdateSpeedhack(const SpeedhackHost& host, SpeedhackState& state, double multiplier,
                     std::string* error = nullptr);

// Restores the original bytes and frees the caves.
bool RemoveSpeedhack(const SpeedhackHost& host, SpeedhackState& state, std::string* error = nullptr);

} // namespace cortex::services
