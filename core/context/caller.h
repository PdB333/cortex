#pragma once

#include <string>
#include <utility>

namespace context {

// Who issued the request a runtime route handler is serving. It is set by the
// dispatcher from the authenticated pipe envelope (the desktop's own adapter
// marks its envelopes), never from tool arguments, so a handler can attribute
// provenance without trusting the caller's word. It is a label for
// provenance, not an authority: write and state permissions are checked
// separately (mcp_tools::Authority).
enum class Caller {
    Unattributed = 0,  // loopback HTTP API, runtime-internal threads
    Desktop,           // the Cortex desktop's private channel
    Agent,             // an MCP client through cortex.exe mcp
};

inline const char* CallerName(Caller caller) {
    switch (caller) {
        case Caller::Desktop: return "desktop";
        case Caller::Agent: return "agent";
        case Caller::Unattributed: break;
    }
    return "unattributed";
}

struct CallerInfo {
    Caller caller = Caller::Unattributed;
    std::string session;  // MCP session id or "desktop"; empty when unknown
};

namespace detail {
inline CallerInfo& CurrentCallerSlot() {
    thread_local CallerInfo current;
    return current;
}
} // namespace detail

// The caller of the request running on this thread. Native route dispatch
// is synchronous, so nested dispatches (batch steps) inherit it.
inline const CallerInfo& CurrentCaller() { return detail::CurrentCallerSlot(); }

class ScopedCaller {
public:
    ScopedCaller(Caller caller, std::string session) : previous_(detail::CurrentCallerSlot()) {
        detail::CurrentCallerSlot() = CallerInfo{caller, std::move(session)};
    }
    ~ScopedCaller() { detail::CurrentCallerSlot() = std::move(previous_); }
    ScopedCaller(const ScopedCaller&) = delete;
    ScopedCaller& operator=(const ScopedCaller&) = delete;

private:
    CallerInfo previous_;
};

} // namespace context
