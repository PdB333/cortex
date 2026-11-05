#include "dap.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace cortex::live {
namespace {

constexpr size_t kMaxRecords = 1000;
constexpr size_t kMaxOutput = 2000;
constexpr size_t kMaxForeign = 200;
constexpr size_t kMaxLocals = 48;

std::string Clip(std::string text, size_t size = 512) {
    if (text.size() > size) text = text.substr(0, size) + "...";
    return text;
}

std::string Now() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    char text[40], out[48];
    std::strftime(text, sizeof text, "%Y-%m-%dT%H:%M:%S", &utc);
    std::snprintf(out, sizeof out, "%s.%03dZ", text, static_cast<int>(ms));
    return out;
}

// Paths compare case-insensitively with either slash (Windows adapters).
std::string PathKey(std::string path) {
    for (auto& c : path) c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return path;
}

} // namespace

DapSession::~DapSession() { Detach(); }

int64_t DapSession::Send(const std::string& command, const json& arguments) {
    int64_t seq = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        seq = ++seq_;
    }
    const std::string body = json{{"seq", seq}, {"type", "request"}, {"command", command}, {"arguments", arguments}}.dump();
    if (!stream_.Send("Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body)) return -1;
    return seq;
}

json DapSession::Wait(int64_t seq, int timeoutMs) {
    if (seq < 0) return {{"success", false}, {"message", "send_failed"}};
    std::unique_lock<std::mutex> lock(mutex_);
    if (!responded_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return responses_.count(seq) > 0 || stopping_; }))
        return {{"success", false}, {"message", "timeout"}};
    if (!responses_.count(seq)) return {{"success", false}, {"message", "disconnected"}};
    json reply = std::move(responses_[seq]);
    responses_.erase(seq);
    return reply;
}

json DapSession::Request(const std::string& command, const json& arguments, int timeoutMs) {
    return Wait(Send(command, arguments), timeoutMs);
}

bool DapSession::WaitEvent(const std::string& name, int timeoutMs) {
    std::unique_lock<std::mutex> lock(mutex_);
    return responded_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return eventsSeen_[name] > 0 || stopping_; }) &&
           eventsSeen_[name] > 0;
}

bool DapSession::Attach(uint16_t port, const std::string& adapter, uint64_t processId, std::string& error) {
    if (adapter != "debugpy" && adapter != "netcoredbg") {
        error = "unknown_adapter";
        return false;
    }
    if (adapter == "netcoredbg" && processId == 0) {
        error = "process_id_required";
        return false;
    }
    if (!stream_.Connect(port, error)) return false;
    port_ = port;
    adapter_ = adapter;
    processId_ = processId;
    connected_ = true;
    stopping_ = false;
    reader_ = std::thread([this] { ReadLoop(); });
    events_ = std::thread([this] { EventLoop(); });
    const json init = Request("initialize", {{"clientID", "cortex"}, {"clientName", "Cortex"}, {"adapterID", adapter},
                                             {"linesStartAt1", true}, {"columnsStartAt1", true}, {"pathFormat", "path"},
                                             {"supportsVariableType", true}, {"supportsRunInTerminalRequest", false}},
                              60000);  // a first start can be slow (netcoredbg loads its managed part)
    if (!init.value("success", false)) {
        error = "dap_initialize_failed: " + init.value("message", std::string());
        Detach();
        return false;
    }
    capabilities_ = init.value("body", json::object());
    // debugpy answers attach only after configurationDone; netcoredbg
    // answers it first. Send it, then complete the configuration.
    json attach = json::object();
    // redirectOutput: the program's stdout/stderr are also sent as output
    // events (it still writes to its own streams).
    if (adapter == "debugpy") attach = {{"justMyCode", false}, {"subProcess", false}, {"redirectOutput", true}};
    else attach = {{"processId", processId}};
    const int64_t attachSeq = Send("attach", attach);
    WaitEvent("initialized", 60000);
    const json done = Request("configurationDone", json::object(), 60000);
    const json attached = Wait(attachSeq, 60000);
    if (!attached.value("success", false)) {
        // The adapter's own explanation (message, and body.error.format).
        std::string detail = attached.value("message", std::string());
        const json body = attached.value("body", json::object());
        if (body.is_object() && body.contains("error") && body["error"].is_object())
            detail += " " + body["error"].value("format", std::string());
        error = "dap_attach_failed: " + detail;
        // What the adapter said meanwhile (output events), for the operator.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            std::string said;
            for (const auto& entry : output_) said += entry.value("text", std::string());
            if (!said.empty()) error += " | adapter output: " + Clip(said, 1500);
        }
        Detach();
        return false;
    }
    if (!done.value("success", false) && capabilities_.value("supportsConfigurationDoneRequest", false)) {
        error = "dap_configuration_failed";
        Detach();
        return false;
    }
    return true;
}

