#include "websocket.h"
#include "protocol_frames.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace cortex::live {
namespace {

#if defined(_WIN32)
using Native = SOCKET;
constexpr intptr_t kInvalid = static_cast<intptr_t>(INVALID_SOCKET);
void CloseNative(intptr_t s) { closesocket(static_cast<SOCKET>(s)); }
bool EnsureWinsock() {
    static const bool ready = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ready;
}
int Poll(intptr_t s, short events, int timeoutMs) {
    WSAPOLLFD entry{static_cast<SOCKET>(s), events, 0};
    return WSAPoll(&entry, 1, timeoutMs);
}
#else
using Native = int;
constexpr intptr_t kInvalid = -1;
void CloseNative(intptr_t s) { ::close(static_cast<int>(s)); }
bool EnsureWinsock() { return true; }
int Poll(intptr_t s, short events, int timeoutMs) {
    pollfd entry{static_cast<int>(s), events, 0};
    return ::poll(&entry, 1, timeoutMs);
}
#endif

intptr_t ConnectLoopback(uint16_t port, int timeoutMs, std::string& error) {
    if (!EnsureWinsock()) {
        error = "socket_init_failed";
        return kInvalid;
    }
    const Native s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (static_cast<intptr_t>(s) == kInvalid) {
        error = "socket_failed";
        return kInvalid;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
#if defined(_WIN32)
    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
#endif
    int result = ::connect(s, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    if (result != 0) {
        if (Poll(static_cast<intptr_t>(s), POLLOUT, timeoutMs) <= 0) {
            CloseNative(static_cast<intptr_t>(s));
            error = "connect_failed";
            return kInvalid;
        }
        int code = 0;
        socklen_t length = sizeof(code);
        getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&code), &length);
        if (code != 0) {
            CloseNative(static_cast<intptr_t>(s));
            error = "connect_refused";
            return kInvalid;
        }
    }
#if defined(_WIN32)
    nonBlocking = 0;
    ioctlsocket(s, FIONBIO, &nonBlocking);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL) & ~O_NONBLOCK);
#endif
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
    return static_cast<intptr_t>(s);
}

bool SendAll(intptr_t s, const char* data, size_t size) {
    while (size) {
        const int sent = ::send(static_cast<Native>(s), data, static_cast<int>(std::min<size_t>(size, 1 << 20)), 0);
        if (sent <= 0) return false;
        data += sent;
        size -= static_cast<size_t>(sent);
    }
    return true;
}

// Reads until "\r\n\r\n"; the rest stays in `extra`.
bool ReadHeaders(intptr_t s, int timeoutMs, std::string& headers, std::string& extra) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    std::string data;
    char buffer[4096];
    while (data.find("\r\n\r\n") == std::string::npos) {
        const int left = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
        if (left <= 0 || Poll(s, POLLIN, left) <= 0) return false;
        const int got = ::recv(static_cast<Native>(s), buffer, sizeof(buffer), 0);
        if (got <= 0) return false;
        data.append(buffer, static_cast<size_t>(got));
        if (data.size() > (64u << 10)) return false;
    }
    const size_t end = data.find("\r\n\r\n") + 4;
    headers = data.substr(0, end);
    extra = data.substr(end);
    return true;
}

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string Header(const std::string& headers, const std::string& name) {
    const std::string lower = Lower(headers);
    const size_t at = lower.find("\r\n" + Lower(name) + ":");
    if (at == std::string::npos) return {};
    size_t start = at + name.size() + 3;
    const size_t end = headers.find("\r\n", start);
    while (start < end && headers[start] == ' ') ++start;
    return headers.substr(start, end - start);
}

int Status(const std::string& headers) {
    const size_t space = headers.find(' ');
    return space == std::string::npos ? 0 : std::atoi(headers.c_str() + space + 1);
}

} // namespace

