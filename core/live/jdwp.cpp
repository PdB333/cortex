#include "jdwp.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>

namespace cortex::live {
namespace {

constexpr size_t kMaxRecords = 1000;
constexpr size_t kMaxLocals = 48;
constexpr uint8_t kEventBreakpoint = 2;
constexpr uint8_t kSuspendEventThread = 1;

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

void Put32(JdwpSession::Bytes& out, uint32_t v) {
    for (int i = 3; i >= 0; --i) out.push_back(static_cast<uint8_t>(v >> (i * 8)));
}
void Put64(JdwpSession::Bytes& out, uint64_t v) {
    for (int i = 7; i >= 0; --i) out.push_back(static_cast<uint8_t>(v >> (i * 8)));
}
void PutString(JdwpSession::Bytes& out, const std::string& s) {
    Put32(out, static_cast<uint32_t>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
}

std::string Clip(std::string text, size_t size = 512) {
    if (text.size() > size) text = text.substr(0, size) + "...";
    return text;
}

// "Lcortex/fixture/LiveApp;" -> "cortex.fixture.LiveApp"
std::string ClassName(const std::string& signature) {
    if (signature.size() < 3 || signature.front() != 'L' || signature.back() != ';') return signature;
    std::string name = signature.substr(1, signature.size() - 2);
    std::replace(name.begin(), name.end(), '/', '.');
    return name;
}

} // namespace

// Big-endian reader over a reply or event; `ok` turns false on underflow.
struct JdwpSession::Reader {
    const Bytes& data;
    size_t pos = 0;
    bool ok = true;
    explicit Reader(const Bytes& d) : data(d) {}
    uint64_t Read(int size) {
        if (pos + static_cast<size_t>(size) > data.size()) {
            ok = false;
            pos = data.size();
            return 0;
        }
        uint64_t v = 0;
        for (int i = 0; i < size; ++i) v = (v << 8) | data[pos++];
        return v;
    }
    uint8_t U8() { return static_cast<uint8_t>(Read(1)); }
    uint32_t U32() { return static_cast<uint32_t>(Read(4)); }
    int32_t I32() { return static_cast<int32_t>(Read(4)); }
    uint64_t U64() { return Read(8); }
    std::string Str() {
        const uint32_t n = U32();
        if (!ok || pos + n > data.size() || n > (16u << 20)) {
            ok = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(data.data() + pos), n);
        pos += n;
        return s;
    }
};

JdwpSession::~JdwpSession() { Detach(); }

void JdwpSession::PutId(Bytes& out, uint64_t value, int size) const {
    for (int i = size - 1; i >= 0; --i) out.push_back(static_cast<uint8_t>(value >> (i * 8)));
}

JdwpSession::Reply JdwpSession::Command(uint8_t set, uint8_t command, const Bytes& data, int timeoutMs) {
    uint32_t id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        id = ++nextId_;
    }
    Bytes packet;
    Put32(packet, static_cast<uint32_t>(11 + data.size()));
    Put32(packet, id);
    packet.push_back(0);
    packet.push_back(set);
    packet.push_back(command);
    packet.insert(packet.end(), data.begin(), data.end());
    if (!stream_.Send(std::string(packet.begin(), packet.end()))) return {};
    std::unique_lock<std::mutex> lock(mutex_);
    if (!responded_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return replies_.count(id) > 0 || stopping_; })) return {};
    if (!replies_.count(id)) return {};
    Reply reply = std::move(replies_[id]);
    replies_.erase(id);
    return reply;
}

