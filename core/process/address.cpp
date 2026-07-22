#include "address.h"
#include "modules.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <iomanip>

namespace process {

namespace {

bool IEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return true;
}

uint64_t ParseIntFlexible(const std::string& s, bool* ok) {
    if (ok) *ok = false;
    try {
        size_t consumed = 0;
        uint64_t v = std::stoull(s, &consumed, 0);
        if (consumed != s.size()) return 0;
        if (ok) *ok = true;
        return v;
    } catch (...) { return 0; }
}

// Parses "module.ext+rva" style strings. Returns false if not that shape.
bool ParseModuleRva(const std::string& s, std::string& modOut, uint64_t& rvaOut) {
    size_t plus = s.find('+');
    if (plus == std::string::npos) return false;
    modOut = s.substr(0, plus);
    std::string rvaStr = s.substr(plus + 1);
    // trim
    while (!modOut.empty() && std::isspace((unsigned char)modOut.back())) modOut.pop_back();
    while (!rvaStr.empty() && std::isspace((unsigned char)rvaStr.front())) rvaStr.erase(0, 1);
    bool ok = false;
    rvaOut = ParseIntFlexible(rvaStr, &ok);
    return ok && !modOut.empty();
}

} // namespace

uintptr_t GetModuleBase(const std::string& name) {
    auto mods = ListModules();
    for (const auto& m : mods) if (IEquals(m.name, name)) return m.base;
    return 0;
}

uintptr_t ResolveAddress(const nlohmann::json& j, std::string* outErr) {
    if (j.is_number_integer() || j.is_number_unsigned()) {
        return (uintptr_t)j.get<uint64_t>();
    }
    if (j.is_object()) {
        if (!j.contains("module") || !j.contains("rva")) {
            if (outErr) *outErr = "expected {module, rva}";
            return 0;
        }
        std::string mod = j["module"].get<std::string>();
        uintptr_t base = GetModuleBase(mod);
        if (!base) { if (outErr) *outErr = "module_not_loaded:" + mod; return 0; }
        const auto& r = j["rva"];
        uint64_t rva = 0;
        if (r.is_number_integer() || r.is_number_unsigned()) rva = r.get<uint64_t>();
        else {
            bool ok = false;
            rva = ParseIntFlexible(r.get<std::string>(), &ok);
            if (!ok) { if (outErr) *outErr = "invalid_rva"; return 0; }
        }
        return base + (uintptr_t)rva;
    }
    if (j.is_string()) {
        std::string s = j.get<std::string>();
        // module+rva form?
        std::string mod;
        uint64_t rva = 0;
        if (ParseModuleRva(s, mod, rva)) {
            uintptr_t base = GetModuleBase(mod);
            if (!base) { if (outErr) *outErr = "module_not_loaded:" + mod; return 0; }
            return base + (uintptr_t)rva;
        }
        bool ok = false;
        uint64_t v = ParseIntFlexible(s, &ok);
        if (!ok) { if (outErr) *outErr = "invalid_address"; return 0; }
        return (uintptr_t)v;
    }
    if (outErr) *outErr = "unsupported_address_form";
    return 0;
}

std::string DescribeAddress(uintptr_t addr) {
    auto mods = ListModules();
    for (const auto& m : mods) {
        if (addr >= m.base && addr < m.base + m.size) {
            std::ostringstream ss;
            ss << m.name << "+0x" << std::hex << (addr - m.base);
            return ss.str();
        }
    }
    std::ostringstream ss; ss << "0x" << std::hex << addr;
    return ss.str();
}

} // namespace process