void DapSession::Detach() {
    if (!connected_ && !reader_.joinable()) return;
    std::vector<std::string> paths;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [id, bp] : breakpoints_)
            if (bp.active) bp.active = false, paths.push_back(bp.path);
    }
    if (connected_) {
        std::sort(paths.begin(), paths.end());
        paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
        for (const auto& path : paths) Request("setBreakpoints", {{"source", {{"path", path}}}, {"breakpoints", json::array()}}, 3000);
        Request("disconnect", {{"restart", false}, {"terminateDebuggee", false}}, 3000);
    }
    stopping_ = true;
    connected_ = false;
    stream_.Close();
    responded_.notify_all();
    queued_.notify_all();
    if (reader_.joinable()) reader_.join();
    if (events_.joinable()) events_.join();
}

void DapSession::ReadLoop() {
    std::string buffer;
    char chunk[8192];
    while (!stopping_) {
        const int got = stream_.Read(chunk, sizeof chunk, 200);
        if (got == 0) continue;
        if (got < 0) break;
        buffer.append(chunk, static_cast<size_t>(got));
        if (buffer.size() > (64u << 20)) break;  // bounded
        for (;;) {
            const size_t end = buffer.find("\r\n\r\n");
            if (end == std::string::npos) break;
            size_t length = 0;
            const std::string header = buffer.substr(0, end);
            const size_t at = header.find("Content-Length:");
            if (at == std::string::npos) {
                buffer.erase(0, end + 4);
                continue;
            }
            length = static_cast<size_t>(std::strtoull(header.c_str() + at + 15, nullptr, 10));
            if (buffer.size() < end + 4 + length) break;
            json message = json::parse(buffer.substr(end + 4, length), nullptr, false);
            buffer.erase(0, end + 4 + length);
            if (!message.is_object()) continue;
            std::lock_guard<std::mutex> lock(mutex_);
            const std::string type = message.value("type", std::string());
            if (type == "response") {
                const int64_t seq = message.value("request_seq", int64_t{0});
                responses_[seq] = std::move(message);
                responded_.notify_all();
            } else if (type == "event") {
                ++eventsSeen_[message.value("event", std::string())];
                responded_.notify_all();
                eventQueue_.push_back(std::move(message));
                queued_.notify_all();
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!stopping_) lastError_ = "adapter_disconnected";
        connected_ = false;
    }
    responded_.notify_all();
    queued_.notify_all();
}

void DapSession::OnStopped(const json& body) {
    ++stops_;
    const int64_t thread = body.value("threadId", int64_t{0});
    const json trace = Request("stackTrace", {{"threadId", thread}, {"startFrame", 0}, {"levels", 32}});
    const json frames = trace.value("body", json::object()).value("stackFrames", json::array());
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& frame : frames)
            if (frame.is_object() && knownPaths_.size() < 10000) {
                const std::string path = frame.value("source", json::object()).value("path", std::string());
                if (!path.empty()) knownPaths_.insert(PathKey(path));
            }
    }
    auto frameJson = [](const json& frame) {
        return json{{"function", Clip(frame.value("name", std::string()))},
                    {"path", frame.value("source", json::object()).value("path", std::string())},
                    {"line", frame.value("line", 0)},
                    {"column", frame.value("column", 0)}};
    };
    int ours = 0;
    if (!frames.empty() && frames[0].is_object()) {
        const std::string key = PathKey(frames[0].value("source", json::object()).value("path", std::string()));
        const int line = frames[0].value("line", 0);
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& hit : body.value("hitBreakpointIds", json::array()))
            for (auto& [id, bp] : breakpoints_)
                if (bp.active && bp.dapId >= 0 && hit.is_number_integer() && hit.get<int64_t>() == bp.dapId) ours = id;
        if (!ours)
            for (auto& [id, bp] : breakpoints_)
                if (bp.active && PathKey(bp.path) == key &&
                    (static_cast<int>(bp.resolvedLine ? bp.resolvedLine : bp.line) == line))
                    ours = id;
    }
    if (!ours) {
        json record{{"time", Now()}, {"reason", body.value("reason", std::string())}};
        if (!frames.empty() && frames[0].is_object()) record["frame"] = frameJson(frames[0]);
        std::lock_guard<std::mutex> lock(mutex_);
        foreignStops_.push_back(record);
        ++foreignStopTotal_;
        while (foreignStops_.size() > kMaxForeign) foreignStops_.pop_front();
    } else {
        uint32_t maxFrames = 8;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            maxFrames = breakpoints_[ours].maxFrames;
        }
        json stack = json::array();
        for (size_t i = 0; i < frames.size() && i < maxFrames; ++i) stack.push_back(frameJson(frames[i]));
        if (!stack.empty()) {
            // The first scope of the top frame (Locals); one level only.
            json locals = json::object();
            const json scopes = Request("scopes", {{"frameId", frames[0].value("id", 0)}});
            const json list = scopes.value("body", json::object()).value("scopes", json::array());
            if (!list.empty() && list[0].is_object()) {
                const json variables = Request("variables", {{"variablesReference", list[0].value("variablesReference", 0)}, {"count", kMaxLocals}});
                for (const auto& variable : variables.value("body", json::object()).value("variables", json::array())) {
                    if (locals.size() >= kMaxLocals || !variable.is_object()) break;
                    const std::string name = variable.value("name", std::string());
                    if (name.rfind("special variables", 0) == 0 || name.rfind("function variables", 0) == 0) continue;
                    locals[name] = {{"value", Clip(variable.value("value", std::string()))}, {"type", variable.value("type", std::string())}};
                }
            }
            stack[0]["locals"] = locals;
        }
        bool exhausted = false;
        std::string path;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto& bp = breakpoints_[ours];
            ++bp.hits;
            bp.records.push_back({{"hit", bp.hits}, {"time", Now()}, {"thread", thread}, {"frames", stack}});
            while (bp.records.size() > kMaxRecords) bp.records.pop_front(), ++bp.dropped;
            exhausted = bp.hits >= bp.maxHits;
            if (exhausted) bp.active = false, path = bp.path;
        }
        if (exhausted) SyncSource(path);
    }
    Request("continue", {{"threadId", thread}});
}

