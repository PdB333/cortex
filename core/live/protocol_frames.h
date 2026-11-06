#pragma once

// Framing of the live debug protocols, apart from their sockets so the same
// code runs in the sessions and under the fuzzer (tests/fuzz/fuzz_protocols).
// Everything here reads bytes sent by a debug interface: untrusted input.

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace cortex::live::frames {

// WebSocket (RFC 6455) messages, as CDP uses them.
struct FrameIo {
    // Reads exactly `size` bytes; false on a closed connection or a timeout
    // (then `timedOut` is true).
    std::function<bool(void* data, size_t size, int timeoutMs, bool& timedOut)> read;
    // Sends a control frame (pong, close).
    std::function<void(uint8_t opcode, const std::string& payload)> send;
};
constexpr uint64_t kMaxWebSocketMessage = 64u << 20;
// One text message, joining continuation frames and answering pings. False
// on close, a timeout before the first byte, or a message over the bound.
bool ReceiveMessage(const FrameIo& io, std::string& text, int timeoutMs, bool& timedOut);

enum class Take { NeedMore, Message, Fatal };

// DAP: "Content-Length: N\r\n\r\n" + N bytes of JSON. Takes one message off
// the front of `buffer`. A part without Content-Length is skipped; a message
// that is not a JSON object comes back as a discarded (null) message. Fatal
// when a length is out of bounds.
constexpr size_t kMaxDapMessage = 64u << 20;
Take TakeDapMessage(std::string& buffer, nlohmann::json& message);

// JDWP packets: length(4) id(4) flags(1) then error code(2) for a reply, or
// command set(1) + command(1); `data` is the rest. Fatal on a length out of
// bounds (11 bytes to 64 MiB).
struct JdwpPacket {
    uint32_t id = 0;
    uint8_t flags = 0;
    uint8_t high = 0, low = 0;  // error code bytes, or command set / command
    std::vector<uint8_t> data;
    bool IsReply() const { return (flags & 0x80) != 0; }
};
constexpr uint32_t kMaxJdwpPacket = 64u << 20;
Take TakeJdwpPacket(std::vector<uint8_t>& buffer, JdwpPacket& packet);

}  // namespace cortex::live::frames