bool JdwpSession::Attach(uint16_t port, std::string& error) {
    if (!stream_.Connect(port, error)) return false;
    // Handshake: both sides send the same 14 bytes.
    const std::string hello = "JDWP-Handshake";
    if (!stream_.Send(hello)) {
        error = "jdwp_handshake_failed";
        return false;
    }
    std::string answer;
    char buffer[64];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (answer.size() < hello.size() && std::chrono::steady_clock::now() < deadline) {
        const int got = stream_.Read(buffer, hello.size() - answer.size(), 200);
        if (got < 0) break;
        answer.append(buffer, static_cast<size_t>(std::max(0, got)));
    }
    if (answer != hello) {
        stream_.Close();
        error = "jdwp_handshake_rejected";
        return false;
    }
    port_ = port;
    connected_ = true;
    stopping_ = false;
    reader_ = std::thread([this] { ReadLoop(); });
    events_ = std::thread([this] { EventLoop(); });
    const Reply version = Command(1, 1, {});
    const Reply sizes = Command(1, 7, {});
    if (!version.ok || !sizes.ok) {
        error = "jdwp_version_failed";
        Detach();
        return false;
    }
    Reader v(version.data);
    vm_["description"] = Clip(v.Str(), 1024);
    const int32_t major = v.I32();  // two statements: evaluation order
    const int32_t minor = v.I32();
    vm_["jdwp"] = std::to_string(major) + "." + std::to_string(minor);
    vm_["vm_version"] = v.Str();
    vm_["vm_name"] = v.Str();
    Reader s(sizes.data);
    fieldIdSize_ = s.I32();
    methodIdSize_ = s.I32();
    objectIdSize_ = s.I32();
    referenceTypeIdSize_ = s.I32();
    frameIdSize_ = s.I32();
    for (int size : {fieldIdSize_, methodIdSize_, objectIdSize_, referenceTypeIdSize_, frameIdSize_})
        if (size < 1 || size > 8) {
            error = "jdwp_bad_id_sizes";
            Detach();
            return false;
        }
    return true;
}

void JdwpSession::Detach() {
    if (!connected_ && !reader_.joinable()) return;
    if (connected_) {
        // Dispose removes every request of this connection and resumes the VM.
        Command(1, 6, {}, 3000);
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [id, bp] : breakpoints_) bp.active = false;
    }
    stopping_ = true;
    connected_ = false;
    stream_.Close();
    responded_.notify_all();
    queued_.notify_all();
    if (reader_.joinable()) reader_.join();
    if (events_.joinable()) events_.join();
}

void JdwpSession::ReadLoop() {
    Bytes buffer;
    char chunk[8192];
    while (!stopping_) {
        const int got = stream_.Read(chunk, sizeof chunk, 200);
        if (got == 0) continue;
        if (got < 0) break;
        buffer.insert(buffer.end(), chunk, chunk + got);
        while (buffer.size() >= 11) {
            const uint32_t length = (uint32_t{buffer[0]} << 24) | (uint32_t{buffer[1]} << 16) | (uint32_t{buffer[2]} << 8) | buffer[3];
            if (length < 11 || length > (64u << 20)) {
                buffer.clear();
                stopping_ = true;
                break;
            }
            if (buffer.size() < length) break;
            const uint32_t id = (uint32_t{buffer[4]} << 24) | (uint32_t{buffer[5]} << 16) | (uint32_t{buffer[6]} << 8) | buffer[7];
            const uint8_t flags = buffer[8];
            std::lock_guard<std::mutex> lock(mutex_);
            if (flags & 0x80) {
                Reply reply;
                reply.error = static_cast<uint16_t>((buffer[9] << 8) | buffer[10]);
                reply.ok = reply.error == 0;
                reply.data.assign(buffer.begin() + 11, buffer.begin() + length);
                replies_[id] = std::move(reply);
                responded_.notify_all();
            } else if (buffer[9] == 64 && buffer[10] == 100) {  // Event.Composite
                eventQueue_.emplace_back(buffer.begin() + 11, buffer.begin() + length);
                queued_.notify_all();
            }
            buffer.erase(buffer.begin(), buffer.begin() + length);
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!stopping_) lastError_ = "vm_disconnected";
        connected_ = false;
    }
    responded_.notify_all();
    queued_.notify_all();
}

void JdwpSession::EventLoop() {
    for (;;) {
        Bytes event;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            queued_.wait(lock, [&] { return !eventQueue_.empty() || stopping_ || !connected_; });
            if (eventQueue_.empty()) return;
            event = std::move(eventQueue_.front());
            eventQueue_.pop_front();
            ++events_total_;
        }
        Reader r(event);
        r.U8();  // suspend policy
        const int32_t count = r.I32();
        for (int32_t i = 0; i < count && r.ok; ++i) {
            const uint8_t kind = r.U8();
            const int32_t request = r.I32();
            if (kind != kEventBreakpoint) break;  // only Cortex's breakpoint requests exist on this connection
            const uint64_t thread = r.Read(objectIdSize_);
            r.U8();
            r.Read(referenceTypeIdSize_);
            r.Read(methodIdSize_);
            r.U64();
            if (r.ok && connected_) OnBreakpoint(request, thread);
        }
    }
}