void DapSession::EventLoop() {
    for (;;) {
        json event;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            queued_.wait(lock, [&] { return !eventQueue_.empty() || stopping_ || !connected_; });
            if (eventQueue_.empty()) return;
            event = std::move(eventQueue_.front());
            eventQueue_.pop_front();
        }
        const std::string name = event.value("event", std::string());
        const json body = event.value("body", json::object());
        if (name == "stopped") {
            if (connected_) OnStopped(body);
        } else if (name == "output") {
            const std::string category = body.value("category", std::string());
            if (category == "telemetry") continue;
            std::lock_guard<std::mutex> lock(mutex_);
            output_.push_back({{"time", Now()}, {"type", category}, {"text", Clip(body.value("output", std::string()), 2048)}});
            ++outputTotal_;
            while (output_.size() > kMaxOutput) output_.pop_front();
        } else if (name == "breakpoint") {
            const json changed = body.value("breakpoint", json::object());
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& [id, bp] : breakpoints_)
                if (bp.dapId >= 0 && changed.value("id", int64_t{-2}) == bp.dapId) {
                    bp.verified = changed.value("verified", bp.verified);
                    if (changed.contains("line")) bp.resolvedLine = changed.value("line", 0u);
                }
        } else if (name == "terminated" || name == "exited") {
            std::lock_guard<std::mutex> lock(mutex_);
            lastError_ = "debuggee_" + name;
        }
    }
}

json DapSession::SyncSource(const std::string& path) {
    std::vector<int> ids;
    json requested = json::array();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [id, bp] : breakpoints_)
            if (bp.active && bp.path == path) {
                ids.push_back(id);
                json entry{{"line", bp.line}};
                if (bp.column) entry["column"] = bp.column;
                requested.push_back(entry);  // no condition, no logMessage
            }
    }
    const json reply = Request("setBreakpoints", {{"source", {{"path", path}}}, {"breakpoints", requested}, {"sourceModified", false}});
    const json answered = reply.value("body", json::object()).value("breakpoints", json::array());
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < ids.size() && i < answered.size(); ++i) {
        auto& bp = breakpoints_[ids[i]];
        bp.verified = answered[i].value("verified", false);
        bp.dapId = answered[i].value("id", int64_t{-1});
        bp.resolvedLine = answered[i].value("line", 0u);
    }
    return reply;
}

json DapSession::SetLogBreakpoint(const std::string& path, uint32_t line, uint32_t column, uint32_t maxHits, uint32_t maxFrames) {
    if (!connected_) return {{"ok", false}, {"error", "not_connected"}};
    int id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Breakpoint bp;
        bp.id = id = ++nextBreakpoint_;
        bp.path = path;
        bp.line = line;
        bp.column = column;
        bp.maxHits = maxHits;
        bp.maxFrames = maxFrames;
        breakpoints_[id] = std::move(bp);
    }
    const json reply = SyncSource(path);
    if (!reply.value("success", false)) {
        std::lock_guard<std::mutex> lock(mutex_);
        breakpoints_.erase(id);
        return {{"ok", false}, {"error", "dap_failed"}, {"message", reply.value("message", std::string())}};
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto& bp = breakpoints_[id];
    return {{"ok", true}, {"breakpoint", id}, {"verified", bp.verified}, {"resolved", json::array({{{"line", bp.resolvedLine}}})},
            {"action", "log"}, {"note", "records the stack and the top frame's locals, then continues; removed after max_hits"}};
}

