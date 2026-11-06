#pragma once

// Typed reads of JSON that a debug interface sent (Chrome DevTools, DAP
// adapters). That data comes from the inspected process and is untrusted:
// nlohmann's value() throws when a field has another type ({"type": 5}), and
// operator[] on a const object without the key is undefined behaviour. These
// accessors never throw: a missing or wrongly typed field reads as the
// default, an object or array that is missing reads as an empty one.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

namespace cortex::live::jsafe {

using json = nlohmann::json;

inline const json& EmptyObject() {
    static const json value = json::object();
    return value;
}

inline const json& EmptyArray() {
    static const json value = json::array();
    return value;
}

inline const json* Field(const json& object, const char* key) {
    if (!object.is_object()) return nullptr;
    const auto it = object.find(key);
    return it == object.end() ? nullptr : &*it;
}

inline std::string Str(const json& object, const char* key, const std::string& fallback = {}) {
    const json* value = Field(object, key);
    return value && value->is_string() ? value->get<std::string>() : fallback;
}

inline int64_t Int(const json& object, const char* key, int64_t fallback = 0) {
    const json* value = Field(object, key);
    if (!value) return fallback;
    if (value->is_number_integer()) return value->get<int64_t>();
    if (value->is_number_unsigned()) {
        const uint64_t v = value->get<uint64_t>();
        return v > static_cast<uint64_t>(INT64_MAX) ? fallback : static_cast<int64_t>(v);
    }
    if (value->is_number_float()) {
        const double v = value->get<double>();
        return v >= -9.2e18 && v <= 9.2e18 ? static_cast<int64_t>(v) : fallback;
    }
    return fallback;
}

inline bool Bool(const json& object, const char* key, bool fallback = false) {
    const json* value = Field(object, key);
    return value && value->is_boolean() ? value->get<bool>() : fallback;
}

inline const json& Obj(const json& object, const char* key) {
    const json* value = Field(object, key);
    return value && value->is_object() ? *value : EmptyObject();
}

inline const json& Arr(const json& object, const char* key) {
    const json* value = Field(object, key);
    return value && value->is_array() ? *value : EmptyArray();
}

// Any field, whatever its type; null when missing.
inline const json& Any(const json& object, const char* key) {
    static const json null;
    const json* value = Field(object, key);
    return value ? *value : null;
}

// Element `index` of an array, or null.
inline const json& At(const json& array, size_t index) {
    static const json null;
    return array.is_array() && index < array.size() ? array[index] : null;
}

// Text that is valid UTF-8, so that serializing it can never throw: invalid
// bytes (a JVM's modified UTF-8, a binary string) become U+FFFD.
inline std::string ValidUtf8(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    const size_t n = text.size();
    auto byte = [&](size_t at) { return static_cast<unsigned char>(text[at]); };
    while (i < n) {
        const unsigned char c = byte(i);
        size_t length = 0;
        uint32_t code = 0;
        if (c < 0x80) { out.push_back(static_cast<char>(c)); ++i; continue; }
        if ((c & 0xE0) == 0xC0) { length = 2; code = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { length = 3; code = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { length = 4; code = c & 0x07; }
        bool valid = length != 0 && i + length <= n;
        for (size_t k = 1; valid && k < length; ++k) {
            const unsigned char next = byte(i + k);
            if ((next & 0xC0) != 0x80) valid = false;
            else code = (code << 6) | (next & 0x3F);
        }
        if (valid) {
            // Overlong forms, surrogates and values past U+10FFFF are invalid.
            const uint32_t minimum = length == 2 ? 0x80 : length == 3 ? 0x800 : 0x10000;
            if (code < minimum || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) valid = false;
        }
        if (valid) {
            out.append(text, i, length);
            i += length;
        } else {
            out += "\xEF\xBF\xBD";
            ++i;
        }
    }
    return out;
}

// At most `size` bytes of `text` (cut before a partial UTF-8 character, with
// "..." when shortened), as valid UTF-8.
inline std::string Clip(const std::string& text, size_t size) {
    // Clean first: a replacement character is 3 bytes for 1 invalid byte, so
    // cleaning after the cut could triple the length.
    std::string clean = ValidUtf8(text);
    if (clean.size() <= size) return clean;
    size_t cut = size;
    while (cut > 0 && (static_cast<unsigned char>(clean[cut]) & 0xC0) == 0x80) --cut;
    clean.resize(cut);
    return clean + "...";
}

}  // namespace cortex::live::jsafe
