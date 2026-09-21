#include "veh_debug_provider.h"

#include <cstdlib>

namespace {
using json = nlohmann::json;
json RouteResult(const json& output) {
    if (!output.is_object()) return json::object();
    auto it = output.find("result");
    return it != output.end() ? *it : output;
}
uint64_t ParseAddress(const json& value) {
    if (value.is_number_unsigned()) return value.get<uint64_t>();
    if (value.is_number_integer()) return static_cast<uint64_t>(value.get<int64_t>());
    if (!value.is_string()) return 0;
    const std::string text = value.get<std::string>();
    char* end = nullptr;
    const auto parsed = std::strtoull(text.c_str(), &end, 0);
    return end && *end == '\0' ? static_cast<uint64_t>(parsed) : 0;
}
} // namespace

VehDebugProvider::VehDebugProvider(cortex::target::SessionManager& sessions,
                                   cortex::services::PayloadClient& payload)
    : service_(sessions), payload_(payload) {}

uint32_t VehDebugProvider::Capabilities() const {
    return DebugCapabilityMask(DebugCapability::SoftwareBreakpoint) |
           DebugCapabilityMask(DebugCapability::HardwareExecuteBreakpoint) |
           DebugCapabilityMask(DebugCapability::HardwareDataBreakpoint) |
           DebugCapabilityMask(DebugCapability::PauseResume) |
           DebugCapabilityMask(DebugCapability::Registers) |
           DebugCapabilityMask(DebugCapability::Step) |
           DebugCapabilityMask(DebugCapability::StepOver) |
           DebugCapabilityMask(DebugCapability::BreakpointLog);
}

bool VehDebugProvider::Attach(std::string* error) {
    if (!payload_.EnsureReady(error)) return false;
    json ignored;
    if (!payload_.CallRouteExisting("POST", "/debug/backend", {{"backend", "veh"}}, ignored, error)) return false;
    if (error) error->clear();
    return true;
}
void VehDebugProvider::Detach() {
    if (!payload_.Ready()) return;
    json ignored;
    std::string ignoredError;
    payload_.CallRouteExisting("POST", "/debug/backend", {{"backend", "windows"}}, ignored, &ignoredError);
}
bool VehDebugProvider::Ready() const { return payload_.Ready(); }
std::vector<uint64_t> VehDebugProvider::Threads(std::string* error) { return service_.Threads(error); }
bool VehDebugProvider::GetRegisters(uint64_t threadId, cortex::target::ThreadRegisterSnapshot& snapshot,
                                    std::string* error) { return service_.Registers(threadId, snapshot, error); }

bool VehDebugProvider::Call(const std::string& tool, const json& arguments, json& result, std::string* error) {
    json output;
    if (!payload_.CallTool(tool, arguments, output, error)) return false;
    result = RouteResult(output);
    if (error) error->clear();
    return true;
}

int VehDebugProvider::SetBreakpoint(const std::string& addressExpression, const std::string& kind, int size,
                                    bool pauseOnHit, bool processGlobal, uint64_t threadId, std::string* error) {
    json arguments = {{"address", addressExpression}, {"kind", kind}, {"size", size},
                      {"action", pauseOnHit ? "pause" : "log"}, {"process_global", processGlobal},
                      {"mutation_permission", true}};
    if (!processGlobal && threadId != 0) arguments["thread_id"] = threadId;
    json result;
    if (!Call("debug_breakpoint_add", arguments, result, error)) return -1;
    return result.value("id", result.value("breakpoint_id", -1));
}
bool VehDebugProvider::RemoveBreakpoint(int id, std::string* error) {
    json result;
    return Call("debug_breakpoint_delete", {{"_path", {{"id", id}}}, {"mutation_permission", true}}, result, error);
}
std::vector<DebugBreakpointInfo> VehDebugProvider::Breakpoints(std::string* error) {
    json result;
    if (!Call("debug_breakpoint_list", json::object(), result, error)) return {};
    std::vector<DebugBreakpointInfo> values;
    if (!result.is_object() || !result.contains("breakpoints") || !result["breakpoints"].is_array()) return values;
    for (const auto& item : result["breakpoints"]) {
        DebugBreakpointInfo value;
        value.id = item.value("id", -1); value.kind = item.value("kind", std::string());
        value.address = item.contains("address") ? ParseAddress(item["address"]) : 0;
        value.size = item.value("size", 1); value.pauseOnHit = item.value("action", std::string("pause")) == "pause";
        value.hitCount = item.value("hit_count", uint64_t{0}); value.processGlobal = item.value("process_global", true);
        value.targetThreadId = item.value("target_thread_id", uint64_t{0});
        value.appliedThreads = item.value("applied_threads", size_t{0}); value.totalThreads = item.value("total_threads", size_t{0});
        values.push_back(std::move(value));
    }
    return values;
}

