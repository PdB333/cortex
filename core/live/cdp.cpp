#include "cdp.h"
#include "json_safe.h"

#include <type_traits>

#include <algorithm>
#include <chrono>
#include <ctime>

namespace cortex::live {
namespace {

constexpr size_t kMaxScripts = 20000;
constexpr size_t kMaxConsole = 2000;
constexpr size_t kMaxRecords = 1000;
constexpr size_t kMaxForeignPauses = 200;
constexpr size_t kMaxText = 512;
constexpr size_t kMaxLocals = 48;

// Cut at a character boundary, and keep only valid UTF-8: the text goes into
// JSON, whose serialization throws on an invalid byte.
std::string Clip(std::string text, size_t size = kMaxText) { return jsafe::Clip(text, size); }

std::string Now() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    char text[40];
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    std::strftime(text, sizeof text, "%Y-%m-%dT%H:%M:%S", &utc);
    char out[48];
    std::snprintf(out, sizeof out, "%s.%03dZ", text, static_cast<int>(ms));
    return out;
}

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// "ws://127.0.0.1:9229/abc" -> "/abc" when the host is loopback and the port
// is the inspector's own.
bool LoopbackPath(const std::string& url, uint16_t port, std::string& path) {
    for (const std::string host : {"127.0.0.1", "localhost", "[::1]"}) {
        const std::string prefix = "ws://" + host + ":" + std::to_string(port) + "/";
        if (url.rfind(prefix, 0) == 0) {
            path = url.substr(prefix.size() - 1);
            return true;
        }
    }
    return false;
}

} // namespace

json ListTargets(uint16_t port) {
    std::string body, error;
    if (!HttpGet(port, "/json/list", body, error))
        return {{"ok", false}, {"error", "inspector_unreachable"}, {"message", error}, {"port", port}};
    json list;
    try {
        list = json::parse(body);
    } catch (const std::exception&) {
        return {{"ok", false}, {"error", "inspector_invalid_answer"}};
    }
    json version = json::object();
    std::string versionBody;
    if (HttpGet(port, "/json/version", versionBody, error)) {
        try {
            version = json::parse(versionBody);
        } catch (...) {
        }
    }
    json targets = json::array();
    for (const auto& row : list.is_array() ? list : json::array()) {
        if (!row.is_object()) continue;
        std::string path;
        const std::string ws = jsafe::Str(row, "webSocketDebuggerUrl");
        targets.push_back({{"id", jsafe::Str(row, "id")},
                           {"type", jsafe::Str(row, "type")},
                           {"title", Clip(jsafe::Str(row, "title"))},
                           {"url", Clip(jsafe::Str(row, "url"), 2048)},
                           {"attachable", LoopbackPath(ws, port, path)}});
    }
    return {{"ok", true},
            {"port", port},
            {"browser", jsafe::Str(version, "Browser")},
            {"protocol", jsafe::Str(version, "Protocol-Version")},
            {"targets", targets}};
}

CdpSession::~CdpSession() { Detach(); }

json CdpSession::Call(const std::string& method, const json& params, int timeoutMs) {
    int64_t id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        id = ++nextId_;
    }
    if (!socket_.SendText(json{{"id", id}, {"method", method}, {"params", params}}.dump()))
        return {{"error", {{"message", "send_failed"}}}};
    std::unique_lock<std::mutex> lock(mutex_);
    if (!responded_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                             [&] { return responses_.count(id) > 0 || !connected_; }))
        return {{"error", {{"message", "timeout"}}}};
    if (!responses_.count(id)) return {{"error", {{"message", "disconnected"}}}};
    json reply = std::move(responses_[id]);
    responses_.erase(id);
    return reply;
}

