#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cortex::services {

// Cheat Engine's user-defined types: a value a game does not store the way
// the standard types read it. Cortex describes one instead of scripting it,
// which covers what these are used for in practice — a big-endian field, a
// value scaled by ten, a few bits inside a word — and costs nothing to
// evaluate while the address list refreshes.
//
// A type reads `size` bytes, takes `bitCount` bits starting at `bitOffset`
// when it is an integer, then reports raw * scale + offset.

enum class CustomTypeBase : uint8_t {
    Integer = 0,
    Float,
    Double,
};

struct CustomType {
    std::string name;
    CustomTypeBase base = CustomTypeBase::Integer;
    size_t size = 4;             // bytes read; 1..8 integer, 4 float, 8 double
    bool bigEndian = false;
    bool signedValue = true;     // integers only
    unsigned bitOffset = 0;      // integers only, from the low bit
    unsigned bitCount = 0;       // integers only; 0 means the whole width
    double scale = 1.0;
    double offset = 0.0;
};

// Says why a type would not work, so the editor can refuse to save it.
bool ValidateCustomType(const CustomType& type, std::string* error = nullptr);

// Reads a value out of `size` bytes of memory. Fails when there are fewer
// bytes than the type needs.
bool ReadCustomType(const CustomType& type, const void* data, size_t size, double& value,
                    std::string* error = nullptr);

// Writes a value back, keeping the bits the type does not cover. `data`
// must already hold what the target has, since a bitfield is a read-modify-
// write.
bool WriteCustomType(const CustomType& type, double value, void* data, size_t size,
                     std::string* error = nullptr);

// The value as the address list shows it: whole numbers without a decimal
// point, otherwise as many digits as it takes to read back the same value.
std::string FormatCustomValue(const CustomType& type, double value);

// The named types of one session. Names are unique and compared exactly.
class CustomTypeTable {
public:
    const std::vector<CustomType>& Types() const { return types_; }
    const CustomType* Find(const std::string& name) const;
    // Adds or replaces the type of that name.
    bool Set(const CustomType& type, std::string* error = nullptr);
    bool Remove(const std::string& name);
    void Clear() { types_.clear(); }

    // A small text file, one type per line, so a table travels with a
    // project and can be read without Cortex.
    std::string Serialize() const;
    bool Deserialize(const std::string& text, std::string* error = nullptr);
    bool Save(const std::string& path, std::string* error = nullptr) const;
    bool Load(const std::string& path, std::string* error = nullptr);

private:
    std::vector<CustomType> types_;
};

} // namespace cortex::services
