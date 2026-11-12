// .NET metadata, CIL and ZIP containers (step 4c).
//
//   cortex_dotnet_tests <tests/fixtures/dotnet directory>
//
// Cortex.Fixture.dll is real C# compiled by the .NET SDK; every expected
// value comes from System.Reflection.Metadata and the runtime's OpCodes
// table (Oracle/Program.cs), never from Cortex itself. The NuGet package
// comes from `dotnet pack`, zip-cases.zip from Python's zipfile.

#include "artifacts/artifact.h"
#include "artifacts/cil.h"
#include "artifacts/dotnet_metadata.h"
#include "artifacts/dotnet_provider.h"
#include "artifacts/inflate.h"
#include "artifacts/pe_provider.h"
#include "artifacts/zip_provider.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <sstream>

namespace {

namespace fs = std::filesystem;
namespace net = cortex::artifacts::dotnet;
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

fs::path g_dir;

std::vector<uint8_t> ReadBytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

// Reports the first differences between two JSON values under `what`.
bool Same(const json& got, const json& expected, const std::string& what) {
    if (got == expected) return true;
    static int shown = 0;
    if (++shown <= 12) std::cerr << "differs: " << what << "\n  got      " << got.dump() << "\n  expected " << expected.dump() << "\n";
    return false;
}

void TestOpCodes(const json& expected) {
    std::map<uint16_t, const net::OpCodeInfo*> mine;
    for (const auto& info : net::OpCodeTable()) mine[info.value] = &info;
    size_t compared = 0;
    for (const auto& op : expected["opcodes"]) {
        const uint16_t value = static_cast<uint16_t>(op["value"].get<int>());
        const auto it = mine.find(value);
        CHECK(it != mine.end());
        if (it == mine.end()) {
            std::cerr << "missing opcode " << op.dump() << "\n";
            continue;
        }
        CHECK(Same(json{it->second->name, net::OperandName(it->second->operand)}, json{op["name"], op["operand"]},
                   "opcode " + std::to_string(value)));
        ++compared;
    }
    CHECK(compared == expected["opcodes"].size() && compared > 200);
    // Extra entries are allowed only where the runtime table has no field.
    for (const auto& [value, info] : mine) {
        bool known = false;
        for (const auto& op : expected["opcodes"]) known = known || op["value"] == value;
        if (!known) CHECK(std::string(info->name) == "no.");
    }
}

void TestMetadata(const json& expected) {
    const auto source = OpenFile((g_dir / "Cortex.Fixture.dll").u8string());
    CHECK(source != nullptr);
    if (!source) return;
    const json result = DefaultRegistry().Inspect(*source);
    CHECK(result["provider"]["name"] == "dotnet" && result["format"] == "dotnet");
    CHECK(result["capabilities"]["decompile"]["status"] == "unavailable");
    const json& net = result["data"]["dotnet"];
    CHECK(result["data"].contains("sections"));  // the PE view stays
    CHECK(Same(net["metadata_version"], expected["metadata_version"], "metadata version"));
    CHECK(Same(json{net["assembly"]["name"], net["assembly"]["version"], net["assembly"]["culture"]},
               json{expected["assembly"]["name"], expected["assembly"]["version"], expected["assembly"]["culture"]},
               "assembly"));
    CHECK(Same(net["module"], expected["module"], "module"));
    json refs = json::array();
    for (const auto& r : net["assembly_refs"]) refs.push_back({{"name", r["name"]}, {"version", r["version"]}});
    CHECK(Same(refs, expected["assembly_refs"], "assembly refs"));
    CHECK(Same(net["type_refs"], expected["type_refs"], "type refs"));
    CHECK(Same(net["member_refs"], expected["member_refs"], "member refs"));
    CHECK(Same(net["manifest_resources"], expected["manifest_resources"], "manifest resources"));
    CHECK(Same(net["pinvoke"], expected["pinvoke"], "pinvoke"));
    CHECK(net["types"].size() == expected["types"].size());
    for (size_t i = 0; i < std::min(net["types"].size(), expected["types"].size()); ++i)
        CHECK(Same(net["types"][i], expected["types"][i], "type " + expected["types"][i]["name"].get<std::string>()));
    CHECK(net["tables"]["TypeDef"] == expected["types"].size());
    CHECK(net["truncated"] == false);

    // Limits are reported, not silently applied.
    InspectOptions small;
    small.maxEntries = 2;
    const json limited = DefaultRegistry().Inspect(*source, small)["data"]["dotnet"];
    CHECK(limited["types"].size() == 2 && limited["truncated"] == true);
}

void TestMethodBodies(const json& expected) {
    const auto source = OpenFile((g_dir / "Cortex.Fixture.dll").u8string());
    if (!source) return;
    PeFile pe;
    net::Metadata metadata;
    std::string error;
    CHECK(ParsePe(*source, pe, error) && metadata.Parse(*source, pe, error));
    size_t methods = 0, instructions = 0;
    bool sawSwitch = false, sawClauses = false, sawString = false, sawGenericCall = false;
    for (const auto& body : expected["method_bodies"]) {
        const uint32_t row = std::stoul(body["token"].get<std::string>(), nullptr, 16) & 0xFFFFFF;
        std::string methodError;
        const json got = net::MethodJson(*source, pe, metadata, row, 100000, methodError);
        const std::string name = body["method"];
        CHECK(Same(got["method"], body["method"], name));
        CHECK(Same(json{got["max_stack"], got["code_size"], got["init_locals"], got["local_signature"]},
                   json{body["max_stack"], body["code_size"], body["init_locals"], body["local_signature"]},
                   name + " header"));
        CHECK(Same(got["exception_clauses"], body["exception_clauses"], name + " clauses"));
        CHECK(Same(got["instructions"], body["instructions"], name + " instructions"));
        ++methods;
        instructions += body["instructions"].size();
        for (const auto& row_ : body["instructions"]) {
            sawSwitch = sawSwitch || row_["opcode"] == "switch";
            sawString = sawString || row_["opcode"] == "ldstr";
            sawGenericCall = sawGenericCall || row_.value("operand", std::string()).find('<') != std::string::npos;
        }
        sawClauses = sawClauses || !body["exception_clauses"].empty();
    }
    CHECK(methods >= 10 && instructions > 150 && sawSwitch && sawClauses && sawString && sawGenericCall);

    // Lookup by name, by qualified name (every overload), by token.
    json found = DisassembleMethods(*source, "Cortex.Fixture.Actor::Heal", 1000);
    CHECK(found["ok"] == true && found["methods"].size() == 1 && found["methods"][0]["header"] == "fat");
    CHECK(DisassembleMethods(*source, "Classify", 1000)["methods"].size() == 1);
    const std::string token = found["methods"][0]["token"];
    CHECK(DisassembleMethods(*source, token, 1000)["methods"][0]["method"] == "Cortex.Fixture.Actor::Heal");
    CHECK(DisassembleMethods(*source, "NoSuchMethod", 1000)["error"] == "method_not_found");
    // A P/Invoke has no body, and says why.
    found = DisassembleMethods(*source, "Ticks", 1000);
    CHECK(found["methods"][0]["body"].is_null() && found["methods"][0]["reason"] == "method_has_no_body");
    // The instruction cap is reported.
    found = DisassembleMethods(*source, "ApplyDamage", 3);
    for (const auto& method : found["methods"])
        if (method.contains("instructions")) CHECK(method["instructions"].size() <= 3 && method["warning"] == "instructions_truncated");

    // Hand-made bodies: truncated operands and unknown opcodes end with a reason.
    std::string disassemblyError;
    CHECK(net::Disassemble(metadata, {0x20, 0x01}, 100, disassemblyError).empty() &&
          disassemblyError.rfind("truncated_operand", 0) == 0);
    CHECK(net::Disassemble(metadata, {0x00, 0xA6}, 100, disassemblyError).size() == 1 &&
          disassemblyError.rfind("unknown_opcode", 0) == 0);
    CHECK(net::Disassemble(metadata, {0x45, 0xFF, 0xFF, 0xFF, 0x0F}, 100, disassemblyError).empty() &&
          disassemblyError.rfind("truncated_switch", 0) == 0);
    disassemblyError.clear();
    const json floats = net::Disassemble(metadata, {0x22, 0, 0, 0, 0x3F, 0x23, 0, 0, 0, 0, 0, 0, 0xF8, 0x3F}, 100, disassemblyError);
    CHECK(floats.size() == 2 && floats[0]["operand"] == "0.5" && floats[1]["operand"] == "1.5");
}

void TestInflate() {
    // A stored block, a fixed-Huffman stream, and invalid input.
    std::vector<uint8_t> out;
    std::string error;
    const uint8_t stored[] = {0x01, 0x03, 0x00, 0xFC, 0xFF, 'a', 'b', 'c'};
    CHECK(Inflate(stored, sizeof(stored), out, 100, error) && std::string(out.begin(), out.end()) == "abc");
    const uint8_t fixedEmpty[] = {0x03, 0x00};  // final fixed block, end of block only
    CHECK(Inflate(fixedEmpty, sizeof(fixedEmpty), out, 100, error) && out.empty());
    const uint8_t badType[] = {0x07};
    CHECK(!Inflate(badType, sizeof(badType), out, 100, error) && error == "inflate_invalid_block_type");
    const uint8_t badLength[] = {0x01, 0x03, 0x00, 0x00, 0x00, 'a'};
    CHECK(!Inflate(badLength, sizeof(badLength), out, 100, error) && error == "inflate_stored_length_mismatch");
    CHECK(!Inflate(stored, sizeof(stored), out, 2, error) && error == "inflate_output_limit");
    CHECK(!Inflate(stored, 4, out, 100, error) && error == "inflate_truncated");
    CHECK(Crc32(reinterpret_cast<const uint8_t*>("123456789"), 9) == 0xCBF43926u);
}

void TestZip() {
    const auto& registry = DefaultRegistry();
    std::string error;
    const auto cases = OpenFile((g_dir / "zip-cases.zip").u8string());
    CHECK(cases != nullptr);
    if (!cases) return;
    const json result = registry.Inspect(*cases);
    CHECK(result["format"] == "zip" && result["data"]["entries"] == 6 && result["data"]["comment"] == "cortex zip cases");
    CHECK(result["data"]["crc"]["matched"] == 6 && result["data"]["crc"]["mismatches"].empty());
    const std::string text = [] {
        std::string out;
        for (int i = 0; i < 200; ++i) out += "The quick brown fox jumps over the lazy dog. ";
        return out;
    }();
    for (const char* name : {"stored.txt", "fast.txt", "best.txt"}) {
        const auto entry = registry.OpenChild(cases, name, error);
        CHECK(entry && entry->Size() == text.size());
        if (entry) {
            const auto bytes = entry->ReadSome(0, text.size());
            CHECK(std::string(bytes.begin(), bytes.end()) == text);
        }
    }
    const auto random = registry.OpenChild(cases, "random.bin", error);
    CHECK(random && random->Size() == 5000);
    if (random) {
        const auto bytes = random->ReadSome(0, 5000);
        bool same = true;
        for (size_t i = 0; i < bytes.size(); ++i) same = same && bytes[i] == static_cast<uint8_t>((i * 7919 + 13) % 251);
        CHECK(same);
    }
    const auto empty = registry.OpenChild(cases, "empty.txt", error);
    CHECK(empty && empty->Size() == 0);
    const auto utf8 = registry.OpenChild(cases, "folder/caf\xC3\xA9.txt", error);
    CHECK(utf8 && utf8->Size() == 5);
    const json children = registry.ListChildren(*cases);
    bool sawCompression = false;
    for (const auto& child : children["children"]) sawCompression = sawCompression || child.value("compression", "") == "deflate";
    CHECK(sawCompression);

    // A NuGet package from dotnet pack, and the assembly inside it.
    const auto package = OpenFile((g_dir / "Cortex.Fixture.1.4.2.nupkg").u8string());
    CHECK(package != nullptr);
    if (package) {
        const json nupkg = registry.Inspect(*package);
        CHECK(nupkg["format"] == "nupkg" && nupkg["data"]["package"]["id"] == "Cortex.Fixture" &&
              nupkg["data"]["package"]["version"] == "1.4.2");
        CHECK(nupkg["data"]["crc"]["mismatches"].empty() && nupkg["data"]["crc"]["matched"].get<int>() > 0);
        CHECK(nupkg["data"]["package"]["assemblies"] == json::array({"lib/net8.0/Cortex.Fixture.dll"}));
        const auto dll = registry.OpenChild(package, "lib/net8.0/Cortex.Fixture.dll", error);
        CHECK(dll != nullptr);
        if (dll) {
            const json inner = registry.Inspect(*dll);
            CHECK(inner["format"] == "dotnet" && inner["data"]["dotnet"]["assembly"]["name"] == "Cortex.Fixture");
            CHECK(DisassembleMethods(*dll, "Cortex.Fixture.Actor::Heal", 100)["ok"] == true);
        }
    }

    // Damaged archives: a flipped byte in deflated data, a cut directory.
    auto bytes = ReadBytes(g_dir / "zip-cases.zip");
    for (const auto& child : registry.Find("zip")->Children(*cases, {}, error))
        if (child.name == "best.txt") bytes[static_cast<size_t>(child.offset + child.storedSize / 2)] ^= 0x55;
    const auto damaged = FromBytes(bytes, "damaged.zip");
    const json damagedResult = registry.Inspect(*damaged)["data"]["crc"];
    CHECK(damagedResult["mismatches"].size() == 1 && damagedResult["mismatches"][0]["file"] == "best.txt");
    CHECK(!registry.OpenChild(damaged, "best.txt", error) && !error.empty());
    auto cut = ReadBytes(g_dir / "zip-cases.zip");
    cut.resize(cut.size() - 30);
    CHECK(registry.Inspect(*FromBytes(cut, "cut.zip"))["format"] != "zip");
    // A prefix (self-extractor stub) shifts every offset.
    auto prefixed = std::vector<uint8_t>(1000, 0x90);
    const auto original = ReadBytes(g_dir / "zip-cases.zip");
    prefixed.insert(prefixed.end(), original.begin(), original.end());
    const auto shifted = FromBytes(prefixed, "prefixed.zip");
    const json shiftedResult = registry.Inspect(*shifted)["data"];
    CHECK(shiftedResult["prefix_bytes"] == 1000 && shiftedResult["crc"]["matched"] == 6);
}

// Standalone structural WinMD fixtures generated with Python stdlib.
// They are not activated or loaded by Windows; this is ECMA-335 parsing only.
void TestWinMd() {
    const fs::path dir = g_dir / "winmd";
    const auto source = OpenFile((dir / "Cortex.WinRT.winmd").u8string());
    CHECK(source != nullptr);
    if (!source) return;
    const auto& registry = DefaultRegistry();
    const json report = registry.Inspect(*source);
    CHECK(report["ok"] == true && report["provider"]["name"] == "dotnet" && report["format"] == "winmd");
    if (report.value("ok", false) != true || report.value("format", "") != "winmd") return;
    const json& data = report["data"];
    CHECK(data["dotnet"]["assembly"]["name"] == "Cortex.WinRT");
    CHECK(data["dotnet"]["metadata_version"] == "Windows Runtime 1.2");
    CHECK(data["dotnet"]["types"].size() == 2);
    CHECK(data["dotnet"]["types"][1]["methods"][0]["name"] == "Ping");
    CHECK(data["winrt"]["metadata_version_marker"] == true);
    CHECK(data["winrt"]["winrt_type_count"] == 1);
    CHECK(data["winrt"]["public_non_winrt_type_count"] == 0);
    CHECK(data["winrt"]["types"].size() == 1);
    CHECK(data["winrt"]["types"][0]["name"] == "Cortex.WinRT.IDemo");
    CHECK(data["winrt"]["types"][0]["kind"] == "interface");
    CHECK(data["winrt"]["types"][0]["public"] == true);
    CHECK(data["winrt"]["assembly_name_matches_filename"] == true);

    // Detection follows structural evidence, not the filename alone.
    const auto original = ReadBytes(dir / "Cortex.WinRT.winmd");
    CHECK(registry.Inspect(*FromBytes(original, "renamed.dll"))["format"] == "winmd");
    const json renamed = registry.Inspect(*FromBytes(original, "different.winmd"));
    CHECK(renamed["data"]["winrt"]["assembly_name_matches_filename"] == false);
    CHECK(renamed["data"]["winrt"]["warnings"] == json::array({"assembly_name_filename_mismatch"}));
    const auto notWinrt = OpenFile((dir / "not-winrt.winmd").u8string());
    CHECK(notWinrt != nullptr);
    if (notWinrt) {
        const json ordinary = registry.Inspect(*notWinrt);
        CHECK(ordinary["ok"] == true && ordinary["format"] == "dotnet");
        CHECK(!ordinary["data"].contains("winrt"));
    }

    InspectOptions limited;
    limited.maxEntries = 0;
    const json capped = registry.Inspect(*source, limited);
    CHECK(capped["data"]["winrt"]["types"].empty());
    CHECK(capped["data"]["winrt"]["types_truncated"] == true);

    // A missing version marker is reported, without discarding real WinRT
    // flags. A missing CLR header must not pass as a WinMD.
    auto corrupted = original;
    const std::string marker = "Windows Runtime 1.2";
    auto found = std::search(corrupted.begin(), corrupted.end(), marker.begin(), marker.end());
    CHECK(found != corrupted.end());
    if (found != corrupted.end()) {
        *found = 'X';
        const json missingVersion = registry.Inspect(*FromBytes(corrupted, "Cortex.WinRT.winmd"));
        CHECK(missingVersion["format"] == "winmd");
        CHECK(missingVersion["data"]["winrt"]["metadata_version_marker"] == false);
        CHECK(missingVersion["data"]["winrt"]["warnings"].dump().find("winmd_version_marker_missing") != std::string::npos);
    }
    for (size_t n = 0; n < original.size(); n += 47) {
        (void)registry.Inspect(*FromBytes(std::vector<uint8_t>(original.begin(), original.begin() + n), "cut.winmd"));
    }
    std::mt19937 random(20261107);
    for (int i = 0; i < 400; ++i) {
        auto bytes = original;
        for (int j = 0; j < 1 + static_cast<int>(random() % 6); ++j)
            bytes[random() % bytes.size()] = static_cast<uint8_t>(random());
        (void)registry.Inspect(*FromBytes(std::move(bytes), "fuzz.winmd"));
    }
}

void Fuzz() {
    std::mt19937 random(9001);
    const auto dll = ReadBytes(g_dir / "Cortex.Fixture.dll");
    const auto zip = ReadBytes(g_dir / "zip-cases.zip");
    for (int round = 0; round < 1500; ++round) {
        auto bytes = round % 2 ? dll : zip;
        const int edits = 1 + static_cast<int>(random() % 8);
        for (int e = 0; e < edits; ++e) bytes[random() % bytes.size()] = static_cast<uint8_t>(random());
        const auto source = FromBytes(bytes, round % 2 ? "fuzz.dll" : "fuzz.zip");
        const json result = DefaultRegistry().Inspect(*source);
        if (round % 2) {
            for (const char* method : {"ApplyDamage", "Heal", "Classify", "Pick", ".ctor"})
                (void)DisassembleMethods(*source, method, 1000);
        } else {
            std::string error;
            for (const auto& child : DefaultRegistry().ListChildren(*source).value("children", json::array()))
                (void)DefaultRegistry().OpenChild(source, child["name"], error);
        }
        std::vector<uint8_t> out;
        std::string error;
        (void)Inflate(bytes.data() + 40, bytes.size() - 40, out, 1 << 20, error);
    }
}

} // namespace

int main(int argc, char** argv) {
    g_dir = argc > 1 ? fs::u8path(argv[1]) : fs::path("tests/fixtures/dotnet");
    std::ifstream in(g_dir / "Cortex.Fixture-expected.json");
    if (!in) {
        std::cerr << "fixtures not found under " << g_dir.u8string() << std::endl;
        return 2;
    }
    const json expected = json::parse(in);
    TestOpCodes(expected);
    TestMetadata(expected);
    TestWinMd();
    TestMethodBodies(expected);
    TestInflate();
    TestZip();
    Fuzz();
    if (failures) {
        std::cerr << failures << " .NET/ZIP check(s) failed" << std::endl;
        return 1;
    }
    std::cout << ".NET metadata, CIL and ZIP tests passed" << std::endl;
    return 0;
}