std::string Sha1(const std::string& input) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::string message = input;
    const uint64_t bits = static_cast<uint64_t>(input.size()) * 8;
    message.push_back(static_cast<char>(0x80));
    while (message.size() % 64 != 56) message.push_back('\0');
    for (int i = 7; i >= 0; --i) message.push_back(static_cast<char>(bits >> (i * 8)));
    auto rotl = [](uint32_t v, int c) { return (v << c) | (v >> (32 - c)); };
    for (size_t chunk = 0; chunk < message.size(); chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (static_cast<uint32_t>(static_cast<uint8_t>(message[chunk + i * 4])) << 24) |
                   (static_cast<uint32_t>(static_cast<uint8_t>(message[chunk + i * 4 + 1])) << 16) |
                   (static_cast<uint32_t>(static_cast<uint8_t>(message[chunk + i * 4 + 2])) << 8) |
                   static_cast<uint32_t>(static_cast<uint8_t>(message[chunk + i * 4 + 3]));
        for (int i = 16; i < 80; ++i) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
            else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
            else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
            else f = b ^ c ^ d, k = 0xCA62C1D6;
            const uint32_t temp = rotl(a, 5) + f + e + k + w[i];
            e = d, d = c, c = rotl(b, 30), b = a, a = temp;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
    }
    std::string out;
    for (uint32_t v : h)
        for (int i = 3; i >= 0; --i) out.push_back(static_cast<char>(v >> (i * 8)));
    return out;
}

std::string Base64(const std::string& bytes) {
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const uint32_t v = (static_cast<uint8_t>(bytes[i]) << 16) | (static_cast<uint8_t>(bytes[i + 1]) << 8) | static_cast<uint8_t>(bytes[i + 2]);
        out += {table[v >> 18], table[(v >> 12) & 63], table[(v >> 6) & 63], table[v & 63]};
    }
    if (i + 1 == bytes.size()) {
        const uint32_t v = static_cast<uint8_t>(bytes[i]) << 16;
        out += {table[v >> 18], table[(v >> 12) & 63], '=', '='};
    } else if (i + 2 == bytes.size()) {
        const uint32_t v = (static_cast<uint8_t>(bytes[i]) << 16) | (static_cast<uint8_t>(bytes[i + 1]) << 8);
        out += {table[v >> 18], table[(v >> 12) & 63], table[(v >> 6) & 63], '='};
    }
    return out;
}

bool HttpGet(uint16_t port, const std::string& path, std::string& body, std::string& error, int timeoutMs, size_t maxBody) {
    const intptr_t s = ConnectLoopback(port, timeoutMs, error);
    if (s == kInvalid) return false;
    const std::string request = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                                "\r\nConnection: close\r\nAccept: application/json\r\n\r\n";
    std::string headers, data;
    if (!SendAll(s, request.data(), request.size()) || !ReadHeaders(s, timeoutMs, headers, data)) {
        CloseNative(s);
        error = "http_failed";
        return false;
    }
    const std::string length = Header(headers, "Content-Length");
    const size_t expected = length.empty() ? SIZE_MAX : static_cast<size_t>(std::strtoull(length.c_str(), nullptr, 10));
    char buffer[8192];
    while (data.size() < expected && data.size() <= maxBody) {
        if (Poll(s, POLLIN, timeoutMs) <= 0) break;
        const int got = ::recv(static_cast<Native>(s), buffer, sizeof(buffer), 0);
        if (got <= 0) break;
        data.append(buffer, static_cast<size_t>(got));
    }
    CloseNative(s);
    if (Status(headers) != 200) {
        error = "http_status_" + std::to_string(Status(headers));
        return false;
    }
    if (data.size() > maxBody || (expected != SIZE_MAX && data.size() < expected)) {
        error = data.size() > maxBody ? "http_body_too_large" : "http_body_truncated";
        return false;
    }
    body = data.substr(0, std::min(expected, data.size()));
    return true;
}

TcpStream::TcpStream() : socket_(kInvalid) {}
TcpStream::~TcpStream() { Close(); }
bool TcpStream::Open() const { return socket_ != kInvalid; }

bool TcpStream::Connect(uint16_t port, std::string& error, int timeoutMs) {
    Close();
    socket_ = ConnectLoopback(port, timeoutMs, error);
    return socket_ != kInvalid;
}

bool TcpStream::Send(const std::string& bytes) {
    std::lock_guard<std::mutex> lock(sendMutex_);
    return socket_ != kInvalid && SendAll(socket_, bytes.data(), bytes.size());
}

int TcpStream::Read(char* buffer, size_t max, int timeoutMs) {
    const intptr_t s = socket_;
    if (s == kInvalid) return -1;
    const int ready = Poll(s, POLLIN, timeoutMs);
    if (ready == 0) return 0;
    if (ready < 0) return -1;
    const int got = ::recv(static_cast<Native>(s), buffer, static_cast<int>(std::min<size_t>(max, 1 << 20)), 0);
    return got > 0 ? got : -1;
}

void TcpStream::Close() {
    std::lock_guard<std::mutex> lock(sendMutex_);
    if (socket_ == kInvalid) return;
#if defined(_WIN32)
    shutdown(static_cast<SOCKET>(socket_), SD_BOTH);
#else
    shutdown(static_cast<int>(socket_), SHUT_RDWR);
#endif
    CloseNative(socket_);
    socket_ = kInvalid;
}

