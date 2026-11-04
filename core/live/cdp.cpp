#include "cdp.h"

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

std::string Clip(std::string text, size_t size = kMaxText) {
    if (text.size() > size) text = text.substr(0, size) + "...";
    return text;
}

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
        const std::string ws = row.value("webSocketDebuggerUrl", std::string());
        targets.push_back({{"id", row.value("id", std::string())},
                           {"type", row.value("type", std::string())},
                           {"title", Clip(row.value("title", std::string()))},
                           {"url", Clip(row.value("url", std::string()), 2048)},
                           {"attachable", LoopbackPath(ws, port, path)}});
    }
    return {{"ok", true},
            {"port", port},
            {"browser", version.value("Browser", std::string())},
            {"protocol", version.value("Protocol-Version", std::string())},
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
    if (!listed.value("ok", false)) {
        error = listed.value("error", std::string("inspector_unreachable"));
        return false;
    }
    std::string body;
    HttpGet(port, "/json/list", body, error);
    json list = json::parse(body, nullptr, false);
    json chosen;
    for (const auto& row : list.is_array() ? list : json::array()) {
        if (!row.is_object()) continue;
        if (!targetId.empty() ? row.value("id", std::string()) == targetId
                              : (row.value("type", std::string()) == "node" || row.value("type", std::string()) == "page")) {
            chosen = row;
            break;
        }
    }
    if (!chosen.is_object()) {
        error = "target_not_found";
        return false;
    }
    std::string path;
    if (!LoopbackPath(chosen.value("webSocketDebuggerUrl", std::string()), port, path)) {
        error = "target_not_loopback";
        return false;
    }
    if (!socket_.Connect(port, path, error)) return false;
    port_ = port;
    target_ = {{"id", chosen.value("id", std::string())},
               {"type", chosen.value("type", std::string())},
               {"title", Clip(chosen.value("title", std::string()))},
               {"url", Clip(chosen.value("url", std::string()), 2048)}};
    connected_ = true;
    stopping_ = false;
    reader_ = std::thread([this] { ReadLoop(); });
    events_ = std::thread([this] { EventLoop(); });
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
    json out{{"type", remote.value("type", std::string())}};
    if (remote.contains("subtype")) out["subtype"] = remote["subtype"];
    if (remote.contains("value")) {
        const json& value = remote["value"];
        out["value"] = value.is_string() ? json(Clip(value.get<std::string>())) : value;
    } else if (remote.contains("unserializableValue")) {
        out["value"] = remote["unserializableValue"];
    } else if (remote.contains("description")) {
        out["description"] = Clip(remote.value("description", std::string()));
    }
    if (remote.contains("preview") && remote["preview"].is_object()) {
        json properties = json::array();
        for (const auto& p : remote["preview"].value("properties", json::array())) {
            if (properties.size() >= 24) break;
            properties.push_back({{"name", p.value("name", std::string())},
                                  {"type", p.value("type", std::string())},
                                  {"value", Clip(p.value("value", std::string()))}});
        }
        out["preview"] = properties;
        if (remote["preview"].value("overflow", false)) out["preview_overflow"] = true;
    }
    return out;
}

std::string CdpSession::UrlOf(const std::string& scriptId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = scripts_.find(scriptId);
    return it == scripts_.end() ? std::string() : it->second.value("url", std::string());
}