std::string JdwpSession::ClassSignature(uint64_t typeId) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = signatures_.find(typeId);
        if (it != signatures_.end()) return it->second;
    }
    Bytes data;
    PutId(data, typeId, referenceTypeIdSize_);
    const Reply reply = Command(2, 1, data);
    Reader r(reply.data);
    const std::string signature = reply.ok ? r.Str() : "?";
    std::lock_guard<std::mutex> lock(mutex_);
    signatures_[typeId] = signature;
    return signature;
}

json JdwpSession::MethodInfo(uint64_t typeId, uint64_t methodId) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = methods_.find({typeId, methodId});
        if (it != methods_.end()) return it->second;
    }
    Bytes data;
    PutId(data, typeId, referenceTypeIdSize_);
    const Reply reply = Command(2, 5, data);  // ReferenceType.Methods
    Reader r(reply.data);
    const int32_t count = reply.ok ? r.I32() : 0;
    std::lock_guard<std::mutex> lock(mutex_);
    for (int32_t i = 0; i < count && r.ok && i < 100000; ++i) {
        const uint64_t id = r.Read(methodIdSize_);
        const std::string name = r.Str(), signature = r.Str();
        r.I32();
        methods_[{typeId, id}] = {{"name", name}, {"signature", signature}};
    }
    const auto it = methods_.find({typeId, methodId});
    return it == methods_.end() ? json{{"name", "?"}, {"signature", ""}} : it->second;
}

json JdwpSession::Value(Reader& r, uint8_t tag) {
    switch (tag) {
        case 'Z': return {{"type", "boolean"}, {"value", r.U8() ? "true" : "false"}};
        case 'B': return {{"type", "byte"}, {"value", std::to_string(static_cast<int8_t>(r.U8()))}};
        case 'C': return {{"type", "char"}, {"value", std::to_string(static_cast<uint16_t>(r.Read(2)))}};
        case 'S': return {{"type", "short"}, {"value", std::to_string(static_cast<int16_t>(r.Read(2)))}};
        case 'I': return {{"type", "int"}, {"value", std::to_string(r.I32())}};
        case 'J': return {{"type", "long"}, {"value", std::to_string(static_cast<int64_t>(r.U64()))}};
        case 'F': {
            const uint32_t bits = r.U32();
            float f;
            std::memcpy(&f, &bits, 4);
            return {{"type", "float"}, {"value", std::to_string(f)}};
        }
        case 'D': {
            const uint64_t bits = r.U64();
            double d;
            std::memcpy(&d, &bits, 8);
            return {{"type", "double"}, {"value", std::to_string(d)}};
        }
        case 'V': return {{"type", "void"}, {"value", ""}};
        default: break;
    }
    const uint64_t object = r.Read(objectIdSize_);
    if (!object) return {{"type", "null"}, {"value", "null"}};
    if (tag == 's') {
        // StringReference.Value reads the characters; no Java code runs.
        Bytes data;
        PutId(data, object, objectIdSize_);
        const Reply reply = Command(10, 1, data);
        Reader s(reply.data);
        return {{"type", "java.lang.String"}, {"value", reply.ok ? Clip(s.Str()) : "?"}};
    }
    // Any other object: its runtime type only (ObjectReference.ReferenceType).
    Bytes data;
    PutId(data, object, objectIdSize_);
    const Reply reply = Command(9, 1, data);
    Reader t(reply.data);
    std::string type = "?";
    if (reply.ok) {
        t.U8();
        type = ClassName(ClassSignature(t.Read(referenceTypeIdSize_)));
    }
    return {{"type", type}, {"value", "{" + type + "}"}};
}