bool CdpSession::Attach(uint16_t port, const std::string& targetId, std::string& error) {
    const json listed = ListTargets(port);
    if (!jsafe::Bool(listed, "ok", false)) {
        error = jsafe::Str(listed, "error", "inspector_unreachable");
        return false;
    }
    std::string body;
    HttpGet(port, "/json/list", body, error);
    json list = json::parse(body, nullptr, false);
    json chosen;
    for (const auto& row : list.is_array() ? list : json::array()) {
        if (!row.is_object()) continue;
        if (!targetId.empty() ? jsafe::Str(row, "id") == targetId
                              : (jsafe::Str(row, "type") == "node" || jsafe::Str(row, "type") == "page")) {
            chosen = row;
            break;
        }
    }
    if (!chosen.is_object()) {
        error = "target_not_found";
        return false;
    }
    std::string path;
    if (!LoopbackPath(jsafe::Str(chosen, "webSocketDebuggerUrl"), port, path)) {
        error = "target_not_loopback";
        return false;
    }
    if (!socket_.Connect(port, path, error)) return false;
    port_ = port;
    target_ = {{"id", jsafe::Str(chosen, "id")},
               {"type", jsafe::Str(chosen, "type")},
               {"title", Clip(jsafe::Str(chosen, "title"))},
               {"url", Clip(jsafe::Str(chosen, "url"), 2048)}};
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
    for (const auto& [method, params] : std::vector<std::pair<std::string, json>>{
             {"Runtime.enable", json::object()},
             {"Debugger.enable", json::object()},
             {"Debugger.setPauseOnExceptions", {{"state", "none"}}},
             {"Debugger.setBreakpointsActive", {{"active", true}}}}) {
        const json reply = Call(method, params, 10000);
        if (reply.contains("error")) {
            error = "cdp_" + method + "_failed";
            Detach();
            return false;
        }
    }
    return true;
}

void CdpSession::Detach() {
    if (!connected_ && !reader_.joinable()) return;
    // Remove Cortex's breakpoints and let the program run as before.
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [id, bp] : breakpoints_)
            if (bp.active) ids.push_back(bp.cdpId), bp.active = false;
    }
    if (connected_) {
        for (const auto& id : ids) Call("Debugger.removeBreakpoint", {{"breakpointId", id}}, 2000);
        Call("Debugger.disable", json::object(), 2000);
        Call("Runtime.disable", json::object(), 2000);
    }
    stopping_ = true;
    connected_ = false;
    socket_.Close();
    queued_.notify_all();
    responded_.notify_all();
    if (reader_.joinable()) reader_.join();
    if (events_.joinable()) events_.join();
}

void CdpSession::ReadLoop() {
    std::string text;
    while (!stopping_) {
        bool timedOut = false;
        if (!socket_.Receive(text, 200, timedOut)) {
            if (timedOut) continue;
            break;
        }
        json message = json::parse(text, nullptr, false);
        if (!message.is_object()) continue;
        std::lock_guard<std::mutex> lock(mutex_);
        if (message.contains("id") && message["id"].is_number_integer()) {
            const int64_t id = message["id"].get<int64_t>();  // before the move
            responses_[id] = std::move(message);
            responded_.notify_all();
        } else if (message.contains("method")) {
            eventQueue_.push_back(std::move(message));
            queued_.notify_all();
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!stopping_) lastError_ = "inspector_disconnected";
        connected_ = false;
    }
    responded_.notify_all();
    queued_.notify_all();
}

json CdpSession::Describe(const json& remote) const {
    json out{{"type", jsafe::Str(remote, "type")}};
    if (remote.contains("subtype")) out["subtype"] = remote["subtype"];
    if (remote.contains("value")) {
        const json& value = remote["value"];
        out["value"] = value.is_string() ? json(Clip(value.get<std::string>())) : value;
    } else if (remote.contains("unserializableValue")) {
        out["value"] = remote["unserializableValue"];
    } else if (remote.contains("description")) {
        out["description"] = Clip(jsafe::Str(remote, "description"));
    }
    if (remote.contains("preview") && remote["preview"].is_object()) {
        json properties = json::array();
        for (const auto& p : jsafe::Arr(remote["preview"], "properties")) {
            if (properties.size() >= 24) break;
            properties.push_back({{"name", jsafe::Str(p, "name")},
                                  {"type", jsafe::Str(p, "type")},
                                  {"value", Clip(jsafe::Str(p, "value"))}});
        }
        out["preview"] = properties;
        if (jsafe::Bool(remote["preview"], "overflow", false)) out["preview_overflow"] = true;
    }
    return out;
}

std::string CdpSession::UrlOf(const std::string& scriptId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = scripts_.find(scriptId);
    return it == scripts_.end() ? std::string() : jsafe::Str(it->second, "url");
}

