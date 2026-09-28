#include "windows_debug_provider.h"

#include <nlohmann/json.hpp>

namespace {
DebugBreakpointInfo Convert(const WindowsDebuggerBackend::BreakpointInfo& value) {
    DebugBreakpointInfo result;
    result.id = value.id; result.kind = value.kind; result.address = value.address;
    result.size = value.size; result.pauseOnHit = value.pauseOnHit; result.hitCount = value.hitCount;
    result.processGlobal = value.processGlobal; result.targetThreadId = value.targetThreadId;
    result.appliedThreads = value.appliedThreads; result.totalThreads = value.totalThreads;
    return result;
}
DebugBreakpointLogEntry Convert(const WindowsDebuggerBackend::BreakpointLogEntry& value) {
    DebugBreakpointLogEntry result;
    result.seq = value.seq; result.threadId = value.threadId; result.timestampMs = value.timestampMs;
    result.instruction = value.instruction; result.registers = value.registers;
    return result;
}
DebugPausedThread Convert(const WindowsDebuggerBackend::PausedThread& value) {
    DebugPausedThread result;
    result.threadId = value.threadId; result.breakpointId = value.breakpointId; result.registers = value.registers;
    return result;
}
} // namespace

WindowsDebugProvider::WindowsDebugProvider(cortex::target::SessionManager& sessions,
                                           cortex::services::PayloadClient* payload)
    : service_(sessions), payload_(payload), backend_(sessions) {}

uint32_t WindowsDebugProvider::Capabilities() const {
    return DebugCapabilityMask(DebugCapability::SoftwareBreakpoint) |
           DebugCapabilityMask(DebugCapability::HardwareExecuteBreakpoint) |
           DebugCapabilityMask(DebugCapability::HardwareDataBreakpoint) |
           DebugCapabilityMask(DebugCapability::PauseResume) |
           DebugCapabilityMask(DebugCapability::Registers) |
           DebugCapabilityMask(DebugCapability::Step) |
           DebugCapabilityMask(DebugCapability::StepOver) |
           DebugCapabilityMask(DebugCapability::BreakpointLog);
}

bool WindowsDebugProvider::Attach(std::string* error) {
    if (payload_ && payload_->Ready()) {
        nlohmann::json ignored;
        std::string routeError;
        if (!payload_->CallRouteExisting("POST", "/debug/backend", {{"backend", "windows"}},
                                         ignored, &routeError)) {
            if (error) *error = routeError;
            return false;
        }
    }
    return backend_.ensureAttached(error);
}

void WindowsDebugProvider::Detach() { backend_.detach(); }
bool WindowsDebugProvider::Ready() const { return backend_.attached(); }
std::vector<uint64_t> WindowsDebugProvider::Threads(std::string* error) { return service_.Threads(error); }
bool WindowsDebugProvider::GetRegisters(uint64_t threadId, cortex::target::ThreadRegisterSnapshot& snapshot,
                                        std::string* error) {
    return backend_.readRegisters(threadId, snapshot, error);
}
int WindowsDebugProvider::SetBreakpoint(const std::string& addressExpression, const std::string& kind,
                                        int size, bool pauseOnHit, bool processGlobal, uint64_t threadId,
                                        std::string* error) {
    uint64_t address = 0;
    if (!backend_.resolveAddress(addressExpression, address, error)) return -1;
    return backend_.addBreakpoint(kind, address, size, pauseOnHit, processGlobal, threadId, error);
}
bool WindowsDebugProvider::RemoveBreakpoint(int id, std::string* error) { return backend_.removeBreakpoint(id, error); }
std::vector<DebugBreakpointInfo> WindowsDebugProvider::Breakpoints(std::string* error) {
    if (error) error->clear();
    std::vector<DebugBreakpointInfo> result;
    for (const auto& value : backend_.breakpoints()) result.push_back(Convert(value));
    return result;
}
std::vector<DebugBreakpointLogEntry> WindowsDebugProvider::BreakpointLog(int id, uint64_t sinceSeq,
                                                                         size_t limit, std::string* error) {
    if (error) error->clear();
    std::vector<DebugBreakpointLogEntry> result;
    for (const auto& value : backend_.breakpointLog(id, sinceSeq, limit)) result.push_back(Convert(value));
    if (result.empty()) {
        bool exists = false;
        for (const auto& bp : backend_.breakpoints()) if (bp.id == id) exists = true;
        if (!exists && error) *error = "unknown_breakpoint";
    }
    return result;
}
std::vector<DebugPausedThread> WindowsDebugProvider::PausedThreads(std::string* error) {
    if (error) error->clear();
    std::vector<DebugPausedThread> result;
    for (const auto& value : backend_.pausedThreads()) result.push_back(Convert(value));
    return result;
}
bool WindowsDebugProvider::Pause(uint64_t threadId, cortex::target::ThreadRegisterSnapshot& snapshot,
                                 std::string* error) { return backend_.pauseThread(threadId, snapshot, error); }
bool WindowsDebugProvider::Resume(uint64_t threadId, std::string* error) { return backend_.continueThread(threadId, error); }
bool WindowsDebugProvider::Step(uint64_t threadId, uint32_t timeoutMs,
                                cortex::target::ThreadRegisterSnapshot& snapshot, std::string* error) {
    return backend_.stepThread(threadId, timeoutMs, snapshot, error);
}
bool WindowsDebugProvider::StepOver(uint64_t threadId, uint32_t timeoutMs,
                                    cortex::target::ThreadRegisterSnapshot& snapshot, std::string* error) {
    return backend_.stepOverThread(threadId, timeoutMs, snapshot, error);
}
