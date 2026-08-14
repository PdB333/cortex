#include "scripting/lua_engine.h"
#include "config.h"
#include "memory/memory.h"
#include "process/address.h"
#include "process/modules.h"
#include "overlay/overlay.h"

#include <nlohmann/json.hpp>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {
int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void TestLibrariesAreClosed() {
    const auto result = scripting::Exec(
        "return tostring(os == nil) .. ':' .. tostring(io == nil) .. ':' .. "
        "tostring(package == nil) .. ':' .. tostring(debug == nil) .. ':' .. tostring(dofile == nil)",
        1000);
    Check(result.ok, "sandbox inspection script executes");
    Check(result.result == "true:true:true:true:true", "filesystem/process/debug libraries stay unavailable");
}

void TestTimeout() {
    const auto result = scripting::Exec("while true do end", 25);
    Check(!result.ok && result.error.find("script_timeout") != std::string::npos,
          "instruction hook interrupts an infinite script");
}

void TestBudgets() {
    std::string oversized(256u * 1024u + 1u, ' ');
    const auto tooLarge = scripting::Exec(oversized, 1000);
    Check(!tooLarge.ok && tooLarge.error == "script_too_large", "script size budget is enforced");

    const auto timeout = scripting::Exec("return 1", 60001);
    Check(!timeout.ok && timeout.error == "timeout_out_of_range", "timeout budget is bounded");

    const auto read = scripting::Exec("local _,e=cortex.memory.read_bytes(1,1048577); return e", 1000);
    Check(read.ok && read.result == "read_size_out_of_range", "bulk-read budget is enforced before backend access");
}
}

namespace memory {
bool ReadBytes(uintptr_t, size_t, std::vector<uint8_t>&) { return false; }
bool WriteBytes(uintptr_t, const std::vector<uint8_t>&) { return false; }
std::optional<std::string> ReadString(uintptr_t, size_t) { return std::nullopt; }
}

namespace process {
uintptr_t ResolveAddress(const nlohmann::json& j, std::string* outErr) {
    if (j.is_number_integer() || j.is_number_unsigned()) return static_cast<uintptr_t>(j.get<uint64_t>());
    if (outErr) *outErr = "invalid_address";
    return 0;
}
std::string DescribeAddress(uintptr_t address) { return std::to_string(address); }
uintptr_t GetModuleBase(const std::string&) { return 0; }
std::vector<ModuleInfo> ListModules() { return {}; }
}

namespace overlay {
void LogApiCall(const std::string&) {}
}

namespace config {
Config Load() { return {}; }
std::string GetModuleDir() { return "."; }
}

int main() {
    TestLibrariesAreClosed();
    TestTimeout();
    TestBudgets();
    if (failures) {
        std::cerr << failures << " Lua sandbox test(s) failed\n";
        return 1;
    }
    std::cout << "PASS: Lua sandbox libraries, timeout, and resource budgets\n";
    return 0;
}
