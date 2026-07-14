#include "structs.h"
#include "../memory/memory.h"
#include "../project/project.h"

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>

namespace structs {

namespace {

std::mutex g_mutex;
std::map<std::string, std::vector<Field>> g_structs;
bool g_initialized = false;

json FieldToJson(const Field& f) {
    return {{"name", f.name}, {"offset", f.offset}, {"type", f.type}, {"count", f.count}};
}

Field FieldFromJson(const json& j) {
    Field f;
    f.name = j.value("name", std::string(""));
    f.offset = j.value("offset", static_cast<int64_t>(0));
    f.type = j.value("type", std::string(""));
    f.count = j.value("count", 0);
    return f;
}

// Caller must hold g_mutex.
void SyncToProject() {
    json arr = json::array();
    for (auto& [name, fields] : g_structs) {
        json fieldsJson = json::array();
        for (auto& f : fields) fieldsJson.push_back(FieldToJson(f));
        arr.push_back({{"name", name}, {"fields", fieldsJson}});
    }
    project::SetStructDefs(arr);
}

std::string BytesToHex(const std::vector<uint8_t>& buf) {
    std::ostringstream hex;
    for (uint8_t b : buf) hex << std::setw(2) << std::setfill('0') << std::hex << static_cast<int>(b);
    return hex.str();
}

std::vector<uint8_t> HexToBytes(const std::string& hexIn) {
    std::vector<uint8_t> out;
    std::string s = hexIn;
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s = s.substr(2);
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        out.push_back(static_cast<uint8_t>(std::stoi(s.substr(i, 2), nullptr, 16)));
    }
    return out;
}

json ReadTypedValue(uintptr_t address, const std::string& type, int count, bool& ok) {
    std::vector<uint8_t> buf;
    json result;
    ok = false;

    if (type == "i8") { if ((ok = memory::ReadBytes(address, 1, buf))) result = static_cast<int8_t>(buf[0]); }
    else if (type == "u8") { if ((ok = memory::ReadBytes(address, 1, buf))) result = buf[0]; }
    else if (type == "i16") { if ((ok = memory::ReadBytes(address, 2, buf))) { int16_t v; memcpy(&v, buf.data(), 2); result = v; } }
    else if (type == "u16") { if ((ok = memory::ReadBytes(address, 2, buf))) { uint16_t v; memcpy(&v, buf.data(), 2); result = v; } }
    else if (type == "i32") { if ((ok = memory::ReadBytes(address, 4, buf))) { int32_t v; memcpy(&v, buf.data(), 4); result = v; } }
    else if (type == "u32") { if ((ok = memory::ReadBytes(address, 4, buf))) { uint32_t v; memcpy(&v, buf.data(), 4); result = v; } }
    else if (type == "i64") { if ((ok = memory::ReadBytes(address, 8, buf))) { int64_t v; memcpy(&v, buf.data(), 8); result = (v >= -9007199254740991LL && v <= 9007199254740991LL) ? json(v) : json(std::to_string(v)); } }
    else if (type == "u64") { if ((ok = memory::ReadBytes(address, 8, buf))) { uint64_t v; memcpy(&v, buf.data(), 8); result = v <= 9007199254740991ULL ? json(v) : json(std::to_string(v)); } }
    else if (type == "float") { if ((ok = memory::ReadBytes(address, 4, buf))) { float v; memcpy(&v, buf.data(), 4); result = v; } }
    else if (type == "double") { if ((ok = memory::ReadBytes(address, 8, buf))) { double v; memcpy(&v, buf.data(), 8); result = v; } }
    else if (type == "pointer" || type == "vtable") {
#ifdef _WIN64
        constexpr size_t n = 8;
#else
        constexpr size_t n = 4;
#endif
        if ((ok = memory::ReadBytes(address, n, buf))) {
            uintptr_t v = 0; memcpy(&v, buf.data(), n);
            std::ostringstream out; out << "0x" << std::hex << v; result = out.str();
        }
    }
    else if (type == "vec3" || type == "vec4" || type == "matrix4") {
        const int components = type == "vec3" ? 3 : type == "vec4" ? 4 : 16;
        if ((ok = memory::ReadBytes(address, components * 4, buf))) {
            result = json::array();
            for (int i = 0; i < components; ++i) { float v; memcpy(&v, buf.data() + i * 4, 4); result.push_back(v); }
        }
    }
    else if (type == "bytes") {
        int n = count > 0 ? count : 16;
        if ((ok = memory::ReadBytes(address, n, buf))) result = BytesToHex(buf);
    } else if (type == "string") {
        int n = count > 0 ? count : 64;
        auto s = memory::ReadString(address, n);
        ok = s.has_value();
        if (ok) result = *s;
    }
    return result;
}

bool WriteTypedValue(uintptr_t address, const std::string& type, const json& jvalue) {
    std::vector<uint8_t> buf;

    if (type == "i8" || type == "u8") { uint8_t v = static_cast<uint8_t>(jvalue.get<int>()); buf = {v}; }
    else if (type == "i16" || type == "u16") { uint16_t v = static_cast<uint16_t>(jvalue.get<int>()); buf.resize(2); memcpy(buf.data(), &v, 2); }
    else if (type == "i32" || type == "u32") { uint32_t v = static_cast<uint32_t>(jvalue.get<int64_t>()); buf.resize(4); memcpy(buf.data(), &v, 4); }
    else if (type == "i64") { int64_t v = jvalue.is_string() ? std::stoll(jvalue.get<std::string>(), nullptr, 0) : jvalue.get<int64_t>(); buf.resize(8); memcpy(buf.data(), &v, 8); }
    else if (type == "u64") { uint64_t v = jvalue.is_string() ? std::stoull(jvalue.get<std::string>(), nullptr, 0) : jvalue.get<uint64_t>(); buf.resize(8); memcpy(buf.data(), &v, 8); }
    else if (type == "float") { float v = jvalue.get<float>(); buf.resize(4); memcpy(buf.data(), &v, 4); }
    else if (type == "double") { double v = jvalue.get<double>(); buf.resize(8); memcpy(buf.data(), &v, 8); }
    else if (type == "pointer" || type == "vtable") {
        uintptr_t v = jvalue.is_string() ? static_cast<uintptr_t>(std::stoull(jvalue.get<std::string>(), nullptr, 0))
                                         : static_cast<uintptr_t>(jvalue.get<uint64_t>());
        buf.resize(sizeof(uintptr_t)); memcpy(buf.data(), &v, sizeof(v));
    }
    else if (type == "vec3" || type == "vec4" || type == "matrix4") {
        const int components = type == "vec3" ? 3 : type == "vec4" ? 4 : 16;
        if (!jvalue.is_array() || jvalue.size() != static_cast<size_t>(components)) return false;
        buf.resize(components * 4);
        for (int i = 0; i < components; ++i) { float v = jvalue[i].get<float>(); memcpy(buf.data() + i * 4, &v, 4); }
    }
    else if (type == "bytes") { buf = HexToBytes(jvalue.get<std::string>()); }
    else return false;

    return memory::WriteBytes(address, buf);
}

} // namespace