json DapSession::RemoveBreakpoint(int id) {
    std::string path;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = breakpoints_.find(id);
        if (it == breakpoints_.end()) return {{"ok", false}, {"error", "unknown_breakpoint"}};
        if (!it->second.active) return {{"ok", true}, {"breakpoint", id}, {"already_inactive", true}};
        it->second.active = false;
        path = it->second.path;
    }
    const json reply = SyncSource(path);
    return {{"ok", reply.value("success", false)}, {"breakpoint", id}};
}

json DapSession::Breakpoints() const {
    std::lock_guard<std::mutex> lock(mutex_);
    json rows = json::array();
    for (const auto& [id, bp] : breakpoints_)
        rows.push_back({{"breakpoint", id}, {"path", bp.path}, {"line", bp.line}, {"active", bp.active}, {"verified", bp.verified},
                        {"hits", bp.hits}, {"max_hits", bp.maxHits}, {"dropped", bp.dropped}});
    return {{"ok", true}, {"breakpoints", rows}};
}

json DapSession::Hits(int id, size_t offset, size_t limit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = breakpoints_.find(id);
    if (it == breakpoints_.end()) return {{"ok", false}, {"error", "unknown_breakpoint"}};
    json rows = json::array();
    for (size_t i = offset; i < it->second.records.size() && rows.size() < limit; ++i) rows.push_back(it->second.records[i]);
    return {{"ok", true}, {"breakpoint", id}, {"hits", it->second.hits}, {"active", it->second.active},
            {"verified", it->second.verified}, {"dropped", it->second.dropped}, {"records", rows}};
}

json DapSession::Console(size_t offset, size_t limit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    json rows = json::array();
    const uint64_t first = outputTotal_ - output_.size();
    for (size_t i = 0; i < output_.size() && rows.size() < limit; ++i)
        if (first + i >= offset) {
            json entry = output_[i];
            entry["index"] = first + i;
            rows.push_back(std::move(entry));
        }
    return {{"ok", true}, {"total", outputTotal_}, {"oldest_kept", first}, {"messages", rows}};
}

json DapSession::Status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t active = 0;
    for (const auto& [id, bp] : breakpoints_) active += bp.active ? 1 : 0;
    json foreign = json::array();
    for (const auto& stop : foreignStops_) foreign.push_back(stop);
    return {{"ok", true},
            {"protocol", "dap"},
            {"adapter", adapter_},
            {"connected", connected_.load()},
            {"port", port_},
            {"target", {{"type", adapter_ == "debugpy" ? "python" : "dotnet"}, {"title", adapter_ + (processId_ ? " pid " + std::to_string(processId_) : "")},
                        {"process_id", processId_}}},
            {"breakpoints_active", active},
            {"pauses", stops_},
            {"console_messages", outputTotal_},
            {"foreign_pauses", foreignStopTotal_},
            {"foreign_pause_records", foreign},
            {"disconnect_reason", lastError_}};
}

json DapSession::Scripts(const std::string&, size_t, size_t) const {
    return {{"ok", false}, {"error", "not_supported"}, {"message", "DAP adapters do not list loaded sources here; give the source file path"}};
}

json DapSession::Source(const std::string& path, size_t firstLine, size_t lineCount) {
    // DAP sources are files on this machine. Only those the adapter reported
    // in a stack are read (others go through cortex_artifact_read and its
    // --artifact-root).
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!knownPaths_.count(PathKey(path)))
            return {{"ok", false}, {"error", "path_not_reported_by_debugger"},
                    {"message", "Only files that appeared in a recorded stack are read; use cortex_artifact_read for others."}};
    }
    std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
    if (!file) return {{"ok", false}, {"error", "script_not_found"}};
    std::string text, line;
    size_t number = 0, total = 0, bytes = 0;
    while (std::getline(file, line)) {
        ++number;
        ++total;
        bytes += line.size() + 1;
        if (number >= firstLine && number < firstLine + lineCount) text += line + "\n";
        if (bytes > (32u << 20)) break;
    }
    return {{"ok", true}, {"url", path}, {"first_line", firstLine}, {"total_lines", total}, {"bytes", bytes}, {"text", text}};
}

} // namespace cortex::live
