#pragma once

// A minimal HTTP/1.1 GET and WebSocket (RFC 6455) client for local debug
// interfaces (Chrome DevTools Protocol of Node.js and Electron). It connects
// to 127.0.0.1 only: debug endpoints on other hosts are refused by design.

#include <cstdint>
#include <mutex>
#include <string>

namespace cortex::live {

// GET http://127.0.0.1:<port><path>; false with `error` on any failure or a
// non-200 status. Bodies are capped at `maxBody` bytes.
bool HttpGet(uint16_t port, const std::string& path, std::string& body, std::string& error, int timeoutMs = 3000,
             size_t maxBody = 8u << 20);

class WebSocket {
public:
    WebSocket();
    ~WebSocket();
    WebSocket(const WebSocket&) = delete;
    WebSocket& operator=(const WebSocket&) = delete;

    // ws://127.0.0.1:<port><path>; the Sec-WebSocket-Accept key is checked.
    bool Connect(uint16_t port, const std::string& path, std::string& error, int timeoutMs = 3000);
    // Thread-safe with respect to Receive on another thread.
    bool SendText(const std::string& text);
    // One complete text message (fragments joined, pings answered). Returns
    // false on close or error; `timedOut` tells a timeout apart.
    bool Receive(std::string& text, int timeoutMs, bool& timedOut);
    void Close();
    bool Open() const;

private:
    bool SendFrame(uint8_t opcode, const std::string& payload);
    bool ReadExact(void* data, size_t size, int timeoutMs, bool& timedOut);

    intptr_t socket_;
    std::mutex sendMutex_;
    std::string pending_;  // bytes read past the handshake
};

// Exposed for tests.
std::string Base64(const std::string& bytes);
std::string Sha1(const std::string& bytes);  // 20 raw bytes

} // namespace cortex::live
