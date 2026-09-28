#pragma once

#include "target/thread_provider.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

enum class DebugCapability : uint32_t {
    SoftwareBreakpoint = 1u << 0,
    HardwareExecuteBreakpoint = 1u << 1,
    HardwareDataBreakpoint = 1u << 2,
    PauseResume = 1u << 3,
    Registers = 1u << 4,
    Step = 1u << 5,
    StepOver = 1u << 6,
    BreakpointLog = 1u << 7,
};

inline uint32_t DebugCapabilityMask(DebugCapability value) {
    return static_cast<uint32_t>(value);
}

inline const char* DebugCapabilityName(DebugCapability value) {
    switch (value) {
        case DebugCapability::SoftwareBreakpoint: return "debug.breakpoint.software";
        case DebugCapability::HardwareExecuteBreakpoint: return "debug.breakpoint.hardware_execute";
        case DebugCapability::HardwareDataBreakpoint: return "debug.breakpoint.hardware_data";
        case DebugCapability::PauseResume: return "debug.pause_resume";
        case DebugCapability::Registers: return "debug.registers";
        case DebugCapability::Step: return "debug.step";
        case DebugCapability::StepOver: return "debug.step_over";
        case DebugCapability::BreakpointLog: return "debug.breakpoint.log";
        default: return "debug.unknown";
    }
}

inline std::vector<std::string> DebugCapabilityNames(uint32_t capabilities) {
    std::vector<std::string> result;
    for (uint32_t bit = 0; bit < 8; ++bit) {
        const auto capability = static_cast<DebugCapability>(1u << bit);
        if ((capabilities & DebugCapabilityMask(capability)) != 0)
            result.emplace_back(DebugCapabilityName(capability));
    }
    return result;
}

struct DebugBreakpointInfo {
    int id = -1;
    std::string kind;
    uint64_t address = 0;
    int size = 1;
    bool pauseOnHit = true;
    uint64_t hitCount = 0;
    bool processGlobal = true;
    uint64_t targetThreadId = 0;
    size_t appliedThreads = 0;
    size_t totalThreads = 0;
};

struct DebugBreakpointLogEntry {
    uint64_t seq = 0;
    uint64_t threadId = 0;
    uint64_t timestampMs = 0;
    uint64_t instruction = 0;
    cortex::target::ThreadRegisterSnapshot registers;
};

struct DebugPausedThread {
    uint64_t threadId = 0;
    int breakpointId = -1;
    cortex::target::ThreadRegisterSnapshot registers;
};

class DebugProvider {
public:
    virtual ~DebugProvider() = default;

    virtual const char* Name() const = 0;
    virtual bool UsesInjectedRuntime() const = 0;
    virtual uint32_t Capabilities() const = 0;
    bool Supports(DebugCapability capability) const {
        return (Capabilities() & DebugCapabilityMask(capability)) != 0;
    }

    virtual bool Attach(std::string* error = nullptr) = 0;
    virtual void Detach() = 0;
    virtual bool Ready() const = 0;

    virtual std::vector<uint64_t> Threads(std::string* error = nullptr) = 0;
    virtual bool GetRegisters(uint64_t threadId,
                              cortex::target::ThreadRegisterSnapshot& snapshot,
                              std::string* error = nullptr) = 0;

    virtual int SetBreakpoint(const std::string& addressExpression,
                              const std::string& kind,
                              int size,
                              bool pauseOnHit,
                              bool processGlobal,
                              uint64_t threadId,
                              std::string* error = nullptr) = 0;
    virtual bool RemoveBreakpoint(int id, std::string* error = nullptr) = 0;
    virtual std::vector<DebugBreakpointInfo> Breakpoints(std::string* error = nullptr) = 0;
    virtual std::vector<DebugBreakpointLogEntry> BreakpointLog(int id,
                                                               uint64_t sinceSeq,
                                                               size_t limit,
                                                               std::string* error = nullptr) = 0;
    virtual std::vector<DebugPausedThread> PausedThreads(std::string* error = nullptr) = 0;

    virtual bool Pause(uint64_t threadId,
                       cortex::target::ThreadRegisterSnapshot& snapshot,
                       std::string* error = nullptr) = 0;
    virtual bool Resume(uint64_t threadId, std::string* error = nullptr) = 0;
    virtual bool Step(uint64_t threadId,
                      uint32_t timeoutMs,
                      cortex::target::ThreadRegisterSnapshot& snapshot,
                      std::string* error = nullptr) = 0;
    virtual bool StepOver(uint64_t threadId,
                          uint32_t timeoutMs,
                          cortex::target::ThreadRegisterSnapshot& snapshot,
                          std::string* error = nullptr) = 0;
};
