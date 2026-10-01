#include "custom_types.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace cortex::services {
namespace {

bool Fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}

// The bits a type actually covers.
unsigned Width(const CustomType& type) {
    const unsigned bits = static_cast<unsigned>(type.size) * 8u;
    if (type.base != CustomTypeBase::Integer) return bits;
    const unsigned count = type.bitCount ? type.bitCount : bits - type.bitOffset;
    return std::min(count, bits - type.bitOffset);
}

uint64_t RawBits(const CustomType& type, const uint8_t* data) {
    uint64_t raw = 0;
    for (size_t i = 0; i < type.size; ++i) {
        const uint8_t byte = type.bigEndian ? data[i] : data[type.size - 1 - i];
        raw = (raw << 8) | byte;
    }
    return raw;
}

void StoreBits(const CustomType& type, uint64_t raw, uint8_t* data) {
    for (size_t i = 0; i < type.size; ++i) {
        const auto byte = static_cast<uint8_t>(raw & 0xFFu);
        if (type.bigEndian) data[type.size - 1 - i] = byte;
        else data[i] = byte;
        raw >>= 8;
    }
}

std::string Escape(const std::string& text) {
    std::string out;
    for (const char c : text) {
        if (c == '\\' || c == '|') out += '\\';
        if (c == '\n') {
            out += "\\n";
            continue;
        }
        out += c;
    }
    return out;
}

std::string Unescape(const std::string& text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\\' || i + 1 == text.size()) {
            out += text[i];
            continue;
        }
        const char next = text[++i];
        out += next == 'n' ? '\n' : next;
    }
    return out;
}

// Splits on '|', honouring the backslash escapes Escape writes.
std::vector<std::string> SplitFields(const std::string& line) {
    std::vector<std::string> fields;
    std::string current;
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '\\' && i + 1 < line.size()) {
            current += line[i];
            current += line[++i];
            continue;
        }
        if (line[i] == '|') {
            fields.push_back(current);
            current.clear();
            continue;
        }
        current += line[i];
    }
    fields.push_back(current);
    return fields;
}

} // namespace

bool ValidateCustomType(const CustomType& type, std::string* error) {
    if (type.name.empty()) return Fail(error, "The type needs a name");
    for (const char c : type.name)
        if (c == '|' || c == '\n' || c == '\r') return Fail(error, "The name cannot hold '|' or a newline");
    switch (type.base) {
        case CustomTypeBase::Float:
            if (type.size != 4) return Fail(error, "A float is 4 bytes");
            break;
        case CustomTypeBase::Double:
            if (type.size != 8) return Fail(error, "A double is 8 bytes");
            break;
        case CustomTypeBase::Integer:
            if (type.size < 1 || type.size > 8) return Fail(error, "An integer is 1 to 8 bytes");
            if (type.bitOffset >= type.size * 8)
                return Fail(error, "The first bit is past the end of the value");
            if (type.bitCount > type.size * 8 - type.bitOffset)
                return Fail(error, "There are not that many bits left in the value");
            break;
    }
    if (!(type.scale != 0.0)) return Fail(error, "A scale of zero cannot be written back");
    if (!std::isfinite(type.scale) || !std::isfinite(type.offset))
        return Fail(error, "The scale and the offset must be numbers");
    if (error) error->clear();
    return true;
}

bool ReadCustomType(const CustomType& type, const void* data, size_t size, double& value,
                    std::string* error) {
    value = 0.0;
    if (!ValidateCustomType(type, error)) return false;
    if (!data || size < type.size) return Fail(error, "Not enough bytes for " + type.name);
    const auto* bytes = static_cast<const uint8_t*>(data);

    double raw = 0.0;
    if (type.base == CustomTypeBase::Float) {
        uint32_t bits = 0;
        const uint64_t stored = RawBits(type, bytes);
        bits = static_cast<uint32_t>(stored);
        float narrow = 0.0f;
        std::memcpy(&narrow, &bits, 4);
        raw = narrow;
    } else if (type.base == CustomTypeBase::Double) {
        const uint64_t bits = RawBits(type, bytes);
        double wide = 0.0;
        std::memcpy(&wide, &bits, 8);
        raw = wide;
    } else {
        const unsigned width = Width(type);
        uint64_t field = RawBits(type, bytes) >> type.bitOffset;
        if (width < 64) field &= (1ull << width) - 1ull;
        if (type.signedValue && width && width <= 64) {
            const uint64_t sign = 1ull << (width - 1);
            if (field & sign) {
                // Sign-extend the field, then read it as a signed number.
                const uint64_t extended = width == 64 ? field : field | ~((1ull << width) - 1ull);
                raw = static_cast<double>(static_cast<int64_t>(extended));
            } else {
                raw = static_cast<double>(field);
            }
        } else {
            raw = static_cast<double>(field);
        }
    }

    value = raw * type.scale + type.offset;
    if (error) error->clear();
    return true;
}

