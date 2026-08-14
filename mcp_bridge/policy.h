#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>

namespace mcp_bridge::policy {

constexpr size_t kMaxMessageBytes = 4u * 1024u * 1024u;

inline std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

inline bool IsLoopbackHost(const std::string& host) {
    const std::string value = Lower(host);
    return value == "127.0.0.1" || value == "localhost" ||
           value == "::1" || value == "[::1]";
}

inline bool IsValidPort(int port) {
    return port > 0 && port <= 65535;
}

inline bool IsMessageSizeAllowed(size_t bytes) {
    return bytes <= kMaxMessageBytes;
}

} // namespace mcp_bridge::policy