json CdpSession::CaptureFrame(const json& frame, bool withLocals) {
    const json location = jsafe::Obj(frame, "location");
    json out{{"function", jsafe::Str(frame, "functionName")},
             {"url", UrlOf(jsafe::Str(location, "scriptId"))},
             {"line", jsafe::Int(location, "lineNumber", 0) + 1},
             {"column", jsafe::Int(location, "columnNumber", 0) + 1}};
    if (!withLocals) return out;
    json locals = json::object();
    for (const auto& scope : jsafe::Arr(frame, "scopeChain")) {
        if (jsafe::Str(scope, "type") != "local") continue;
        const std::string objectId = jsafe::Str(jsafe::Obj(scope, "object"), "objectId");
        if (objectId.empty()) continue;
        // Own properties of the scope object; accessors are listed, not run.
        const json reply = Call("Runtime.getProperties",
                                {{"objectId", objectId}, {"ownProperties", true}, {"generatePreview", true}});
        for (const auto& property : jsafe::Arr(jsafe::Obj(reply, "result"), "result")) {
            if (locals.size() >= kMaxLocals) break;
            if (!property.contains("value")) continue;
            locals[jsafe::Str(property, "name")] = Describe(jsafe::Any(property, "value"));
        }
    }
    out["locals"] = locals;
    if (frame.contains("this")) out["this"] = Describe(frame["this"]);
    return out;
}

void CdpSession::OnPaused(const json& params) {
    ++pauses_;
    std::vector<int> ours;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& hit : jsafe::Arr(params, "hitBreakpoints"))
            for (auto& [id, bp] : breakpoints_)
                if (bp.cdpId == hit.get<std::string>() && bp.active) ours.push_back(id);
    }
    const json frames = jsafe::Arr(params, "callFrames");
    if (ours.empty()) {
        // Not Cortex's pause (a `debugger;` statement, another client):
        // recorded, then resumed so the program behaves as without Cortex.
        json record{{"time", Now()}, {"reason", jsafe::Str(params, "reason")}};
        if (!frames.empty()) record["frame"] = CaptureFrame(frames[0], false);
        std::lock_guard<std::mutex> lock(mutex_);
        foreignPauses_.push_back(record);
        ++foreignPauseTotal_;
        while (foreignPauses_.size() > kMaxForeignPauses) foreignPauses_.pop_front();
    } else {
        for (int id : ours) {
            uint32_t maxFrames = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                maxFrames = breakpoints_[id].maxFrames;
            }
            json stack = json::array();
            for (size_t i = 0; i < frames.size() && i < maxFrames; ++i) stack.push_back(CaptureFrame(frames[i], i == 0));
            bool exhausted = false;
            std::string cdpId;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto& bp = breakpoints_[id];
                ++bp.hits;
                bp.records.push_back({{"hit", bp.hits}, {"time", Now()}, {"frames", stack}});
                while (bp.records.size() > kMaxRecords) bp.records.pop_front(), ++bp.dropped;
                exhausted = bp.hits >= bp.maxHits;
                if (exhausted) bp.active = false, cdpId = bp.cdpId;
            }
            if (exhausted) Call("Debugger.removeBreakpoint", {{"breakpointId", cdpId}});
        }
    }
    Call("Debugger.resume", json::object());
}

