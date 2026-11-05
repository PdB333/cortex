// Live debug protocols: WebSocket messages (CDP), DAP framing, JDWP packets,
// and the typed JSON accessors and UTF-8 cleaning applied to what a debug
// interface sends. Besides "no crash", it checks properties: every string
// that goes into a record serializes, ValidUtf8 is idempotent, Clip respects
// its bound, and the accessors never throw.
#include "live/json_safe.h"
#include "live/protocol_frames.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace cortex::live;
using json = nlohmann::json;

namespace {

[[noreturn]] void Fail(const char* what) {
    std::fprintf(stderr, "property violated: %s\n", what);
    std::abort();
}

void CheckText(const std::string& raw) {
    const std::string clean = jsafe::ValidUtf8(raw);
    if (jsafe::ValidUtf8(clean) != clean) Fail("ValidUtf8 is not idempotent");
    try {
        (void)json(clean).dump();
    } catch (...) {
        Fail("a ValidUtf8 string does not serialize");
    }
    for (size_t size : {0u, 1u, 3u, 16u, 512u}) {
        const std::string clipped = jsafe::Clip(raw, size);
        if (clipped.size() > size + 3 && raw.size() > size) Fail("Clip exceeds its bound");
        try {
            (void)json(clipped).dump();
        } catch (...) {
            Fail("a clipped string does not serialize");
        }
    }
}

// Reads fields the way the sessions do at every node of the message (its
// real structure, so the walk is linear in the message size).
void Walk(const json& value, int depth) {
    if (depth > 16) return;
    static const char* const keys[] = {"type", "event", "request_seq", "body", "seq", "success", "message",
                                       "stackFrames", "source", "path", "line", "column", "name", "id",
                                       "method", "params", "result", "callFrames", "location", "scriptId",
                                       "lineNumber", "value", "variables", "hitBreakpointIds", "reason"};
    for (const char* key : keys) {
        CheckText(jsafe::Str(value, key));
        (void)jsafe::Int(value, key, -1);
        (void)jsafe::Bool(value, key, false);
        (void)jsafe::Obj(value, key);
        (void)jsafe::Arr(value, key);
        (void)jsafe::Any(value, key);
    }
    (void)jsafe::At(value, 0);
    if (value.is_object())
        for (const auto& item : value.items()) Walk(item.value(), depth + 1);
    else if (value.is_array())
        for (const auto& element : value) Walk(element, depth + 1);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) return 0;
    const uint8_t mode = data[0] % 4;
    const uint8_t* body = data + 1;
    const size_t length = size - 1;
    switch (mode) {
        case 0: {  // WebSocket frames from a CDP endpoint
            size_t at = 0;
            size_t sent = 0;
            frames::FrameIo io;
            io.read = [&](void* out, size_t want, int, bool& timedOut) {
                timedOut = false;
                if (length - at < want) {
                    timedOut = at == length;
                    return false;
                }
                std::memcpy(out, body + at, want);
                at += want;
                return true;
            };
            io.send = [&](uint8_t, const std::string& payload) { sent += payload.size(); };
            std::string text;
            bool timedOut = false;
            for (int i = 0; i < 64 && frames::ReceiveMessage(io, text, 0, timedOut); ++i) {
                const json message = json::parse(text, nullptr, false);
                Walk(message, 0);
            }
            break;
        }
        case 1: {  // DAP stream, delivered in uneven chunks
            std::string buffer;
            size_t at = 0;
            while (at < length) {
                const size_t chunk = 1 + (body[at] % 97);
                const size_t take = std::min(chunk, length - at);
                buffer.append(reinterpret_cast<const char*>(body + at), take);
                at += take;
                json message;
                frames::Take taken;
                while ((taken = frames::TakeDapMessage(buffer, message)) == frames::Take::Message) Walk(message, 0);
                if (taken == frames::Take::Fatal) return 0;
            }
            break;
        }
        case 2: {  // JDWP packets
            std::vector<uint8_t> buffer(body, body + length);
            frames::JdwpPacket packet;
            while (frames::TakeJdwpPacket(buffer, packet) == frames::Take::Message) {
                if (packet.data.size() > frames::kMaxJdwpPacket) Fail("JDWP packet over its bound");
            }
            break;
        }
        case 3:  // raw text that a debug interface reports (names, values, output)
            CheckText(std::string(reinterpret_cast<const char*>(body), length));
            break;
    }
    return 0;
}
