#define CORTEX_DIAGNOSTICS_TESTING 1

#include "../core/diagnostics/registry.h"
#include "../core/diagnostics/symbolizer.h"
#include "../core/symbols/symbols.h"

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

std::string ReadFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::string ExecutablePath() {
    char path[32768]{};
    const DWORD length = GetModuleFileNameA(nullptr, path, static_cast<DWORD>(sizeof(path)));
    return std::string(path, length);
}

std::string DirectoryOf(const std::string& path) {
    const size_t position = path.find_last_of("\\/");
    return position == std::string::npos ? std::string(".") : path.substr(0, position);
}

__declspec(noinline) int M3SymbolTarget(int value) {
    volatile int adjusted = value + 7;
    return adjusted;
}

CortexDiagModInfo TestModInfo(const std::string& pdbPath) {
    static std::string storedPdb;
    storedPdb = pdbPath;
    CortexDiagModInfo info{};
    info.struct_size = sizeof(info);
    info.abi_version = CORTEX_DIAG_ABI_VERSION;
    info.module = GetModuleHandleA(nullptr);
    info.name = "DiagnosticsM3Test";
    info.version = "3.0.0";
    info.author = "Cortex tests";
    info.git_commit = "m3-test-commit";
    info.build_id = "registered-test-build";
    info.source_root = ".";
    info.symbol_path = storedPdb.c_str();
    return info;
}

void SetInstruction(CONTEXT& context, uintptr_t address) {
#ifdef _WIN64
    context.Rip = static_cast<DWORD64>(address);
#else
    context.Eip = static_cast<DWORD>(address);
#endif
}

void TestPeIdentity(const std::string& executable) {
    const symbols::ModuleIdentity identity = symbols::InspectModule(executable);
    Check(identity.validPe, "the test executable should be recognized as PE");
    Check(identity.imageSize != 0, "PE identity should include image size");
    Check(identity.timeDateStamp != 0, "PE identity should include timestamp");
    Check(!identity.buildId.empty(), "PE identity should produce a stable build id");
    Check(identity.hasCodeView, "MSVC /Zi build should expose an RSDS CodeView record");
    Check(!identity.pdbGuid.empty() && identity.pdbAge != 0,
          "CodeView identity should include PDB GUID and age");
}

void TestPdbResolution(const std::string& executable, const std::string& pdbPath) {
    const auto identity = symbols::InspectModule(executable);
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    const auto loaded = symbols::LoadModule(base, identity.imageSize, executable, pdbPath);
    Check(loaded.loaded, "DbgHelp should load the current test module");
    Check(loaded.hasSymbols, "DbgHelp should load symbols from the generated PDB");
    Check(loaded.exactMatch, "loaded PDB GUID and age should exactly match the PE");
    Check(loaded.verification == "pdb_guid_age_match",
          "PDB verification should report a GUID/age match");

    const uintptr_t address = reinterpret_cast<uintptr_t>(&M3SymbolTarget);
    const auto location = symbols::ResolveDetailed(address);
    Check(location.has_value(), "detailed resolution should return a location");
    if (location) {
        Check(location->hasSymbol && location->symbolName.find("M3SymbolTarget") != std::string::npos,
              "PDB resolution should return the target function name");
        Check(location->hasLine && location->line != 0,
              "PDB resolution should return source line information");
        Check(location->exactSymbols, "resolved location should retain exact-match state");
        Check(location->moduleRva == address - base,
              "resolved location should retain module RVA");
    }
}

void TestStackCapture() {
    CONTEXT context{};
    RtlCaptureContext(&context);
    SetInstruction(context, reinterpret_cast<uintptr_t>(&M3SymbolTarget));
    std::array<symbols::StackFrame, 16> frames{};
    symbols::StackCapture capture{};
    const size_t count = symbols::CaptureStack(context, GetCurrentThread(),
                                               frames.data(), frames.size(), &capture);
    Check(count >= 1, "stack capture should always retain the exception instruction");
    Check(capture.lockAcquired, "stack capture should acquire the DbgHelp lock in tests");
    Check(frames[0].hasSymbol && frames[0].symbolName.find("M3SymbolTarget") != std::string::npos,
          "first stack frame should resolve the synthetic crash instruction");
}

