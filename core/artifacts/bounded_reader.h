#pragma once

// Small, portable byte reader shared by the offline PDB/ELF/container parsers.
// No packed-struct casts, host-endian assumptions, unchecked offset additions,
// or interpretation of input as instructions. All returned text is valid UTF-8.
#include "artifact.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace cortex::artifacts::bounded {

class Error : public std::runtime_error {
public:
    explicit Error(const std::string& what) : std::runtime_error(what) {}
};

inline bool Range(uint64_t total, uint64_t offset, uint64_t length) {
    return offset <= total && length <= total - offset;
}

inline std::string Utf8(const uint8_t* data, size_t size) {
    std::string out;
    for (size_t i = 0; i < size;) {
        const uint8_t a = data[i];
        size_t n = a < 0x80 ? 1 : (a >= 0xC2 && a <= 0xDF ? 2 :
                   (a >= 0xE0 && a <= 0xEF ? 3 : (a >= 0xF0 && a <= 0xF4 ? 4 : 0)));
        bool valid = n != 0 && n <= size - i;
        for (size_t j = 1; valid && j < n; ++j) valid = (data[i + j] & 0xC0) == 0x80;
        if (valid && n >= 3) {
            const uint8_t b = data[i + 1];
            valid = !(a == 0xE0 && b < 0xA0) && !(a == 0xED && b >= 0xA0) &&
                    !(a == 0xF0 && b < 0x90) && !(a == 0xF4 && b >= 0x90);
        }
        if (valid) {
            out.append(reinterpret_cast<const char*>(data + i), n);
            i += n;
        } else {
            out += "\xEF\xBF\xBD";
            ++i;
        }
    }
    return out;
}

class View {
public:
    View() = default;
    explicit View(const std::vector<uint8_t>& bytes) : data_(bytes.data()), size_(bytes.size()) {}
    View(const uint8_t* data, size_t size) : data_(data), size_(size) {}
    size_t Size() const { return size_; }
    bool Has(uint64_t offset, uint64_t length) const { return Range(size_, offset, length); }
    const uint8_t* Data(uint64_t offset, uint64_t length) const {
        if (!Has(offset, length)) throw Error("truncated_or_out_of_range");
        return offset ? data_ + static_cast<size_t>(offset) : data_;
    }
    View Slice(uint64_t offset, uint64_t length) const {
        return View(Data(offset, length), static_cast<size_t>(length));
    }
    uint8_t U8(uint64_t at) const { return *Data(at, 1); }
    uint16_t U16(uint64_t at, bool little = true) const { return static_cast<uint16_t>(UInt(at, 2, little)); }
    uint32_t U32(uint64_t at, bool little = true) const { return static_cast<uint32_t>(UInt(at, 4, little)); }
    uint64_t U64(uint64_t at, bool little = true) const { return UInt(at, 8, little); }
    // Bounded NUL-terminated string. next includes the NUL, before padding.
    std::string CString(uint64_t at, size_t* next = nullptr, size_t limit = 4096) const {
        if (!Has(at, 1)) throw Error("string_out_of_range");
        const size_t start = static_cast<size_t>(at);
        const size_t end = start + std::min(size_ - start, limit);
        size_t pos = start;
        while (pos < end && data_[pos]) ++pos;
        if (pos == end) throw Error("string_unterminated_or_limit");
        if (next) *next = pos + 1;
        return Utf8(data_ + start, pos - start);
    }
    std::string Text(uint64_t at, size_t length) const {
        return Utf8(Data(at, length), length);
    }
private:
    uint64_t UInt(uint64_t at, size_t n, bool little) const {
        const uint8_t* p = Data(at, n);
        uint64_t value = 0;
        for (size_t i = 0; i < n; ++i) value |= uint64_t(p[i]) << (8 * (little ? i : n - 1 - i));
        return value;
    }
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
};

inline void AppendCodePoint(std::string& out, uint32_t cp) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 63)); }
    else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 63)); out += static_cast<char>(0x80 | (cp & 63));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 63));
        out += static_cast<char>(0x80 | ((cp >> 6) & 63)); out += static_cast<char>(0x80 | (cp & 63));
    }
}

inline std::string Utf16(const View& bytes, bool little = true) {
    if (bytes.Size() % 2) throw Error("odd_utf16_size");
    std::string out;
    for (size_t at = 0; at < bytes.Size(); at += 2) {
        uint32_t cp = bytes.U16(at, little);
        if (cp >= 0xD800 && cp <= 0xDBFF && bytes.Has(at + 2, 2)) {
            const uint32_t low = bytes.U16(at + 2, little);
            if (low >= 0xDC00 && low <= 0xDFFF) { cp = 0x10000 + ((cp - 0xD800) << 10) + low - 0xDC00; at += 2; }
        }
        AppendCodePoint(out, cp);
    }
    return out;
}

// At most `limit` bytes of valid UTF-8 text, never cut inside a character.
inline std::string Clip(const std::string& text, size_t limit, const char* ellipsis = "...") {
    if (text.size() <= limit) return text;
    size_t end = limit;
    while (end > 0 && (static_cast<uint8_t>(text[end]) & 0xC0) == 0x80) --end;
    return text.substr(0, end) + ellipsis;
}

inline uint64_t Align4(uint64_t value) {
    if (value > std::numeric_limits<uint64_t>::max() - 3) throw Error("alignment_overflow");
    return (value + 3) & ~uint64_t(3);
}

inline std::vector<uint8_t> Read(const ByteSource& source, uint64_t at, uint64_t length,
                               uint64_t limit = 64ull << 20) {
    if (length > limit || length > std::numeric_limits<size_t>::max()) throw Error("read_budget_exceeded");
    if (!Range(source.Size(), at, length)) throw Error("file_range_invalid");
    auto bytes = source.ReadSome(at, static_cast<size_t>(length));
    if (bytes.size() != length) throw Error("file_read_failed");
    return bytes;
}

inline std::string Guid(const View& v, uint64_t at) {
    const uint8_t* b = v.Data(at, 16);
    static const char* digits = "0123456789abcdef";
    static const unsigned order[] = {3,2,1,0,5,4,7,6,8,9,10,11,12,13,14,15};
    std::string out;
    for (unsigned i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out += '-';
        out += digits[b[order[i]] >> 4]; out += digits[b[order[i]] & 15];
    }
    return out;
}

} // namespace cortex::artifacts::bounded