void Init() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_initialized) return;
    g_initialized = true;

    for (const auto& item : project::GetStructDefs()) {
        std::string name = item.value("name", std::string(""));
        if (name.empty()) continue;
        std::vector<Field> fields;
        for (const auto& fj : item.value("fields", json::array())) fields.push_back(FieldFromJson(fj));
        g_structs[name] = fields;
    }
}

bool Define(const std::string& name, const std::vector<Field>& fields) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_structs[name] = fields;
    SyncToProject();
    return true;
}

bool Remove(const std::string& name) {
    std::lock_guard<std::mutex> lock(g_mutex);
    bool removed = g_structs.erase(name) > 0;
    if (removed) SyncToProject();
    return removed;
}

std::vector<StructDef> List() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<StructDef> out;
    for (auto& [name, fields] : g_structs) out.push_back(StructDef{name, fields});
    return out;
}

bool Read(const std::string& name, uintptr_t baseAddress, json& outFields, json& outErrors) {
    std::vector<Field> fields;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_structs.find(name);
        if (it == g_structs.end()) return false;
        fields = it->second;
    }

    outFields = json::object();
    outErrors = json::object();
    for (const auto& f : fields) {
        bool ok = false;
        json v = ReadTypedValue(baseAddress + static_cast<uintptr_t>(f.offset), f.type, f.count, ok);
        if (ok) outFields[f.name] = v; else outErrors[f.name] = "read_failed";
    }
    return true;
}

bool Write(const std::string& name, uintptr_t baseAddress, const json& values, json& outErrors) {
    std::vector<Field> fields;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_structs.find(name);
        if (it == g_structs.end()) return false;
        fields = it->second;
    }

    outErrors = json::object();
    for (auto it = values.begin(); it != values.end(); ++it) {
        const std::string& fieldName = it.key();
        auto fieldIt = std::find_if(fields.begin(), fields.end(), [&](const Field& f) { return f.name == fieldName; });
        if (fieldIt == fields.end()) { outErrors[fieldName] = "unknown_field"; continue; }
        bool ok = WriteTypedValue(baseAddress + static_cast<uintptr_t>(fieldIt->offset), fieldIt->type, it.value());
        if (!ok) outErrors[fieldName] = "write_failed";
    }
    return true;
}

} // namespace structs
