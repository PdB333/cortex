#include "dap.h"
#include "json_safe.h"
#include "protocol_frames.h"

#include <type_traits>

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

// Cut at a character boundary, and keep only valid UTF-8: the text goes into
// JSON, whose serialization throws on an invalid byte.
std::string Clip(std::string text, size_t size = 512) { return jsafe::Clip(text, size); }

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
    if (!SendRaw("Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body)) return -1;
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
    return Handshake(adapter, processId, error);
}

bool DapSession::AttachStdio(const std::string& adapterPath, const std::string& adapter, uint64_t processId, std::string& error,
                             const std::string& logFile) {
    if (adapter != "netcoredbg") {
        error = "stdio_mode_is_for_netcoredbg";
        return false;
    }
    if (processId == 0) {
        error = "process_id_required";
        return false;
    }
    child_ = std::make_unique<providers::ChildProcess>();
    providers::ProcessSpec spec;
    spec.argv = {adapterPath, "--interpreter=vscode"};
    spec.outputFile = logFile;
    std::error_code ec;
    const auto folder = std::filesystem::u8path(adapterPath).parent_path();
    if (!folder.empty()) spec.workingDirectory = folder.u8string();
    if (!child_->Start(spec, error)) {
        child_.reset();
        return false;
    }
    return Handshake(adapter, processId, error);
}

bool DapSession::SendRaw(const std::string& bytes) {
    if (!child_) return stream_.Send(bytes);  // TcpStream serializes sends itself
    std::lock_guard<std::mutex> lock(sendMutex_);
    return child_->Write(bytes);
}

int DapSession::ReadRaw(char* buffer, size_t max, int timeoutMs) {
    return child_ ? child_->Read(buffer, max, timeoutMs) : stream_.Read(buffer, max, timeoutMs);
}

void DapSession::CloseRaw() {
    if (child_) child_->Kill();  // after disconnect: the adapter only
    else stream_.Close();
}

bool DapSession::Handshake(const std::string& adapter, uint64_t processId, std::string& error) {
    adapter_ = adapter;
    processId_ = processId;
    connected_ = true;
    stopping_ = false;
    // Data from the debug interface is untrusted: an exception in a reader or
    // event thread ends this session (protocol_error), never the process.
    auto guarded = [this](void (std::remove_pointer_t<decltype(this)>::*loop)()) {
        try {
            (this->*loop)();
            return;
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(mutex_);
            lastError_ = std::string("protocol_error: ") + e.what();
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            lastError_ = "protocol_error";
        }
        stopping_ = true;
        connected_ = false;
        responded_.notify_all();
        queued_.notify_all();
    };
    reader_ = std::thread([guarded, this] { guarded(&std::remove_pointer_t<decltype(this)>::ReadLoop); });
    events_ = std::thread([guarded, this] { guarded(&std::remove_pointer_t<decltype(this)>::EventLoop); });
    const json init = Request("initialize", {{"clientID", "cortex"}, {"clientName", "Cortex"}, {"adapterID", adapter},
                                             {"linesStartAt1", true}, {"columnsStartAt1", true}, {"pathFormat", "path"},
                                             {"supportsVariableType", true}, {"supportsRunInTerminalRequest", false}},
                              60000);  // a first start can be slow (netcoredbg loads its managed part)
    if (!jsafe::Bool(init, "success", false)) {
        error = "dap_initialize_failed: " + jsafe::Str(init, "message");
        Detach();
        return false;
    }
    capabilities_ = jsafe::Obj(init, "body");
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
    if (!jsafe::Bool(attached, "success", false)) {
        // The adapter's own explanation (message, and body.error.format).
        std::string detail = jsafe::Str(attached, "message");
        const json body = jsafe::Obj(attached, "body");
        if (body.is_object() && body.contains("error") && body["error"].is_object())
            detail += " " + jsafe::Str(body["error"], "format");
        error = "dap_attach_failed: " + detail;
        // What the adapter said meanwhile (output events), for the operator.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            std::string said;
            for (const auto& entry : output_) said += jsafe::Str(entry, "text");
            if (!said.empty()) error += " | adapter output: " + Clip(said, 1500);
        }
        Detach();
        return false;
    }
    if (!jsafe::Bool(done, "success", false) && jsafe::Bool(capabilities_, "supportsConfigurationDoneRequest", false)) {
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
    // A socket is closed first to wake the reader; an adapter's pipes are
    // closed only after the reader stopped using them (it polls stopping_).
    if (!child_) CloseRaw();
    responded_.notify_all();
    queued_.notify_all();
    if (reader_.joinable()) reader_.join();
    if (events_.joinable()) events_.join();
    if (child_) CloseRaw();
}

void DapSession::ReadLoop() {
    std::string buffer;
    char chunk[8192];
    while (!stopping_) {
        const int got = ReadRaw(chunk, sizeof chunk, 200);
        if (got == 0) continue;
        if (got < 0) break;
        buffer.append(chunk, static_cast<size_t>(got));
        json message;
        frames::Take taken;
        while ((taken = frames::TakeDapMessage(buffer, message)) == frames::Take::Message) {
            if (!message.is_object()) continue;
            std::lock_guard<std::mutex> lock(mutex_);
            const std::string type = jsafe::Str(message, "type");
            if (type == "response") {
                const int64_t seq = jsafe::Int(message, "request_seq", 0);
                responses_[seq] = std::move(message);
                responded_.notify_all();
            } else if (type == "event") {
                ++eventsSeen_[jsafe::Str(message, "event")];
                responded_.notify_all();
                eventQueue_.push_back(std::move(message));
                queued_.notify_all();
            }
            message = json();
        }
        if (taken == frames::Take::Fatal) {
            std::lock_guard<std::mutex> lock(mutex_);
            lastError_ = "adapter_protocol_error";
            stopping_ = true;
            break;
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
    const int64_t thread = jsafe::Int(body, "threadId", 0);
    const json trace = Request("stackTrace", {{"threadId", thread}, {"startFrame", 0}, {"levels", 32}});
    const json frames = jsafe::Arr(jsafe::Obj(trace, "body"), "stackFrames");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& frame : frames)
            if (frame.is_object() && knownPaths_.size() < 10000) {
                const std::string path = jsafe::Str(jsafe::Obj(frame, "source"), "path");
                if (!path.empty()) knownPaths_.insert(PathKey(path));
            }
    }
    auto frameJson = [](const json& frame) {
        return json{{"function", Clip(jsafe::Str(frame, "name"))},
                    {"path", jsafe::Str(jsafe::Obj(frame, "source"), "path")},
                    {"line", jsafe::Int(frame, "line", 0)},
                    {"column", jsafe::Int(frame, "column", 0)}};
    };
    int ours = 0;
    if (!frames.empty() && frames[0].is_object()) {
        const std::string key = PathKey(jsafe::Str(jsafe::Obj(frames[0], "source"), "path"));
        const int line = jsafe::Int(frames[0], "line", 0);
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& hit : jsafe::Arr(body, "hitBreakpointIds"))
            for (auto& [id, bp] : breakpoints_)
                if (bp.active && bp.dapId >= 0 && hit.is_number_integer() && hit.get<int64_t>() == bp.dapId) ours = id;
        if (!ours)
            for (auto& [id, bp] : breakpoints_)
                if (bp.active && PathKey(bp.path) == key &&
                    (static_cast<int>(bp.resolvedLine ? bp.resolvedLine : bp.line) == line))
                    ours = id;
    }
    if (!ours) {
        json record{{"time", Now()}, {"reason", jsafe::Str(body, "reason")}};
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
            const json scopes = Request("scopes", {{"frameId", jsafe::Int(frames[0], "id", 0)}});
            const json list = jsafe::Arr(jsafe::Obj(scopes, "body"), "scopes");
            if (!list.empty() && list[0].is_object()) {
                const json variables = Request("variables", {{"variablesReference", jsafe::Int(list[0], "variablesReference", 0)}, {"count", kMaxLocals}});
                for (const auto& variable : jsafe::Arr(jsafe::Obj(variables, "body"), "variables")) {
                    if (locals.size() >= kMaxLocals || !variable.is_object()) break;
                    const std::string name = jsafe::Str(variable, "name");
                    if (name.rfind("special variables", 0) == 0 || name.rfind("function variables", 0) == 0) continue;
                    locals[name] = {{"value", Clip(jsafe::Str(variable, "value"))}, {"type", jsafe::Str(variable, "type")}};
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
        const std::string name = jsafe::Str(event, "event");
        const json body = jsafe::Obj(event, "body");
        if (name == "stopped") {
            if (connected_) OnStopped(body);
        } else if (name == "output") {
            const std::string category = jsafe::Str(body, "category");
            if (category == "telemetry") continue;
            std::lock_guard<std::mutex> lock(mutex_);
            output_.push_back({{"time", Now()}, {"type", category}, {"text", Clip(jsafe::Str(body, "output"), 2048)}});
            ++outputTotal_;
            while (output_.size() > kMaxOutput) output_.pop_front();
        } else if (name == "breakpoint") {
            const json changed = jsafe::Obj(body, "breakpoint");
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& [id, bp] : breakpoints_)
                if (bp.dapId >= 0 && jsafe::Int(changed, "id", -2) == bp.dapId) {
                    bp.verified = jsafe::Bool(changed, "verified", bp.verified);
                    if (changed.contains("line")) bp.resolvedLine = jsafe::Int(changed, "line", 0);
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
    const json answered = jsafe::Arr(jsafe::Obj(reply, "body"), "breakpoints");
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < ids.size() && i < answered.size(); ++i) {
        auto& bp = breakpoints_[ids[i]];
        bp.verified = jsafe::Bool(answered[i], "verified", false);
        bp.dapId = jsafe::Int(answered[i], "id", -1);
        bp.resolvedLine = jsafe::Int(answered[i], "line", 0);
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
    if (!jsafe::Bool(reply, "success", false)) {
        std::lock_guard<std::mutex> lock(mutex_);
        breakpoints_.erase(id);
        return {{"ok", false}, {"error", "dap_failed"}, {"message", jsafe::Str(reply, "message")}};
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
    return {{"ok", jsafe::Bool(reply, "success", false)}, {"breakpoint", id}};
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
