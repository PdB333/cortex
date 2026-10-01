#pragma once

// Helpers shared by the Memory workspace sources (scanner and address list).

#include "services/value_scanner.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace cortex::ui::memory_internal {

using services::ScanCompare;
using services::ScanDataType;
using services::ScanTristate;
using services::ValueScanner;

inline constexpr ScanDataType kTypes[] = {
    ScanDataType::Byte, ScanDataType::Int16, ScanDataType::Int32, ScanDataType::Int64,
    ScanDataType::Float, ScanDataType::Double, ScanDataType::String, ScanDataType::ByteArray,
    ScanDataType::AllNumeric
};

inline const char* const kRoundingNames[] = {
    "Rounded (default)", "Rounded (extreme)", "Truncated", "Exact"
};

inline std::string Hex(uint64_t value) {
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%llX", static_cast<unsigned long long>(value));
    return buffer;
}

inline std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return text;
}

inline std::string Trim(const std::string& text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

inline bool ParseHex(const std::string& raw, uint64_t& value) {
    std::string text = Trim(raw);
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text.erase(0, 2);
    if (text.empty() || text.size() > 16) return false;
    for (const char ch : text)
        if (!std::isxdigit(static_cast<unsigned char>(ch))) return false;
    value = std::strtoull(text.c_str(), nullptr, 16);
    return true;
}

inline bool IsText(ScanDataType type) {
    return type == ScanDataType::String || type == ScanDataType::ByteArray;
}

// Addresses workspace and live watches name numeric types this way.
inline std::string PersistentType(ScanDataType type, bool isUnsigned) {
    switch (type) {
        case ScanDataType::Byte: return isUnsigned ? "u8" : "i8";
        case ScanDataType::Int16: return isUnsigned ? "u16" : "i16";
        case ScanDataType::Int32: return isUnsigned ? "u32" : "i32";
        case ScanDataType::Int64: return isUnsigned ? "u64" : "i64";
        case ScanDataType::Float: return "float";
        case ScanDataType::Double: return "double";
        default: return {};
    }
}

inline ScanDataType TypeFromSetting(const std::string& value) {
    if (value == "byte") return ScanDataType::Byte;
    if (value == "i16") return ScanDataType::Int16;
    if (value == "i64") return ScanDataType::Int64;
    if (value == "f32") return ScanDataType::Float;
    if (value == "f64") return ScanDataType::Double;
    if (value == "string") return ScanDataType::String;
    if (value == "bytes") return ScanDataType::ByteArray;
    if (value == "all") return ScanDataType::AllNumeric;
    return ScanDataType::Int32;
}

inline int RoundingFromSetting(const std::string& value) {
    if (value == "extreme") return 1;
    if (value == "truncated") return 2;
    if (value == "exact") return 3;
    return 0;
}

// -1, 0 or 1 comparing two stored values of a numeric type.
inline int CompareStored(ScanDataType type, bool isUnsigned, const std::vector<uint8_t>& left,
                  const std::vector<uint8_t>& right) {
    const size_t size = ValueScanner::TypeSize(type);
    if (!size || left.size() < size || right.size() < size) return 0;
    if (type == ScanDataType::Float || type == ScanDataType::Double) {
        double a = 0.0;
        double b = 0.0;
        if (type == ScanDataType::Float) {
            float fa = 0.0f;
            float fb = 0.0f;
            std::memcpy(&fa, left.data(), 4);
            std::memcpy(&fb, right.data(), 4);
            a = fa;
            b = fb;
        } else {
            std::memcpy(&a, left.data(), 8);
            std::memcpy(&b, right.data(), 8);
        }
        return a < b ? -1 : (a > b ? 1 : 0);
    }
    uint64_t a = 0;
    uint64_t b = 0;
    std::memcpy(&a, left.data(), size);
    std::memcpy(&b, right.data(), size);
    if (!isUnsigned) {
        const unsigned shift = static_cast<unsigned>(64 - size * 8);
        const int64_t sa = shift ? static_cast<int64_t>(a << shift) >> shift : static_cast<int64_t>(a);
        const int64_t sb = shift ? static_cast<int64_t>(b << shift) >> shift : static_cast<int64_t>(b);
        return sa < sb ? -1 : (sa > sb ? 1 : 0);
    }
    return a < b ? -1 : (a > b ? 1 : 0);
}

inline bool TypeCombo(const char* id, int* index, bool includeAll) {
    bool changed = false;
    const auto current = static_cast<ScanDataType>(*index);
    if (ImGui::BeginCombo(id, ValueScanner::TypeName(current))) {
        for (const auto type : kTypes) {
            if (!includeAll && type == ScanDataType::AllNumeric) continue;
            const bool selected = type == current;
            if (ImGui::Selectable(ValueScanner::TypeName(type), selected)) {
                *index = static_cast<int>(type);
                changed = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

} // namespace cortex::ui::memory_internal
