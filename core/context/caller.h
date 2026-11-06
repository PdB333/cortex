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
// The per-thread slot holds no object with a destructor. With MinGW, a
// thread_local with a non-trivial destructor in a DLL (this runtime) can be
// destroyed after its TLS block was freed when a thread exits, which crashed
// the target (~CallerInfo reading freed memory). A fixed buffer avoids it.
struct CallerSlot {
    Caller caller = Caller::Unattributed;
    unsigned char length = 0;
    char session[127] = {};
};
inline CallerSlot& CurrentCallerSlot() {
    thread_local CallerSlot current;
    return current;
}
inline CallerInfo Read(const CallerSlot& slot) { return CallerInfo{slot.caller, std::string(slot.session, slot.length)}; }
inline void Write(CallerSlot& slot, const CallerInfo& info) {
    slot.caller = info.caller;
    const size_t length = info.session.size() < sizeof slot.session ? info.session.size() : sizeof slot.session;
    for (size_t i = 0; i < length; ++i) slot.session[i] = info.session[i];
    slot.length = static_cast<unsigned char>(length);
}
} // namespace detail

// The caller of the request running on this thread. Native route dispatch
// is synchronous, so nested dispatches (batch steps) inherit it.
inline CallerInfo CurrentCaller() { return detail::Read(detail::CurrentCallerSlot()); }

class ScopedCaller {
public:
    ScopedCaller(Caller caller, std::string session) : previous_(CurrentCaller()) {
        detail::Write(detail::CurrentCallerSlot(), CallerInfo{caller, std::move(session)});
    }
    ~ScopedCaller() { detail::Write(detail::CurrentCallerSlot(), previous_); }
    ScopedCaller(const ScopedCaller&) = delete;
    ScopedCaller& operator=(const ScopedCaller&) = delete;

private:
    CallerInfo previous_;
};

} // namespace context