void CdpSession::EventLoop() {
    for (;;) {
        json event;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            queued_.wait(lock, [&] { return !eventQueue_.empty() || stopping_ || !connected_; });
            if (eventQueue_.empty()) return;
            event = std::move(eventQueue_.front());
            eventQueue_.pop_front();
        }
        const std::string method = jsafe::Str(event, "method");
        const json params = jsafe::Obj(event, "params");
        if (method == "Debugger.paused") {
            if (connected_) OnPaused(params);
        } else if (method == "Debugger.scriptParsed") {
            std::lock_guard<std::mutex> lock(mutex_);
            if (scripts_.size() < kMaxScripts)
                scripts_[jsafe::Str(params, "scriptId")] = {
                    {"script_id", jsafe::Str(params, "scriptId")},
                    {"url", jsafe::Str(params, "url")},
                    {"lines", jsafe::Int(params, "endLine", 0) + 1},
                    {"hash", jsafe::Str(params, "hash")},
                    {"source_map_url", Clip(jsafe::Str(params, "sourceMapURL"), 2048)}};
        } else if (method == "Debugger.breakpointResolved") {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& [id, bp] : breakpoints_)
                if (bp.cdpId == jsafe::Str(params, "breakpointId")) {
                    const json location = jsafe::Obj(params, "location");
                    bp.locations.push_back({{"line", jsafe::Int(location, "lineNumber", 0) + 1},
                                            {"column", jsafe::Int(location, "columnNumber", 0) + 1}});
                }
        } else if (method == "Runtime.consoleAPICalled" || method == "Runtime.exceptionThrown") {
            json entry{{"time", Now()}};
            if (method == "Runtime.consoleAPICalled") {
                entry["type"] = jsafe::Str(params, "type");
                std::string text;
                for (const auto& argument : jsafe::Arr(params, "args")) {
                    const json d = Describe(argument);
                    text += (text.empty() ? "" : " ") +
                            (d.contains("value") ? (d["value"].is_string() ? d["value"].get<std::string>() : d["value"].dump())
                                                 : jsafe::Str(d, "description", jsafe::Str(d, "type")));
                }
                entry["text"] = Clip(text, 2048);
                const auto frames = jsafe::Arr(jsafe::Obj(params, "stackTrace"), "callFrames");
                if (!frames.empty())
                    entry["at"] = jsafe::Str(frames[0], "url") + ":" + std::to_string(jsafe::Int(frames[0], "lineNumber", 0) + 1);
            } else {
                const json details = jsafe::Obj(params, "exceptionDetails");
                entry["type"] = "exception";
                entry["text"] = Clip(jsafe::Str(jsafe::Obj(details, "exception"), "description", jsafe::Str(details, "text")), 2048);
                entry["at"] = jsafe::Str(details, "url") + ":" + std::to_string(jsafe::Int(details, "lineNumber", 0) + 1);
            }
            std::lock_guard<std::mutex> lock(mutex_);
            console_.push_back(entry);
            ++consoleTotal_;
            while (console_.size() > kMaxConsole) console_.pop_front();
        }
    }
}

json CdpSession::Status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t active = 0;
    for (const auto& [id, bp] : breakpoints_) active += bp.active ? 1 : 0;
    json foreign = json::array();
    for (const auto& pause : foreignPauses_) foreign.push_back(pause);
    return {{"ok", true},
            {"protocol", "cdp"},
            {"connected", connected_.load()},
            {"port", port_},
            {"target", target_},
            {"scripts", scripts_.size()},
            {"breakpoints_active", active},
            {"pauses", pauses_},
            {"console_messages", consoleTotal_},
            {"foreign_pauses", foreignPauseTotal_},
            {"foreign_pause_records", foreign},
            {"disconnect_reason", lastError_}};
}

json CdpSession::Scripts(const std::string& query, size_t offset, size_t limit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string needle = Lower(query);
    json rows = json::array();
    size_t total = 0;
    for (const auto& [id, script] : scripts_) {
        if (!needle.empty() && Lower(jsafe::Str(script, "url")).find(needle) == std::string::npos) continue;
        if (total >= offset && rows.size() < limit) rows.push_back(script);
        ++total;
    }
    return {{"ok", true}, {"total", total}, {"offset", offset}, {"scripts", rows}};
}

json CdpSession::Source(const std::string& key, size_t firstLine, size_t lineCount) {
    std::string scriptId;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (scripts_.count(key)) scriptId = key;
        else
            for (const auto& [id, script] : scripts_)
                if (jsafe::Str(script, "url") == key) scriptId = id;
    }
    if (scriptId.empty()) return {{"ok", false}, {"error", "script_not_found"}};
    const json reply = Call("Debugger.getScriptSource", {{"scriptId", scriptId}}, 10000);
    if (reply.contains("error")) return {{"ok", false}, {"error", "cdp_failed"}, {"message", jsafe::Str(reply["error"], "message")}};
    const std::string source = jsafe::Str(jsafe::Obj(reply, "result"), "scriptSource");
    std::string text;
    size_t line = 1, total = 1;
    for (size_t i = 0; i < source.size(); ++i) {
        if (line >= firstLine && line < firstLine + lineCount) text.push_back(source[i]);
        if (source[i] == '\n') ++line, ++total;
    }
    return {{"ok", true},           {"script_id", scriptId}, {"url", UrlOf(scriptId)}, {"first_line", firstLine},
            {"total_lines", total}, {"bytes", source.size()}, {"text", text}};
}

