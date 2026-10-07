#pragma once

// Append-only record of state-changing calls that Cortex refused.
//
// An AI client has no way to grant itself write authority, so a refused
// mutating call is a meaningful signal: it was attempted and it did not
// happen. Every refusal is kept in memory and, once a directory is set,
// appended as one JSON object per line to <directory>/cortex_denied_mutations.jsonl.
//
//   {"ts_ms":1760000000000,"ts_utc":"2026-10-07T12:00:00Z","source":"runtime",
//    "tool":"memory_write","reason":"write_authority_required","arguments":{...}}
//
// Header-only so the injected runtime, the desktop host and the tests share
// one implementation.

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <ctime>
#include <fstream>
#include <mutex>
#include <string>

namespace cortex::security {

namespace detail {
struct DenialState {
    std::mutex mutex;
    std::string directory;
    std::atomic<unsigned long long> count{0};
};
inline DenialState& State() {
    static DenialState state;
    return state;
}
inline constexpr size_t kMaxArgumentBytes = 4096;
} // namespace detail

// Where the log file goes. Empty (the default) keeps refusals in memory only.
inline void SetDenialDirectory(const std::string& directory) {
    auto& state = detail::State();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.directory = directory;
}

inline unsigned long long DenialCount() {
    return detail::State().count.load(std::memory_order_relaxed);
}

inline std::string DenialLogName() { return "cortex_denied_mutations.jsonl"; }

// `source` says who refused ("runtime" or "host"); `reason` is the stable
// error code returned to the caller.
inline void RecordDenial(const std::string& source,
                         const std::string& tool,
                         const std::string& reason,
                         const nlohmann::json& arguments,
                         const std::string& semanticTool = {}) {
    auto& state = detail::State();
    const auto now = std::chrono::system_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

    nlohmann::json entry = {{"ts_ms", ms}, {"source", source}, {"tool", tool}, {"reason", reason}};
    if (!semanticTool.empty()) entry["semantic_tool"] = semanticTool;
    std::string dumped = arguments.is_null() ? std::string("{}") : arguments.dump();
    if (dumped.size() > detail::kMaxArgumentBytes) {
        entry["arguments_truncated"] = true;
        dumped.resize(detail::kMaxArgumentBytes);
        entry["arguments"] = dumped;
    } else {
        entry["arguments"] = arguments.is_null() ? nlohmann::json::object() : arguments;
    }

    std::lock_guard<std::mutex> lock(state.mutex);
    std::time_t seconds = static_cast<std::time_t>(ms / 1000);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    char stamp[32] = {};
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
    entry["ts_utc"] = stamp;
    state.count.fetch_add(1, std::memory_order_relaxed);
    if (state.directory.empty()) return;
    std::ofstream out(state.directory + "/" + DenialLogName(), std::ios::app | std::ios::binary);
    if (out) out << entry.dump() << '\n';
}

// Debugger attach/detach on a target. A debugger attached to a process
// suspends its threads while it handles events, so "read-only" tools that use
// it still have an effect on the target. Each attach/detach is appended to
// <directory>/cortex_debugger_events.jsonl so a benchmark can count them.
inline std::string DebuggerLogName() { return "cortex_debugger_events.jsonl"; }

inline void RecordDebuggerEvent(const std::string& phase,
                                unsigned long long processId,
                                const std::string& backend,
                                const std::string& detail = {}) {
    auto& state = detail::State();
    const auto now = std::chrono::system_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    nlohmann::json entry = {{"ts_ms", ms}, {"phase", phase}, {"pid", processId}, {"backend", backend}};
    if (!detail.empty()) entry["detail"] = detail;
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.directory.empty()) return;
    std::ofstream out(state.directory + "/" + DebuggerLogName(), std::ios::app | std::ios::binary);
    if (out) out << entry.dump() << '\n';
}

} // namespace cortex::security
