// Offline native code analysis and annotations (step 4d).
//
//   cortex_native_code_tests <test target x64 or x86 exe>
//
// The fixture is the repository's own test target, compiled by the same
// build. Function ranges from the exception directory are checked against
// the runtime's own unwind reader (mcp_bridge/pe_unwind.h); the CFG of
// CortexDemoApplyDamage against what the source does (the health write);
// the call graph against objdump in tests/native_static_e2e.py.

#include "../mcp_bridge/artifact_tools.h"
#include "../mcp_bridge/pe_unwind.h"

#include <chrono>
#include <iostream>
#include <random>

namespace {

namespace fs = std::filesystem;
namespace native = cortex::artifacts::native;
using namespace cortex::artifacts;
using json = nlohmann::json;

int failures = 0;

#define CHECK(condition)                                                              \
    do {                                                                              \
        if (!(condition)) {                                                           \
            ++failures;                                                               \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #condition \
                      << std::endl;                                                   \
        }                                                                             \
    } while (false)

uint32_t Rva(const json& value) { return static_cast<uint32_t>(std::stoul(value.get<std::string>(), nullptr, 16)); }

const json* FindFunction(const json& functions, const std::string& name) {
    for (const auto& function : functions["functions"])
        if (function["name"] == name) return &function;
    return nullptr;
}

void TestTarget(const std::string& path) {
    std::string error;
    const auto source = OpenFile(path, &error);
    CHECK(source != nullptr);
    if (!source) return;
    PeFile pe;
    CHECK(ParsePe(*source, pe, error));
    native::CodeIndex index;
    CHECK(native::BuildIndex(*source, pe, index, error));
    const bool x64 = pe.machine == 0x8664;
    CHECK(index.x64 == x64);

    const json functions = native::FunctionsJson(index, "", 1 << 20, {});
    CHECK(functions["total"].get<size_t>() > 100 && functions["by_source"]["entry"] == 1);
    for (const char* name : {"CortexDemoApplyDamage", "CortexNativeAdd", "CortexBreakpointCanary"}) {
        const json* function = FindFunction(functions, name);
        CHECK(function != nullptr);
        if (!function) continue;
        const auto& sources = (*function)["sources"];
        CHECK(std::find(sources.begin(), sources.end(), "export") != sources.end());
        if (x64) CHECK(std::find(sources.begin(), sources.end(), "pdata") != sources.end() && (*function)["size_source"] == "pdata");
    }
    // Data exports are not functions.
    CHECK(FindFunction(functions, "g_cortex_demo_actor") == nullptr);

    // x64: every pdata range equals what the runtime's unwind reader finds
    // for an address inside it.
    if (x64) {
        CHECK(index.pdataEntries > 100);
        const auto read = [&](uint64_t address, size_t count, std::vector<uint8_t>& out) {
            const uint32_t rva = static_cast<uint32_t>(address - pe.imageBase);
            uint64_t offset = rva;
            uint32_t available = static_cast<uint32_t>(count);
            if (rva >= pe.sizeOfHeaders && !pe.RvaToOffset(rva, offset, &available)) return false;
            out = source->ReadSome(offset, std::min<size_t>(count, available));
            return out.size() == count;
        };
        size_t checked = 0, matched = 0;
        for (const auto& function : index.functions) {
            if (!function.sizeFromPdata || checked >= 400) continue;
            ++checked;
            const json unwind = cortex::test::pe_unwind::Find(pe.imageBase, pe.sizeOfImage, pe.imageBase + function.rva + function.size / 2, read);
            if (unwind.value("status", "") == "unwind_range" &&
                unwind["begin_rva"] == cortex::test::pe_unwind::Hex(function.rva) &&
                unwind["end_rva"] == cortex::test::pe_unwind::Hex(function.rva + function.size))
                ++matched;
            else if (checked - matched <= 3)
                std::cerr << "pdata " << function.rva << " vs " << unwind.dump() << "\n";
        }
        CHECK(checked > 100 && matched == checked);
    }

    // CortexDemoApplyDamage: called, and its CFG holds the health write
    // (actor+4 -= damage), as the source says.
    uint32_t damage = 0;
    CHECK(native::ResolveLocation(index, "CortexDemoApplyDamage", {}, damage));
    const json callers = native::XrefsJson(index, damage, 100, {});
    bool called = false;
    for (const auto& ref : callers["references"]) called = called || ref["type"] == "call";
    CHECK(called && callers["target_function"] == "CortexDemoApplyDamage");
    const json cfg = native::CfgJson(*source, pe, index, damage, {damage}, 100, {});
    CHECK(cfg["ok"] == true && cfg["blocks"].size() >= 2 && cfg["coverage"]["blocks_covered"] == 1);
    bool healthWrite = false, ends = false;
    for (const auto& block : cfg["blocks"])
        for (const auto& instruction : block["instructions"]) {
            const std::string text = instruction["text"];
            healthWrite = healthWrite || (text.find("+0x04]") != std::string::npos && text.find("[") < text.find(",") &&
                                          (text.rfind("mov", 0) == 0 || text.rfind("sub", 0) == 0));
            ends = ends || text == "ret";
        }
    CHECK(healthWrite && ends);

    // The data export is read or written by code.
    uint32_t actor = 0;
    const json exports = PeExports(*source, pe, {}, *std::make_unique<bool>(false));
    for (const auto& row : exports["functions"])
        if (row["name"] == "g_cortex_demo_actor") actor = Rva(row["rva"]);
    CHECK(actor != 0);
    const json dataRefs = native::XrefsJson(index, actor, 100, {});
    bool accessed = false;
    for (const auto& ref : dataRefs["references"])
        accessed = accessed || ref["type"] == "read" || ref["type"] == "write" || ref["type"] == "address";
    CHECK(accessed);

    // Strings: found in data sections; those referenced by code point back
    // to them. (Not every literal has a static reference: compilers also
    // materialise addresses in ways a sweep does not see, as objdump agrees
    // for "--e2e-manifest" here.)
    const json manifest = native::StringsJson(index, "--e2e-manifest", 10);
    CHECK(manifest["strings"].size() == 1 && manifest["strings"][0]["text"] == "--e2e-manifest");
    const json strings = native::StringsJson(index, "", 1 << 20);
    size_t referenced = 0;
    for (const auto& row : strings["strings"]) {
        if (row["references"].empty()) continue;
        if (++referenced > 20) continue;
        const json back = native::XrefsJson(index, Rva(row["rva"]), 10, {});
        CHECK(back["target_string"] == row["text"] && back["total"].get<size_t>() == row["references"].size());
    }
    CHECK(referenced > 20);

    // Location forms.
    uint32_t rva = 0;
    CHECK(native::ResolveLocation(index, "va:" + cortex::test::pe_unwind::Hex(pe.imageBase + damage), {}, rva) && rva == damage);
    CHECK(native::ResolveLocation(index, "rva:" + cortex::test::pe_unwind::Hex(damage), {}, rva) && rva == damage);
    char sub[32];
    std::snprintf(sub, sizeof(sub), "sub_%X", damage);
    CHECK(native::ResolveLocation(index, sub, {}, rva) && rva == damage);
    CHECK(native::ResolveLocation(index, "renamed", {{"renamed", damage}}, rva) && rva == damage);
    CHECK(!native::ResolveLocation(index, "nope", {}, rva));
    CHECK(native::CfgJson(*source, pe, index, 0x10, {}, 10, {})["error"] == "not_in_executable_section");

    // Fuzzing: damaged headers and code never crash the index.
    std::mt19937 random(77);
    const auto original = source->ReadSome(0, static_cast<size_t>(source->Size()));
    for (int round = 0; round < 60; ++round) {
        auto bytes = original;
        for (int e = 0; e < 64; ++e) {
            const size_t at = round % 2 ? random() % 1024 : random() % bytes.size();
            bytes[at] = static_cast<uint8_t>(random());
        }
        const auto mutated = FromBytes(bytes, "fuzz.exe");
        PeFile fuzzPe;
        native::CodeIndex fuzzIndex;
        if (ParsePe(*mutated, fuzzPe, error) && native::BuildIndex(*mutated, fuzzPe, fuzzIndex, error) &&
            !fuzzIndex.functions.empty())
            (void)native::CfgJson(*mutated, fuzzPe, fuzzIndex, fuzzIndex.functions[random() % fuzzIndex.functions.size()].rva,
                                  {}, 50, {});
    }
}

void TestAnnotations(const std::string& path) {
    const fs::path directory =
        fs::temp_directory_path() / ("cortex-annotations-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    AnnotationStore store(directory);
    const auto source = OpenFile(path);
    const std::string sha = Sha256Of(*source);
    json state = store.Get(sha);
    CHECK(state["ok"] == true && state["revision"] == 0 && state["annotations"].empty());
    CHECK(store.Put(sha, "rva:0x14c0", "name", "apply_damage", "agent", 0)["revision"] == 1);
    CHECK(store.Put(sha, "rva:0x14c0", "comment", "writes health at +4", "desktop", 1)["revision"] == 2);
    CHECK(store.Put(sha, "rva:0x14c0", "name", "stale", "agent", 1)["error"] == "revision_conflict");
    CHECK(store.Put(sha, "../x", "name", "bad", "agent", -1)["error"] == "invalid_location");
    CHECK(store.Put("../../etc", "rva:0x1", "name", "bad", "agent", -1)["error"] == "invalid_sha256");
    CHECK(store.Put(sha, "rva:0x1", "name", "two\nlines", "agent", -1)["error"] == "invalid_text");
    CHECK(store.Put(sha, "token:0x06000004", "comment", "Heal", "agent", -1)["ok"] == true);
    CHECK(store.Put(sha, "js:app.asar!main.js:25:16", "comment", "IPC handler", "agent", -1)["ok"] == true);
    CHECK(store.NativeNames(sha) == (std::map<uint32_t, std::string>{{0x14c0, "apply_damage"}}));
    // Persisted: a new store over the same folder reads the same records.
    AnnotationStore reopened(directory);
    CHECK(reopened.Get(sha)["revision"] == 4 && reopened.Get(sha)["annotations"].size() == 4);
    CHECK(reopened.Put(sha, "rva:0x14c0", "comment", "", "agent", 4)["removed"] == true);
    CHECK(reopened.Put(sha, "rva:0x9999", "comment", "", "agent", -1)["error"] == "annotation_not_found");

    // Through the tools: the name shows in the catalog; writing needs mutation_permission.
    const auto& tools = mcp_bridge::artifacts::HandleCode;
    CHECK(tools("cortex_artifact_annotate", {{"location", "rva:0x1"}, {"kind", "name"}, {"text", "x"}}, *source, &store, "agent")["error"] ==
          "mutation_permission_required");
    json listed = tools("cortex_artifact_functions", {{"filter", "apply_damage"}}, *source, &store, "agent");
    if (FindFunction(listed, "apply_damage") == nullptr) {
        // Only the x64 fixture has a function at 0x14c0.
        CHECK(listed["matched"] == 0);
    } else {
        CHECK((*FindFunction(listed, "apply_damage"))["annotated"] == true);
        CHECK(tools("cortex_artifact_cfg", {{"function", "apply_damage"}}, *source, &store, "agent")["name"] == "apply_damage");
    }
    std::error_code ignored;
    fs::remove_all(directory, ignored);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: cortex_native_code_tests <test target exe>" << std::endl;
        return 2;
    }
    TestTarget(argv[1]);
    TestAnnotations(argv[1]);
    if (failures) {
        std::cerr << failures << " native code check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "native code and annotation tests passed" << std::endl;
    return 0;
}