json CdpSession::SetLogBreakpoint(const std::string& url, uint32_t line, uint32_t column, uint32_t maxHits, uint32_t maxFrames) {
    if (!connected_) return {{"ok", false}, {"error", "not_connected"}};
    json params{{"url", url}, {"lineNumber", line - 1}};
    if (column) params["columnNumber"] = column - 1;
    // No "condition": CDP evaluates it inside the target.
    const json reply = Call("Debugger.setBreakpointByUrl", params, 10000);
    if (reply.contains("error"))
        return {{"ok", false}, {"error", "cdp_failed"}, {"message", jsafe::Str(reply["error"], "message")}};
    const json result = jsafe::Obj(reply, "result");
    std::lock_guard<std::mutex> lock(mutex_);
    Breakpoint bp;
    bp.id = ++nextBreakpoint_;
    bp.cdpId = jsafe::Str(result, "breakpointId");
    bp.url = url;
    bp.line = line;
    bp.column = column;
    bp.maxHits = maxHits;
    bp.maxFrames = maxFrames;
    for (const auto& location : jsafe::Arr(result, "locations"))
        bp.locations.push_back({{"line", jsafe::Int(location, "lineNumber", 0) + 1}, {"column", jsafe::Int(location, "columnNumber", 0) + 1}});
    const json locations = bp.locations;
    breakpoints_[bp.id] = std::move(bp);
    return {{"ok", true}, {"breakpoint", nextBreakpoint_}, {"resolved", locations},
            {"action", "log"}, {"note", "captures the stack and locals, then resumes; removed after max_hits"}};
}

json CdpSession::RemoveBreakpoint(int id) {
    std::string cdpId;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = breakpoints_.find(id);
        if (it == breakpoints_.end()) return {{"ok", false}, {"error", "unknown_breakpoint"}};
        if (!it->second.active) return {{"ok", true}, {"breakpoint", id}, {"already_inactive", true}};
        it->second.active = false;
        cdpId = it->second.cdpId;
    }
    const json reply = Call("Debugger.removeBreakpoint", {{"breakpointId", cdpId}});
    return {{"ok", !reply.contains("error")}, {"breakpoint", id}};
}

json CdpSession::Breakpoints() const {
    std::lock_guard<std::mutex> lock(mutex_);
    json rows = json::array();
    for (const auto& [id, bp] : breakpoints_)
        rows.push_back({{"breakpoint", id}, {"url", bp.url}, {"line", bp.line}, {"column", bp.column}, {"active", bp.active},
                        {"hits", bp.hits}, {"max_hits", bp.maxHits}, {"resolved", bp.locations}, {"dropped", bp.dropped}});
    return {{"ok", true}, {"breakpoints", rows}};
}

json CdpSession::Hits(int id, size_t offset, size_t limit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = breakpoints_.find(id);
    if (it == breakpoints_.end()) return {{"ok", false}, {"error", "unknown_breakpoint"}};
    json rows = json::array();
    for (size_t i = offset; i < it->second.records.size() && rows.size() < limit; ++i) rows.push_back(it->second.records[i]);
    return {{"ok", true}, {"breakpoint", id}, {"hits", it->second.hits}, {"active", it->second.active},
            {"dropped", it->second.dropped}, {"records", rows}};
}

json CdpSession::Console(size_t offset, size_t limit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    json rows = json::array();
    const uint64_t first = consoleTotal_ - console_.size();  // index of console_[0]
    for (size_t i = 0; i < console_.size() && rows.size() < limit; ++i)
        if (first + i >= offset) {
            json entry = console_[i];
            entry["index"] = first + i;
            rows.push_back(std::move(entry));
        }
    return {{"ok", true}, {"total", consoleTotal_}, {"oldest_kept", first}, {"messages", rows}};
}

} // namespace cortex::live