bool WriteCustomType(const CustomType& type, double value, void* data, size_t size, std::string* error) {
    if (!ValidateCustomType(type, error)) return false;
    if (!data || size < type.size) return Fail(error, "Not enough bytes for " + type.name);
    auto* bytes = static_cast<uint8_t*>(data);

    const double raw = (value - type.offset) / type.scale;
    if (!std::isfinite(raw)) return Fail(error, "That value cannot be stored in " + type.name);

    if (type.base == CustomTypeBase::Float) {
        const auto narrow = static_cast<float>(raw);
        uint32_t bits = 0;
        std::memcpy(&bits, &narrow, 4);
        StoreBits(type, bits, bytes);
    } else if (type.base == CustomTypeBase::Double) {
        const double wide = raw;
        uint64_t bits = 0;
        std::memcpy(&bits, &wide, 8);
        StoreBits(type, bits, bytes);
    } else {
        const unsigned width = Width(type);
        const double rounded = std::nearbyint(raw);
        uint64_t field = 0;
        if (type.signedValue) {
            const auto signedValue = static_cast<int64_t>(rounded);
            field = static_cast<uint64_t>(signedValue);
        } else {
            if (rounded < 0.0) return Fail(error, type.name + " does not hold negative values");
            field = static_cast<uint64_t>(rounded);
        }
        const uint64_t mask = width >= 64 ? ~0ull : (1ull << width) - 1ull;
        // The bits outside the field belong to whatever else lives there.
        const uint64_t current = RawBits(type, bytes);
        const uint64_t cleared = current & ~(mask << type.bitOffset);
        StoreBits(type, cleared | ((field & mask) << type.bitOffset), bytes);
    }
    if (error) error->clear();
    return true;
}

std::string FormatCustomValue(const CustomType& type, double value) {
    char buffer[64] = {};
    const bool whole = type.base == CustomTypeBase::Integer && type.scale == 1.0 &&
                       type.offset == std::nearbyint(type.offset);
    if (whole || value == std::nearbyint(value)) {
        std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(std::nearbyint(value)));
        return buffer;
    }
    std::snprintf(buffer, sizeof(buffer), "%.*g", 9, value);
    return buffer;
}

// ------------------------------------------------------------------ table

const CustomType* CustomTypeTable::Find(const std::string& name) const {
    const auto found = std::find_if(types_.begin(), types_.end(),
                                    [&name](const CustomType& type) { return type.name == name; });
    return found == types_.end() ? nullptr : &*found;
}

bool CustomTypeTable::Set(const CustomType& type, std::string* error) {
    if (!ValidateCustomType(type, error)) return false;
    const auto found = std::find_if(types_.begin(), types_.end(),
                                    [&type](const CustomType& other) { return other.name == type.name; });
    if (found == types_.end()) types_.push_back(type);
    else *found = type;
    return true;
}

bool CustomTypeTable::Remove(const std::string& name) {
    const auto found = std::find_if(types_.begin(), types_.end(),
                                    [&name](const CustomType& type) { return type.name == name; });
    if (found == types_.end()) return false;
    types_.erase(found);
    return true;
}

std::string CustomTypeTable::Serialize() const {
    std::ostringstream out;
    out << "# Cortex custom types\n";
    out << "# name|base|size|bigEndian|signed|bitOffset|bitCount|scale|offset\n";
    for (const auto& type : types_) {
        out << Escape(type.name) << '|'
            << (type.base == CustomTypeBase::Float ? "float"
                : type.base == CustomTypeBase::Double ? "double"
                                                      : "integer")
            << '|' << type.size << '|' << (type.bigEndian ? 1 : 0) << '|' << (type.signedValue ? 1 : 0)
            << '|' << type.bitOffset << '|' << type.bitCount << '|' << type.scale << '|' << type.offset
            << '\n';
    }
    return out.str();
}

bool CustomTypeTable::Deserialize(const std::string& text, std::string* error) {
    std::vector<CustomType> parsed;
    std::istringstream in(text);
    std::string line;
    size_t number = 0;
    while (std::getline(in, line)) {
        ++number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const auto fields = SplitFields(line);
        if (fields.size() < 9)
            return Fail(error, "Line " + std::to_string(number) + " does not have nine fields");
        CustomType type;
        type.name = Unescape(fields[0]);
        type.base = fields[1] == "float"    ? CustomTypeBase::Float
                    : fields[1] == "double" ? CustomTypeBase::Double
                                            : CustomTypeBase::Integer;
        type.size = static_cast<size_t>(std::strtoul(fields[2].c_str(), nullptr, 10));
        type.bigEndian = fields[3] == "1";
        type.signedValue = fields[4] == "1";
        type.bitOffset = static_cast<unsigned>(std::strtoul(fields[5].c_str(), nullptr, 10));
        type.bitCount = static_cast<unsigned>(std::strtoul(fields[6].c_str(), nullptr, 10));
        type.scale = std::strtod(fields[7].c_str(), nullptr);
        type.offset = std::strtod(fields[8].c_str(), nullptr);
        std::string message;
        if (!ValidateCustomType(type, &message))
            return Fail(error, "Line " + std::to_string(number) + ": " + message);
        parsed.push_back(std::move(type));
    }
    types_ = std::move(parsed);
    if (error) error->clear();
    return true;
}

bool CustomTypeTable::Save(const std::string& path, std::string* error) const {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return Fail(error, "Cannot write \"" + path + "\"");
    const std::string text = Serialize();
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!file) return Fail(error, "Cannot write \"" + path + "\"");
    return true;
}

bool CustomTypeTable::Load(const std::string& path, std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return Fail(error, "Cannot read \"" + path + "\"");
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return Deserialize(buffer.str(), error);
}

} // namespace cortex::services
