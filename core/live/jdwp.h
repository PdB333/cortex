#pragma once

// JDWP live provider for the JVM: the program is started by the operator with
//   java -agentlib:jdwp=transport=dt_socket,server=y,suspend=n,address=127.0.0.1:PORT ...
// and Cortex connects to 127.0.0.1:PORT.
//
// Log breakpoints only. A breakpoint is a class name (as loaded, e.g.
// "cortex.fixture.LiveApp") and a source line; it is set with suspend policy
// EVENT_THREAD, so only the hitting thread stops. At a hit Cortex reads the
// stack and the top frame's local variables (StackFrame.GetValues, which runs
// no Java code; objects are described by their type, strings by
// StringReference.Value) and resumes that thread. After max_hits the request
// is cleared. Detaching sends VirtualMachine.Dispose, which removes Cortex's
// requests and resumes anything still suspended. No method is ever invoked.

#include "cdp.h"  // LiveSession
#include "websocket.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cortex::live {

class JdwpSession final : public LiveSession {
public:
    JdwpSession() = default;
    ~JdwpSession() override;

    bool Attach(uint16_t port, std::string& error);

    std::string Protocol() const override { return "jdwp"; }
    void Detach() override;
    bool Connected() const override { return connected_; }
    json Status() const override;
    // Loaded classes (signatures), filtered by substring.
    json Scripts(const std::string& query, size_t offset, size_t limit) const override;
    json Source(const std::string& key, size_t firstLine, size_t lineCount) override;
    // `className`: binary name ("pkg.Outer$Inner") of a loaded class.
    json SetLogBreakpoint(const std::string& className, uint32_t line, uint32_t column, uint32_t maxHits, uint32_t maxFrames) override;
    json RemoveBreakpoint(int id) override;
    json Breakpoints() const override;
    json Hits(int id, size_t offset, size_t limit) const override;
    json Console(size_t offset, size_t limit) const override;

    using Bytes = std::vector<uint8_t>;

private:
    struct Reply {
        bool ok = false;
        uint16_t error = 0;
        Bytes data;
    };
    struct Reader;
    Reply Command(uint8_t set, uint8_t command, const Bytes& data, int timeoutMs = 10000);
    void ReadLoop();
    void EventLoop();
    void OnBreakpoint(int32_t requestId, uint64_t thread);
    std::string ClassSignature(uint64_t typeId);
    json MethodInfo(uint64_t typeId, uint64_t methodId);  // {"name", "signature"}
    json Value(Reader& reader, uint8_t tag);
    void PutId(Bytes& out, uint64_t value, int size) const;

    struct Breakpoint {
        int id = 0;
        int32_t requestId = 0;
        std::string className;
        uint32_t line = 0, maxHits = 0, maxFrames = 0, hits = 0;
        bool active = true;
        json location;
        std::deque<json> records;
        uint64_t dropped = 0;
    };

    TcpStream stream_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> stopping_{false};
    uint16_t port_ = 0;
    json vm_ = json::object();
    int fieldIdSize_ = 8, methodIdSize_ = 8, objectIdSize_ = 8, referenceTypeIdSize_ = 8, frameIdSize_ = 8;
    std::thread reader_, events_;

    mutable std::mutex mutex_;
    std::condition_variable responded_, queued_;
    std::map<uint32_t, Reply> replies_;
    uint32_t nextId_ = 0;
    std::deque<Bytes> eventQueue_;
    std::map<std::pair<uint64_t, uint64_t>, json> methods_;
    std::map<uint64_t, std::string> signatures_;

    std::map<int, Breakpoint> breakpoints_;
    int nextBreakpoint_ = 0;
    uint64_t events_total_ = 0;
    std::string lastError_;
};

} // namespace cortex::live