bool VehDebugProvider::SnapshotFromJson(uint64_t threadId, const json& registers,
                                        cortex::target::ThreadRegisterSnapshot& snapshot) {
    if (!registers.is_object()) return false;
    snapshot = {}; snapshot.threadId = threadId;
    for (auto it = registers.begin(); it != registers.end(); ++it) {
        const uint64_t value = ParseAddress(it.value());
        if (it.key() == "rip" || it.key() == "eip") snapshot.instructionPointer = value;
        snapshot.registers.push_back({it.key(), value});
    }
    return true;
}

std::vector<DebugBreakpointLogEntry> VehDebugProvider::BreakpointLog(int id, uint64_t sinceSeq,
                                                                     size_t limit, std::string* error) {
    json arguments = {{"_path", {{"id", id}}}, {"_query", {{"since_seq", sinceSeq}, {"limit", limit}}}};
    json result;
    if (!Call("debug_breakpoint_log", arguments, result, error)) return {};
    std::vector<DebugBreakpointLogEntry> values;
    if (!result.is_object() || !result.contains("entries") || !result["entries"].is_array()) return values;
    for (const auto& item : result["entries"]) {
        DebugBreakpointLogEntry value;
        value.seq = item.value("seq", uint64_t{0}); value.threadId = item.value("thread_id", uint64_t{0});
        value.timestampMs = item.value("timestamp_ms", uint64_t{0});
        value.instruction = item.contains("instruction") ? ParseAddress(item["instruction"]) : 0;
        SnapshotFromJson(value.threadId, item.value("registers", json::object()), value.registers);
        values.push_back(std::move(value));
    }
    return values;
}

std::vector<DebugPausedThread> VehDebugProvider::PausedThreads(std::string* error) {
    json result;
    if (!Call("debug_paused", json::object(), result, error)) return {};
    std::vector<DebugPausedThread> values;
    if (!result.is_object() || !result.contains("threads") || !result["threads"].is_array()) return values;
    for (const auto& item : result["threads"]) {
        DebugPausedThread value;
        value.threadId = item.value("thread_id", uint64_t{0}); value.breakpointId = item.value("breakpoint_id", -1);
        SnapshotFromJson(value.threadId, item.value("registers", json::object()), value.registers);
        values.push_back(std::move(value));
    }
    return values;
}

bool VehDebugProvider::Pause(uint64_t threadId, cortex::target::ThreadRegisterSnapshot& snapshot, std::string* error) {
    json result;
    if (!Call("debug_pause", {{"thread_id", threadId}, {"mutation_permission", true}}, result, error)) return false;
    if (!SnapshotFromJson(threadId, result.value("registers", json::object()), snapshot)) {
        if (error) *error = "debug_pause_missing_registers";
        return false;
    }
    return true;
}
bool VehDebugProvider::Resume(uint64_t threadId, std::string* error) {
    json result;
    return Call("debug_continue", {{"thread_id", threadId}, {"mutation_permission", true}}, result, error);
}
bool VehDebugProvider::Step(uint64_t threadId, uint32_t timeoutMs,
                            cortex::target::ThreadRegisterSnapshot& snapshot, std::string* error) {
    json result;
    if (!Call("debug_step", {{"thread_id", threadId}, {"timeout_ms", timeoutMs}, {"mutation_permission", true}}, result, error)) return false;
    if (!SnapshotFromJson(threadId, result.value("registers", json::object()), snapshot)) {
        if (error) *error = "debug_step_missing_registers";
        return false;
    }
    return true;
}
bool VehDebugProvider::StepOver(uint64_t threadId, uint32_t timeoutMs,
                                cortex::target::ThreadRegisterSnapshot& snapshot, std::string* error) {
    json result;
    if (!Call("debug_step_over", {{"thread_id", threadId}, {"timeout_ms", timeoutMs}, {"mutation_permission", true}}, result, error)) return false;
    if (!SnapshotFromJson(threadId, result.value("registers", json::object()), snapshot)) {
        if (error) *error = "debug_step_over_missing_registers";
        return false;
    }
    return true;
}