void JdwpSession::OnBreakpoint(int32_t requestId, uint64_t thread) {
    int ours = 0;
    uint32_t maxFrames = 8;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [id, bp] : breakpoints_)
            if (bp.requestId == requestId && bp.active) ours = id, maxFrames = bp.maxFrames;
    }
    if (ours) {
        // Frames refuses a length beyond the thread's depth (INVALID_LENGTH):
        // ask FrameCount first.
        Bytes countRequest;
        PutId(countRequest, thread, objectIdSize_);
        const Reply depth = Command(11, 7, countRequest);  // ThreadReference.FrameCount
        Reader dr(depth.data);
        const int32_t available = depth.ok ? dr.I32() : 0;
        Bytes framesRequest;
        PutId(framesRequest, thread, objectIdSize_);
        Put32(framesRequest, 0);
        Put32(framesRequest, static_cast<uint32_t>(std::max(0, std::min<int32_t>(available, static_cast<int32_t>(maxFrames)))));
        const Reply frames = Command(11, 6, framesRequest);  // ThreadReference.Frames
        if (!frames.ok) {
            std::lock_guard<std::mutex> lock(mutex_);
            lastError_ = "frames_error_" + std::to_string(frames.error);
        }
        Reader r(frames.data);
        const int32_t count = frames.ok ? r.I32() : 0;
        json stack = json::array();
        uint64_t topFrame = 0, topType = 0, topMethod = 0, topIndex = 0;
        for (int32_t i = 0; i < count && r.ok; ++i) {
            const uint64_t frame = r.Read(frameIdSize_);
            r.U8();
            const uint64_t type = r.Read(referenceTypeIdSize_);
            const uint64_t method = r.Read(methodIdSize_);
            const uint64_t index = r.U64();
            if (i == 0) topFrame = frame, topType = type, topMethod = method, topIndex = index;
            const json info = MethodInfo(type, method);
            stack.push_back({{"function", ClassName(ClassSignature(type)) + "." + info.value("name", std::string("?"))},
                             {"class", ClassName(ClassSignature(type))},
                             {"code_index", index}});
        }
        if (!stack.empty()) {
            // Line of the top frame from the method's line table.
            Bytes lineRequest;
            PutId(lineRequest, topType, referenceTypeIdSize_);
            PutId(lineRequest, topMethod, methodIdSize_);
            const Reply table = Command(6, 1, lineRequest);  // Method.LineTable
            Reader lt(table.data);
            if (table.ok) {
                lt.U64();
                lt.U64();
                const int32_t lines = lt.I32();
                int32_t best = -1;
                uint64_t bestIndex = 0;
                for (int32_t i = 0; i < lines && lt.ok; ++i) {
                    const uint64_t at = lt.U64();
                    const int32_t number = lt.I32();
                    if (at <= topIndex && (best < 0 || at >= bestIndex)) best = number, bestIndex = at;
                }
                if (best > 0) stack[0]["line"] = best;
            }
            // Locals in scope at the top frame's code index.
            const Reply vars = Command(6, 2, lineRequest);  // Method.VariableTable (needs javac -g)
            json locals = json::object();
            if (vars.ok) {
                Reader vt(vars.data);
                vt.I32();
                const int32_t slots = vt.I32();
                std::vector<std::pair<int32_t, std::string>> wanted;
                std::vector<std::string> names;
                for (int32_t i = 0; i < slots && vt.ok; ++i) {
                    const uint64_t start = vt.U64();
                    const std::string name = vt.Str(), signature = vt.Str();
                    const uint32_t length = vt.U32();
                    const int32_t slot = vt.I32();
                    if (topIndex >= start && topIndex < start + length && !signature.empty() && wanted.size() < kMaxLocals) {
                        wanted.push_back({slot, signature});
                        names.push_back(name);
                    }
                }
                Bytes valuesRequest;
                PutId(valuesRequest, thread, objectIdSize_);
                PutId(valuesRequest, topFrame, frameIdSize_);
                Put32(valuesRequest, static_cast<uint32_t>(wanted.size()));
                for (const auto& [slot, signature] : wanted) {
                    Put32(valuesRequest, static_cast<uint32_t>(slot));
                    valuesRequest.push_back(static_cast<uint8_t>(signature[0]));
                }
                const Reply values = Command(16, 1, valuesRequest);  // StackFrame.GetValues
                Reader gv(values.data);
                const int32_t got = values.ok ? gv.I32() : 0;
                for (int32_t i = 0; i < got && gv.ok && i < static_cast<int32_t>(names.size()); ++i) {
                    const uint8_t tag = gv.U8();
                    locals[names[static_cast<size_t>(i)]] = Value(gv, tag);
                }
            }
            stack[0]["locals"] = locals;
        }
        bool exhausted = false;
        int32_t clearId = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto& bp = breakpoints_[ours];
            ++bp.hits;
            bp.records.push_back({{"hit", bp.hits}, {"time", Now()}, {"thread", thread}, {"frames", stack}});
            while (bp.records.size() > kMaxRecords) bp.records.pop_front(), ++bp.dropped;
            exhausted = bp.hits >= bp.maxHits;
            if (exhausted) bp.active = false, clearId = bp.requestId;
        }
        if (exhausted) {
            Bytes clear{kEventBreakpoint};
            Put32(clear, static_cast<uint32_t>(clearId));
            Command(15, 2, clear);
        }
    }
    // The event suspended this thread only (EVENT_THREAD): resume it.
    Bytes resume;
    PutId(resume, thread, objectIdSize_);
    Command(11, 3, resume);
}

