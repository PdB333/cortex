#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace structs {

using json = nlohmann::json;

struct Field {
    std::string name;
    int64_t offset;
    std::string type; // numeric types plus pointer|vtable|vec3|vec4|matrix4|bytes|string
    int count = 0;     // bytes/string only
};

struct StructDef {
    std::string name;
    std::vector<Field> fields;
};

// Restores previously defined structs from the persistent project file (see
// core/project). Idempotent; call once during DLL init, after
// project::Init().
void Init();

// Defines (or overwrites) a named struct layout. Persisted immediately to
// the project file, so definitions survive a DLL reload.
bool Define(const std::string& name, const std::vector<Field>& fields);
bool Remove(const std::string& name);
std::vector<StructDef> List();

// Reads every field of `name` at `baseAddress` in one call. `outFields` gets
// {field_name: value} for fields that read successfully; `outErrors` gets
// {field_name: "read_failed"} for the rest. Returns false only if `name`
// isn't a known struct.
bool Read(const std::string& name, uintptr_t baseAddress, json& outFields, json& outErrors);

// Writes each field present as a key in `values` ({field_name: value}).
// Unknown field names and write failures are reported in `outErrors`.
// Returns false only if `name` isn't a known struct.
bool Write(const std::string& name, uintptr_t baseAddress, const json& values, json& outErrors);

} // namespace structs
