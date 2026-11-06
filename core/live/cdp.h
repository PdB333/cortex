#pragma once

// Chrome DevTools Protocol (CDP) live provider for Node.js and Electron
// (main process with --inspect, renderers with --remote-debugging-port).
//
// What it does: lists the inspector's targets, attaches, lists scripts and
// their source, records console messages, and sets *log* breakpoints. At a
// log breakpoint the call stack and the local variables are captured (without
// invoking getters) and execution resumes at once; after `max_hits` the
// breakpoint removes itself.
//
// What it never does: Runtime.evaluate, breakpoint conditions (evaluated in
// the target), setting variables, changing scripts, or leaving the program
// paused. A pause Cortex did not set (a `debugger;` statement) is recorded
// and resumed. Connections are to 127.0.0.1 only.

#include "websocket.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace cortex::live {

using json = nlohmann::json;

// GET /json/list and /json/version of a local inspector.
json ListTargets(uint16_t port);

// What every live session offers (CDP here, DAP in dap.h). Reads are
// observation; breakpoints are log breakpoints that never leave the program
// paused.
class LiveSession {
public:
    virtual ~LiveSession() = default;
    virtual std::string Protocol() const = 0;
    virtual void Detach() = 0;
    virtual bool Connected() const = 0;
    virtual json Status() const = 0;
    virtual json Scripts(const std::string& query, size_t offset, size_t limit) const = 0;
    virtual json Source(const std::string& key, size_t firstLine, size_t lineCount) = 0;
    // `location` is a script URL (CDP) or a source file path (DAP).
    virtual json SetLogBreakpoint(const std::string& location, uint32_t line, uint32_t column, uint32_t maxHits,
                                  uint32_t maxFrames) = 0;
    virtual json RemoveBreakpoint(int id) = 0;
    virtual json Breakpoints() const = 0;
    virtual json Hits(int id, size_t offset, size_t limit) const = 0;
    virtual json Console(size_t offset, size_t limit) const = 0;
};

class CdpSession final : public LiveSession {
public:
    CdpSession() = default;
    ~CdpSession() override;
    CdpSession(const CdpSession&) = delete;
    CdpSession& operator=(const CdpSession&) = delete;

    // Attaches to `targetId` (or the only/first node or page target) of
    // the inspector on `port`; enables the Debugger and Runtime domains.
    bool Attach(uint16_t port, const std::string& targetId, std::string& error);
    std::string Protocol() const override { return "cdp"; }
    void Detach() override;
    bool Connected() const override { return connected_; }

    json Status() const override;
    json Scripts(const std::string& query, size_t offset, size_t limit) const override;
    // Source by script id or URL; 1-based line range.
    json Source(const std::string& scriptIdOrUrl, size_t firstLine, size_t lineCount) override;
    // A log breakpoint at a 1-based line (and optional 1-based column) of a
    // script URL.
    json SetLogBreakpoint(const std::string& url, uint32_t line, uint32_t column, uint32_t maxHits, uint32_t maxFrames) override;
    json RemoveBreakpoint(int id) override;
    json Breakpoints() const override;
    json Hits(int id, size_t offset, size_t limit) const override;
    json Console(size_t offset, size_t limit) const override;

private:
    json Call(const std::string& method, const json& params, int timeoutMs = 5000);
    void ReadLoop();
    void EventLoop();
    void OnPaused(const json& params);
    json CaptureFrame(const json& frame, bool withLocals);
    json Describe(const json& remote) const;
    std::string UrlOf(const std::string& scriptId) const;

    struct Breakpoint {
        int id = 0;
        std::string cdpId, url;
        uint32_t line = 0, column = 0, maxHits = 0, maxFrames = 0;
        uint32_t hits = 0;
        bool active = true;
        json locations = json::array();
        std::deque<json> records;  // bounded
        uint64_t dropped = 0;
    };

    WebSocket socket_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> stopping_{false};
    uint16_t port_ = 0;
    json target_;
    std::thread reader_, events_;

    mutable std::mutex mutex_;
    std::condition_variable responded_, queued_;
    std::map<int64_t, json> responses_;
    int64_t nextId_ = 0;
    std::deque<json> eventQueue_;

    std::map<std::string, json> scripts_;  // scriptId -> {url, hash, lines, source_map_url}
    std::map<int, Breakpoint> breakpoints_;
    int nextBreakpoint_ = 0;
    std::deque<json> console_;
    uint64_t consoleTotal_ = 0;
    std::deque<json> foreignPauses_;
    uint64_t foreignPauseTotal_ = 0;
    uint64_t pauses_ = 0;
    std::string lastError_;
};

} // namespace cortex::live
