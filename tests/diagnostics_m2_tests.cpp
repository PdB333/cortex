#define CORTEX_DIAGNOSTICS_TESTING 1

#include "../core/diagnostics/registry.h"

#include <windows.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace {
int g_failures = 0;

void Check(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++g_failures;
}

void Stage(const char* name) {
    std::printf("RUN: %s\n", name);
    std::fflush(stdout);
}

std::string ReadFile(const char* path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

CortexDiagModInfo TestModInfo() {
    CortexDiagModInfo info{};
    info.struct_size = sizeof(info);
    info.abi_version = CORTEX_DIAG_ABI_VERSION;
    info.module = GetModuleHandleA(nullptr);
    info.name = "DiagnosticsTestMod";
    info.version = "2.0.0";
    info.author = "Cortex tests";
    info.git_commit = "abcdef123456";
    info.build_id = "m2-test-build";
    info.source_root = "C:\\src\\diagnostics-test";
    info.symbol_path = "C:\\symbols\\diagnostics-test.pdb";
    return info;
}

void TestModRegistration() {
    diagnostics::testing::ResetRegistry();
    CortexDiagModInfo info = TestModInfo();
    Check(CortexDiagRegisterMod(&info) != FALSE, "manual mod registration should succeed");

    static std::array<diagnostics::ModSnapshot, diagnostics::kMaxRegisteredMods> mods{};
    const size_t count = diagnostics::SnapshotMods(mods.data(), mods.size());
    Check(count == 1, "one manually registered mod should be visible");
    Check(std::strcmp(mods[0].name, "DiagnosticsTestMod") == 0,
          "registered mod should retain its name");
    Check(std::strcmp(mods[0].version, "2.0.0") == 0,
          "registered mod should retain its version");
    Check(std::strcmp(mods[0].gitCommit, "abcdef123456") == 0,
          "registered mod should retain its commit");
    Check(mods[0].base != 0 && mods[0].imageSize != 0,
          "base and image size should be detected automatically");
    Check(mods[0].path[0] != '\0', "module path should be detected automatically");

    diagnostics::ModSnapshot found{};
    Check(diagnostics::FindModForAddress(mods[0].base + 1, found),
          "address lookup should resolve the registered mod");
    Check(found.id == mods[0].id, "address lookup should return the same mod id");
}

void TestScopesAndValues() {
    diagnostics::testing::ResetRegistry();
    CortexDiagModInfo info = TestModInfo();
    Check(CortexDiagRegisterMod(&info) != FALSE, "mod registration should succeed before scope test");

    const uint64_t outer = CortexDiagScopeEnter("PlayerUpdateDetour", "player_hook.cpp", 87);
    const uint64_t inner = CortexDiagScopeEnter("UpdateWeapon", "weapon.cpp", 42);
    Check(outer != 0 && inner != 0, "nested scopes should receive ids");

    CortexDiagValuePointer("player", reinterpret_cast<void*>(0x12345678));
    CortexDiagValueInt64("health", 74);
    CortexDiagValueText("weapon", "nullptr");
    CortexDiagValueBool("alive", TRUE);

    static std::array<diagnostics::ScopeSnapshot, diagnostics::kMaxActiveScopes> scopes{};
    const size_t scopeCount = diagnostics::SnapshotScopes(scopes.data(), scopes.size());
    Check(scopeCount == 2, "both active scopes should be captured");
    Check(scopes[0].depth == 0 && scopes[1].depth == 1,
          "nested scopes should preserve depth");
    Check(scopes[1].parentId == scopes[0].id, "inner scope should reference its parent");

    static std::array<diagnostics::ValueSnapshot, diagnostics::kValueCapacity> values{};
    uint64_t dropped = 0;
    const size_t valueCount = diagnostics::SnapshotValues(values.data(), values.size(), &dropped);
    Check(valueCount == 4 && dropped == 0, "typed values should be retained");
    Check(values[0].type == diagnostics::DiagnosticValueType::Pointer &&
          values[0].pointerValue == 0x12345678,
          "pointer value should be captured");
    Check(values[1].int64Value == 74, "integer value should be captured");
    Check(std::strcmp(values[2].textValue, "nullptr") == 0,
          "text value should be captured");
    Check(values[3].boolValue, "boolean value should be captured");
    Check(values[0].scopeId == inner, "values should attach to the innermost active scope");

    CortexDiagScopeExit(inner);
    CortexDiagScopeExit(outer);
    Check(diagnostics::SnapshotScopes(scopes.data(), scopes.size()) == 0,
          "exited scopes should not remain active");
}

void TestRegistryFiles() {
    diagnostics::testing::ResetRegistry();
    CortexDiagModInfo info = TestModInfo();
    Check(CortexDiagRegisterMod(&info) != FALSE, "mod registration should succeed before file test");
    const uint64_t scope = CortexDiagScopeEnter("CrashScope", "crash.cpp", 12);
    CortexDiagValueText("state", "before crash");

    char temp[MAX_PATH]{};
    GetTempPathA(MAX_PATH, temp);
    char directory[MAX_PATH]{};
    std::snprintf(directory, sizeof(directory), "%scortex_diag_m2_%lu",
                  temp, static_cast<unsigned long>(GetCurrentProcessId()));
    CreateDirectoryA(directory, nullptr);

    Check(diagnostics::WriteRegistrySnapshots(directory),
          "registry snapshot files should be written");

    char modsPath[MAX_PATH]{}, scopesPath[MAX_PATH]{}, valuesPath[MAX_PATH]{};
    std::snprintf(modsPath, sizeof(modsPath), "%s\\mods.json", directory);
    std::snprintf(scopesPath, sizeof(scopesPath), "%s\\scopes.json", directory);
    std::snprintf(valuesPath, sizeof(valuesPath), "%s\\values.json", directory);
    const std::string mods = ReadFile(modsPath);
    const std::string scopes = ReadFile(scopesPath);
    const std::string values = ReadFile(valuesPath);
    Check(mods.find("DiagnosticsTestMod") != std::string::npos,
          "mods.json should contain registered metadata");
    Check(scopes.find("CrashScope") != std::string::npos,
          "scopes.json should contain active scopes");
    Check(values.find("before crash") != std::string::npos,
          "values.json should contain recent typed values");

    CortexDiagScopeExit(scope);
    DeleteFileA(modsPath);
    DeleteFileA(scopesPath);
    DeleteFileA(valuesPath);
    RemoveDirectoryA(directory);
}

void TestUnregister() {
    diagnostics::testing::ResetRegistry();
    CortexDiagModInfo info = TestModInfo();
    Check(CortexDiagRegisterMod(&info) != FALSE, "mod registration should succeed");
    CortexDiagUnregisterMod(info.module);
    static std::array<diagnostics::ModSnapshot, diagnostics::kMaxRegisteredMods> mods{};
    Check(diagnostics::SnapshotMods(mods.data(), mods.size()) == 0,
          "unregistered mod should disappear");
}
} // namespace

int main() {
    Stage("mod registration");
    TestModRegistration();
    Stage("scopes and typed values");
    TestScopesAndValues();
    Stage("registry JSON files");
    TestRegistryFiles();
    Stage("unregister");
    TestUnregister();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d milestone 2 diagnostic test(s) failed\n", g_failures);
        return 1;
    }
    std::puts("diagnostics milestone 2 tests passed");
    return 0;
}
