#pragma once

#include <cstddef>
#include <limits>
#include <string>

namespace api::request_limits {

constexpr size_t kMaxPayloadBytes = 16u * 1024u * 1024u;

enum class ContentLengthError {
    None,
    Invalid,
    TooLarge
};

struct ContentLengthResult {
    size_t length = 0;
    ContentLengthError error = ContentLengthError::None;

    explicit operator bool() const { return error == ContentLengthError::None; }
};

inline ContentLengthResult ParseContentLength(const std::string& raw,
                                              size_t maxBytes = kMaxPayloadBytes) {
    if (raw.empty()) return {};
    try {
        size_t consumed = 0;
        const unsigned long long parsed = std::stoull(raw, &consumed, 10);
        if (consumed != raw.size() || parsed > (std::numeric_limits<size_t>::max)())
            return {0, ContentLengthError::Invalid};
        const size_t length = static_cast<size_t>(parsed);
        if (length > maxBytes) return {length, ContentLengthError::TooLarge};
        return {length, ContentLengthError::None};
    } catch (...) {
        return {0, ContentLengthError::Invalid};
    }
}

} // namespace api::request_limits