json JdwpSession::SetLogBreakpoint(const std::string& className, uint32_t line, uint32_t, uint32_t maxHits, uint32_t maxFrames) {
    if (!connected_) return {{"ok", false}, {"error", "not_connected"}};
    std::string signature = "L" + className + ";";
    std::replace(signature.begin() + 1, signature.end() - 1, '.', '/');
    Bytes query;
    PutString(query, signature);
    const Reply classes = Command(1, 2, query);  // VirtualMachine.ClassesBySignature
    Reader r(classes.data);
    const int32_t count = classes.ok ? r.I32() : 0;
    if (count < 1) return {{"ok", false}, {"error", "class_not_loaded"}, {"message", "no loaded class " + className}};
    r.U8();
    const uint64_t type = r.Read(referenceTypeIdSize_);
    MethodInfo(type, 0);  // fills the cache with every method of the class
    std::vector<uint64_t> candidates;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [key, info] : methods_)
            if (key.first == type) candidates.push_back(key.second);
    }
    // The first method whose line table has the line: its lowest code index.
    for (uint64_t method : candidates) {
        Bytes request;
        PutId(request, type, referenceTypeIdSize_);
        PutId(request, method, methodIdSize_);
        const Reply table = Command(6, 1, request);
        if (!table.ok) continue;
        Reader lt(table.data);
        lt.U64();
        lt.U64();
        const int32_t lines = lt.I32();
        bool found = false;
        uint64_t index = 0;
        for (int32_t i = 0; i < lines && lt.ok; ++i) {
            const uint64_t at = lt.U64();
            const int32_t number = lt.I32();
            if (number == static_cast<int32_t>(line) && (!found || at < index)) found = true, index = at;
        }
        if (!found) continue;
        Bytes set{kEventBreakpoint, kSuspendEventThread};
        Put32(set, 1);
        set.push_back(7);  // modifier LocationOnly
        set.push_back(1);  // type tag CLASS
        PutId(set, type, referenceTypeIdSize_);
        PutId(set, method, methodIdSize_);
        Put64(set, index);
        const Reply created = Command(15, 1, set);  // EventRequest.Set
        if (!created.ok) return {{"ok", false}, {"error", "jdwp_error_" + std::to_string(created.error)}};
        Reader cr(created.data);
        const json info = MethodInfo(type, method);
        std::lock_guard<std::mutex> lock(mutex_);
        Breakpoint bp;
        bp.id = ++nextBreakpoint_;
        bp.requestId = cr.I32();
        bp.className = className;
        bp.line = line;
        bp.maxHits = maxHits;
        bp.maxFrames = maxFrames;
        bp.location = {{"method", info.value("name", std::string())}, {"code_index", index}};
        const json location = bp.location;
        breakpoints_[bp.id] = std::move(bp);
        return {{"ok", true}, {"breakpoint", nextBreakpoint_}, {"verified", true}, {"resolved", json::array({{{"line", line}}})},
                {"location", location}, {"action", "log"},
                {"note", "suspends only the hitting thread, reads the stack and locals, resumes it; cleared after max_hits"}};
    }
    return {{"ok", false}, {"error", "line_not_in_class"}, {"message", "no method of " + className + " has line " + std::to_string(line)}};
}