WebSocket::WebSocket() : socket_(kInvalid) {}
WebSocket::~WebSocket() { Close(); }
bool WebSocket::Open() const { return socket_ != kInvalid; }

bool WebSocket::Connect(uint16_t port, const std::string& path, std::string& error, int timeoutMs) {
    Close();
    const intptr_t s = ConnectLoopback(port, timeoutMs, error);
    if (s == kInvalid) return false;
    std::random_device random;
    std::string nonce;
    for (int i = 0; i < 16; ++i) nonce.push_back(static_cast<char>(random() & 0xFF));
    const std::string key = Base64(nonce);
    const std::string request = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                                "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
                                "\r\nSec-WebSocket-Version: 13\r\n\r\n";
    std::string headers;
    if (!SendAll(s, request.data(), request.size()) || !ReadHeaders(s, timeoutMs, headers, pending_)) {
        CloseNative(s);
        error = "websocket_handshake_failed";
        return false;
    }
    const std::string expected = Base64(Sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
    if (Status(headers) != 101 || Header(headers, "Sec-WebSocket-Accept") != expected) {
        CloseNative(s);
        error = "websocket_handshake_rejected";
        return false;
    }
    socket_ = s;
    return true;
}

bool WebSocket::SendFrame(uint8_t opcode, const std::string& payload) {
    std::lock_guard<std::mutex> lock(sendMutex_);
    if (socket_ == kInvalid) return false;
    std::string frame;
    frame.push_back(static_cast<char>(0x80 | opcode));
    const uint64_t size = payload.size();
    if (size < 126) {
        frame.push_back(static_cast<char>(0x80 | size));
    } else if (size <= 0xFFFF) {
        frame.push_back(static_cast<char>(0x80 | 126));
        frame.push_back(static_cast<char>(size >> 8));
        frame.push_back(static_cast<char>(size & 0xFF));
    } else {
        frame.push_back(static_cast<char>(0x80 | 127));
        for (int i = 7; i >= 0; --i) frame.push_back(static_cast<char>(size >> (i * 8)));
    }
    static thread_local std::mt19937 random{std::random_device{}()};
    char mask[4];
    for (char& m : mask) m = static_cast<char>(random() & 0xFF);
    frame.append(mask, 4);
    for (size_t i = 0; i < payload.size(); ++i) frame.push_back(static_cast<char>(payload[i] ^ mask[i % 4]));
    return SendAll(socket_, frame.data(), frame.size());
}

bool WebSocket::SendText(const std::string& text) { return SendFrame(0x1, text); }

bool WebSocket::ReadExact(void* data, size_t size, int timeoutMs, bool& timedOut) {
    auto* out = static_cast<char*>(data);
    while (size && !pending_.empty()) {
        const size_t take = std::min(size, pending_.size());
        std::memcpy(out, pending_.data(), take);
        pending_.erase(0, take);
        out += take;
        size -= take;
    }
    while (size) {
        const int ready = Poll(socket_, POLLIN, timeoutMs);
        if (ready == 0) {
            timedOut = true;
            return false;
        }
        if (ready < 0) return false;
        const int got = ::recv(static_cast<Native>(socket_), out, static_cast<int>(std::min<size_t>(size, 1 << 20)), 0);
        if (got <= 0) return false;
        out += got;
        size -= static_cast<size_t>(got);
    }
    return true;
}

bool WebSocket::Receive(std::string& text, int timeoutMs, bool& timedOut) {
    timedOut = false;
    text.clear();
    if (socket_ == kInvalid) return false;
    frames::FrameIo io;
    io.read = [this](void* data, size_t size, int wait, bool& expired) { return ReadExact(data, size, wait, expired); };
    io.send = [this](uint8_t opcode, const std::string& payload) { SendFrame(opcode, payload); };
    return frames::ReceiveMessage(io, text, timeoutMs, timedOut);
}

void WebSocket::Close() {
    if (socket_ == kInvalid) return;
    SendFrame(0x8, std::string("\x03\xE8", 2));
    std::lock_guard<std::mutex> lock(sendMutex_);
#if defined(_WIN32)
    shutdown(static_cast<SOCKET>(socket_), SD_BOTH);
#else
    shutdown(static_cast<int>(socket_), SHUT_RDWR);
#endif
    CloseNative(socket_);
    socket_ = kInvalid;
    pending_.clear();
}

} // namespace cortex::live