json CdpSession::CaptureFrame(const json& frame, bool withLocals) {
    const json location = frame.value("location", json::object());
    json out{{"function", frame.value("functionName", std::string())},
             {"url", UrlOf(location.value("scriptId", std::string()))},
             {"line", location.value("lineNumber", 0) + 1},
             {"column", location.value("columnNumber", 0) + 1}};
    if (!withLocals) return out;
    json locals = json::object();
    for (const auto& scope : frame.value("scopeChain", json::array())) {
        if (scope.value("type", std::string()) != "local") continue;
        const std::string objectId = scope.value("object", json::object()).value("objectId", std::string());
        if (objectId.empty()) continue;
        // Own properties of the scope object; accessors are listed, not run.
        const json reply = Call("Runtime.getProperties",
                                {{"objectId", objectId}, {"ownProperties", true}, {"generatePreview", true}});
        for (const auto& property : reply.value("result", json::object()).value("result", json::array())) {
            if (locals.size() >= kMaxLocals) break;
            if (!property.contains("value")) continue;
            locals[property.value("name", std::string())] = Describe(property["value"]);
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
        for (const auto& hit : params.value("hitBreakpoints", json::array()))
            for (auto& [id, bp] : breakpoints_)
                if (bp.cdpId == hit.get<std::string>() && bp.active) ours.push_back(id);
    }
    const json frames = params.value("callFrames", json::array());
    if (ours.empty()) {
        // Not Cortex's pause (a `debugger;` statement, another client):
        // recorded, then resumed so the program behaves as without Cortex.
        json record{{"time", Now()}, {"reason", params.value("reason", std::string())}};
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
        const std::string method = event.value("method", std::string());
        const json params = event.value("params", json::object());
        if (method == "Debugger.paused") {
            if (connected_) OnPaused(params);
        } else if (method == "Debugger.scriptParsed") {
            std::lock_guard<std::mutex> lock(mutex_);
            if (scripts_.size() < kMaxScripts)
                scripts_[params.value("scriptId", std::string())] = {
                    {"script_id", params.value("scriptId", std::string())},
                    {"url", params.value("url", std::string())},
                    {"lines", params.value("endLine", 0) + 1},
                    {"hash", params.value("hash", std::string())},
                    {"source_map_url", Clip(params.value("sourceMapURL", std::string()), 2048)}};
        } else if (method == "Debugger.breakpointResolved") {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& [id, bp] : breakpoints_)
                if (bp.cdpId == params.value("breakpointId", std::string())) {
                    const json location = params.value("location", json::object());
                    bp.locations.push_back({{"line", location.value("lineNumber", 0) + 1},
                                            {"column", location.value("columnNumber", 0) + 1}});
                }
        } else if (method == "Runtime.consoleAPICalled" || method == "Runtime.exceptionThrown") {
            json entry{{"time", Now()}};
            if (method == "Runtime.consoleAPICalled") {
                entry["type"] = params.value("type", std::string());
                std::string text;
                for (const auto& argument : params.value("args", json::array())) {
                    const json d = Describe(argument);
                    text += (text.empty() ? "" : " ") +
                            (d.contains("value") ? (d["value"].is_string() ? d["value"].get<std::string>() : d["value"].dump())
                                                 : d.value("description", d.value("type", std::string())));
                }
                entry["text"] = Clip(text, 2048);
                const auto frames = params.value("stackTrace", json::object()).value("callFrames", json::array());
                if (!frames.empty())
                    entry["at"] = frames[0].value("url", std::string()) + ":" + std::to_string(frames[0].value("lineNumber", 0) + 1);
            } else {
                const json details = params.value("exceptionDetails", json::object());
                entry["type"] = "exception";
                entry["text"] = Clip(details.value("exception", json::object()).value("description", details.value("text", std::string())), 2048);
                entry["at"] = details.value("url", std::string()) + ":" + std::to_string(details.value("lineNumber", 0) + 1);
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
        if (!needle.empty() && Lower(script.value("url", std::string())).find(needle) == std::string::npos) continue;
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
                if (script.value("url", std::string()) == key) scriptId = id;
    }
    if (scriptId.empty()) return {{"ok", false}, {"error", "script_not_found"}};
    const json reply = Call("Debugger.getScriptSource", {{"scriptId", scriptId}}, 10000);
    if (reply.contains("error")) return {{"ok", false}, {"error", "cdp_failed"}, {"message", reply["error"].value("message", std::string())}};
    const std::string source = reply.value("result", json::object()).value("scriptSource", std::string());
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
        return {{"ok", false}, {"error", "cdp_failed"}, {"message", reply["error"].value("message", std::string())}};
    const json result = reply.value("result", json::object());
    std::lock_guard<std::mutex> lock(mutex_);
    Breakpoint bp;
    bp.id = ++nextBreakpoint_;
    bp.cdpId = result.value("breakpointId", std::string());
    bp.url = url;
    bp.line = line;
    bp.column = column;
    bp.maxHits = maxHits;
    bp.maxFrames = maxFrames;
    for (const auto& location : result.value("locations", json::array()))
        bp.locations.push_back({{"line", location.value("lineNumber", 0) + 1}, {"column", location.value("columnNumber", 0) + 1}});
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