void TestCrashArtifacts(const std::string& pdbPath) {
    diagnostics::testing::ResetRegistry();
    CortexDiagModInfo info = TestModInfo(pdbPath);
    Check(CortexDiagRegisterMod(&info) != FALSE, "M3 test mod registration should succeed");
    const uint64_t scope = CortexDiagScopeEnter("PlayerUpdateDetour", "player_hook.cpp", 87);
    CortexDiagValuePointer("player", reinterpret_cast<void*>(0x12345678));
    CortexDiagValueText("weapon", "nullptr");

    char temp[MAX_PATH]{};
    GetTempPathA(MAX_PATH, temp);
    char directory[MAX_PATH]{};
    std::snprintf(directory, sizeof(directory), "%scortex_diag_m3_%lu",
                  temp, static_cast<unsigned long>(GetCurrentProcessId()));
    CreateDirectoryA(directory, nullptr);

    EXCEPTION_RECORD record{};
    CONTEXT context{};
    RtlCaptureContext(&context);
    record.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    record.ExceptionAddress = reinterpret_cast<void*>(&M3SymbolTarget);
    record.NumberParameters = 2;
    record.ExceptionInformation[0] = 0;
    record.ExceptionInformation[1] = 0x28;
    SetInstruction(context, reinterpret_cast<uintptr_t>(&M3SymbolTarget));
    EXCEPTION_POINTERS pointers{&record, &context};

    Check(diagnostics::WriteSymbolizedCrash(directory, &pointers),
          "symbolized crash artifacts should be written");
    const std::string stackPath = std::string(directory) + "\\stack.json";
    const std::string buildPath = std::string(directory) + "\\build_info.json";
    const std::string reportPath = std::string(directory) + "\\report.txt";
    const std::string stack = ReadFile(stackPath);
    const std::string builds = ReadFile(buildPath);
    const std::string report = ReadFile(reportPath);

    Check(stack.find("M3SymbolTarget") != std::string::npos,
          "stack.json should contain the resolved function");
    Check(stack.find("\"exact_symbols\":true") != std::string::npos,
          "stack.json should mark exact PDB matches");
    Check(builds.find("pdb_guid_age_match") != std::string::npos,
          "build_info.json should contain verification evidence");
    Check(report.find("PlayerUpdateDetour") != std::string::npos,
          "report.txt should include active mod scopes");
    Check(report.find("weapon = nullptr") != std::string::npos,
          "report.txt should include recent typed values");
    Check(report.find("M3SymbolTarget") != std::string::npos,
          "report.txt should include the resolved crash function");

    CortexDiagScopeExit(scope);
    DeleteFileA(stackPath.c_str());
    DeleteFileA(buildPath.c_str());
    DeleteFileA(reportPath.c_str());
    RemoveDirectoryA(directory);
}

} // namespace

int main() {
    const std::string executable = ExecutablePath();
    const std::string directory = DirectoryOf(executable);
    const std::string pdbPath = directory + "\\diagnostics_m3_tests.pdb";
    symbols::Init(directory, true);

    std::puts("[m3] PE identity");
    TestPeIdentity(executable);
    std::puts("[m3] PDB resolution");
    TestPdbResolution(executable, pdbPath);
    std::puts("[m3] stack capture");
    TestStackCapture();
    std::puts("[m3] crash artifacts");
    TestCrashArtifacts(pdbPath);

    symbols::Shutdown();
    diagnostics::testing::ResetRegistry();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d milestone 3 test(s) failed\n", g_failures);
        return 1;
    }
    Check(M3SymbolTarget(5) == 12, "symbol target should remain callable");
    std::puts("diagnostics milestone 3 tests passed");
    return g_failures == 0 ? 0 : 1;
}
