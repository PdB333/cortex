#include "protocol_frames.h"

#include <cstdlib>
#include <cstring>

namespace cortex::live::frames {

bool ReceiveMessage(const FrameIo& io, std::string& text, int timeoutMs, bool& timedOut) {
    timedOut = false;
    text.clear();
    bool first = true;
    for (;;) {
        uint8_t head[2];
        // Only the first byte may time out; a started frame is read in full.
        if (!io.read(head, 2, first ? timeoutMs : 10000, timedOut)) {
            if (!first) timedOut = false;
            return false;
        }
        first = false;
        const bool fin = head[0] & 0x80;
        const uint8_t opcode = head[0] & 0x0F;
        uint64_t size = head[1] & 0x7F;
        bool ignored = false;
        if (size == 126) {
            uint8_t ext[2];
            if (!io.read(ext, 2, 10000, ignored)) return false;
            size = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
        } else if (size == 127) {
            uint8_t ext[8];
            if (!io.read(ext, 8, 10000, ignored)) return false;
            size = 0;
            for (uint8_t b : ext) size = (size << 8) | b;
        }
        if (size > kMaxWebSocketMessage || text.size() + size > kMaxWebSocketMessage) return false;  // bounded
        uint8_t mask[4] = {0, 0, 0, 0};
        const bool masked = head[1] & 0x80;
        if (masked && !io.read(mask, 4, 10000, ignored)) return false;
        std::string payload(static_cast<size_t>(size), '\0');
        if (size && !io.read(payload.data(), payload.size(), 10000, ignored)) return false;
        if (masked)
            for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(payload[i] ^ mask[i % 4]);
        if (opcode == 0x8) {  // close
            if (io.send) io.send(0x8, payload.substr(0, 2));
            return false;
        }
        if (opcode == 0x9) {  // ping
            if (io.send) io.send(0xA, payload);
            first = text.empty();
            continue;
        }
        if (opcode == 0xA) {
            first = text.empty();
            continue;
        }
        text += payload;
        if (fin) return true;
    }
}

Take TakeDapMessage(std::string& buffer, nlohmann::json& message) {
    for (;;) {
        const size_t end = buffer.find("\r\n\r\n");
        if (end == std::string::npos) return buffer.size() > kMaxDapMessage ? Take::Fatal : Take::NeedMore;
        const std::string header = buffer.substr(0, end);
        const size_t at = header.find("Content-Length:");
        if (at == std::string::npos) {
            buffer.erase(0, end + 4);
            continue;
        }
        const char* digits = header.c_str() + at + 15;
        while (*digits == ' ' || *digits == '\t') ++digits;
        if (*digits < '0' || *digits > '9') return Take::Fatal;
        char* stop = nullptr;
        const unsigned long long length = std::strtoull(digits, &stop, 10);
        if (length > kMaxDapMessage) return Take::Fatal;
        const size_t total = end + 4 + static_cast<size_t>(length);
        if (buffer.size() < total) return Take::NeedMore;
        message = nlohmann::json::parse(buffer.begin() + static_cast<std::ptrdiff_t>(end + 4),
                                        buffer.begin() + static_cast<std::ptrdiff_t>(total), nullptr, false);
        buffer.erase(0, total);
        if (!message.is_object()) message = nullptr;
        return Take::Message;
    }
}

Take TakeJdwpPacket(std::vector<uint8_t>& buffer, JdwpPacket& packet) {
    if (buffer.size() < 11) return Take::NeedMore;
    const uint32_t length = (uint32_t{buffer[0]} << 24) | (uint32_t{buffer[1]} << 16) | (uint32_t{buffer[2]} << 8) | buffer[3];
    if (length < 11 || length > kMaxJdwpPacket) return Take::Fatal;
    if (buffer.size() < length) return Take::NeedMore;
    packet.id = (uint32_t{buffer[4]} << 24) | (uint32_t{buffer[5]} << 16) | (uint32_t{buffer[6]} << 8) | buffer[7];
    packet.flags = buffer[8];
    packet.high = buffer[9];
    packet.low = buffer[10];
    packet.data.assign(buffer.begin() + 11, buffer.begin() + length);
    buffer.erase(buffer.begin(), buffer.begin() + length);
    return Take::Message;
}

}  // namespace cortex::live::frames