json JdwpSession::RemoveBreakpoint(int id) {
    int32_t request = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = breakpoints_.find(id);
        if (it == breakpoints_.end()) return {{"ok", false}, {"error", "unknown_breakpoint"}};
        if (!it->second.active) return {{"ok", true}, {"breakpoint", id}, {"already_inactive", true}};
        it->second.active = false;
        request = it->second.requestId;
    }
    Bytes clear{kEventBreakpoint};
    Put32(clear, static_cast<uint32_t>(request));
    return {{"ok", Command(15, 2, clear).ok}, {"breakpoint", id}};
}

json JdwpSession::Breakpoints() const {
    std::lock_guard<std::mutex> lock(mutex_);
    json rows = json::array();
    for (const auto& [id, bp] : breakpoints_)
        rows.push_back({{"breakpoint", id}, {"class", bp.className}, {"line", bp.line}, {"active", bp.active}, {"hits", bp.hits},
                        {"max_hits", bp.maxHits}, {"location", bp.location}, {"dropped", bp.dropped}});
    return {{"ok", true}, {"breakpoints", rows}};
}

json JdwpSession::Hits(int id, size_t offset, size_t limit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = breakpoints_.find(id);
    if (it == breakpoints_.end()) return {{"ok", false}, {"error", "unknown_breakpoint"}};
    json rows = json::array();
    for (size_t i = offset; i < it->second.records.size() && rows.size() < limit; ++i) rows.push_back(it->second.records[i]);
    return {{"ok", true}, {"breakpoint", id}, {"hits", it->second.hits}, {"active", it->second.active},
            {"dropped", it->second.dropped}, {"records", rows}};
}

json JdwpSession::Console(size_t, size_t) const {
    return {{"ok", true}, {"total", 0}, {"messages", json::array()}, {"note", "JDWP carries no program output"}};
}

json JdwpSession::Source(const std::string&, size_t, size_t) {
    return {{"ok", false}, {"error", "not_supported"}, {"message", "JDWP gives source file names only; read sources with cortex_artifact_read"}};
}

json JdwpSession::Scripts(const std::string& query, size_t offset, size_t limit) const {
    // AllClasses is a command: the const session sends it through a copy of
    // `this` (the stream is thread-safe).
    auto* self = const_cast<JdwpSession*>(this);
    const Reply reply = self->Command(1, 3, {});
    Reader r(reply.data);
    const int32_t count = reply.ok ? r.I32() : 0;
    json rows = json::array();
    size_t total = 0;
    for (int32_t i = 0; i < count && r.ok; ++i) {
        r.U8();
        r.Read(referenceTypeIdSize_);
        const std::string name = ClassName(r.Str());
        r.I32();
        if (!query.empty() && name.find(query) == std::string::npos) continue;
        if (total >= offset && rows.size() < limit) rows.push_back({{"url", name}, {"script_id", name}});
        ++total;
    }
    return {{"ok", reply.ok}, {"total", total}, {"offset", offset}, {"scripts", rows}};
}

json JdwpSession::Status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t active = 0;
    for (const auto& [id, bp] : breakpoints_) active += bp.active ? 1 : 0;
    return {{"ok", true},
            {"protocol", "jdwp"},
            {"connected", connected_.load()},
            {"port", port_},
            {"target", {{"type", "jvm"}, {"title", vm_.value("vm_name", std::string()) + " " + vm_.value("vm_version", std::string())}}},
            {"vm", vm_},
            {"breakpoints_active", active},
            {"pauses", events_total_},
            {"console_messages", 0},
            {"foreign_pauses", 0},
            {"foreign_pause_records", json::array()},
            {"disconnect_reason", lastError_}};
}

} // namespace cortex::live
