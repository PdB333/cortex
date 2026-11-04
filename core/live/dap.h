#pragma once

// Debug Adapter Protocol (DAP) live provider: one client for the adapters
// the operator starts and points Cortex at, on 127.0.0.1:
//   - debugpy (Python): `python -m debugpy --listen 127.0.0.1:PORT app.py`;
//   - netcoredbg (.NET, MIT): `netcoredbg --interpreter=vscode --server=PORT`,
//     then Cortex attaches it to a .NET process id.
//
// Same rules as the CDP provider: log breakpoints only (no condition, no
// logMessage, no hit condition, no evaluate). At a hit the stack and the top
// frame's local variables (one level, not expanded) are recorded and the
// thread continues at once; after max_hits the breakpoint is removed. A stop
// Cortex did not set (breakpoint(), an exception filter of another client)
// is recorded and continued. Detaching removes Cortex's breakpoints and sends
// disconnect with terminateDebuggee=false.
//
// Values are the adapter's text: debugpy builds it with repr(), which runs
// the class's __repr__ for user objects; netcoredbg shows objects as their
// type name and does not run property getters for top-level locals.

#include "cdp.h"  // LiveSession
#include "websocket.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace cortex::live {

class DapSession final : public LiveSession {
public:
    DapSession() = default;
    ~DapSession() override;

    // adapter: "debugpy" (attach to the listener's own process) or
    // "netcoredbg" (attach to `processId`).
    bool Attach(uint16_t port, const std::string& adapter, uint64_t processId, std::string& error);

    std::string Protocol() const override { return "dap"; }
    void Detach() override;
    bool Connected() const override { return connected_; }
    json Status() const override;
    json Scripts(const std::string& query, size_t offset, size_t limit) const override;
    json Source(const std::string& path, size_t firstLine, size_t lineCount) override;
    json SetLogBreakpoint(const std::string& path, uint32_t line, uint32_t column, uint32_t maxHits, uint32_t maxFrames) override;
    json RemoveBreakpoint(int id) override;
    json Breakpoints() const override;
    json Hits(int id, size_t offset, size_t limit) const override;
    json Console(size_t offset, size_t limit) const override;

private:
    json Request(const std::string& command, const json& arguments, int timeoutMs = 10000);
    int64_t Send(const std::string& command, const json& arguments);
    json Wait(int64_t seq, int timeoutMs);
    bool WaitEvent(const std::string& name, int timeoutMs);
    json SyncSource(const std::string& path);  // setBreakpoints with the active ones of `path`
    void ReadLoop();
    void EventLoop();
    void OnStopped(const json& body);

    struct Breakpoint {
        int id = 0;
        std::string path;
        uint32_t line = 0, column = 0, maxHits = 0, maxFrames = 0;
        uint32_t hits = 0;
        bool active = true, verified = false;
        int64_t dapId = -1;
        uint32_t resolvedLine = 0;
        std::deque<json> records;
        uint64_t dropped = 0;
    };

    TcpStream stream_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> stopping_{false};
    uint16_t port_ = 0;
    std::string adapter_;
    uint64_t processId_ = 0;
    json capabilities_ = json::object();
    std::thread reader_, events_;

    mutable std::mutex mutex_;
    std::condition_variable responded_, queued_;
    std::map<int64_t, json> responses_;
    std::map<std::string, int> eventsSeen_;
    int64_t seq_ = 0;
    std::deque<json> eventQueue_;

    std::map<int, Breakpoint> breakpoints_;
    int nextBreakpoint_ = 0;
    std::deque<json> output_;
    uint64_t outputTotal_ = 0;
    std::deque<json> foreignStops_;
    uint64_t foreignStopTotal_ = 0;
    uint64_t stops_ = 0;
    std::string lastError_;
    // Source() reads only files the adapter reported in a stack frame: an MCP
    // client must not read arbitrary files through a live session.
    std::set<std::string> knownPaths_;
};

} // namespace cortex::live
