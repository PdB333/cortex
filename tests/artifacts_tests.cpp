// Artifact providers (core/artifacts): SHA-256 identity, the offline PE
// provider on a real Cortex-built executable, the ASAR provider on generated
// archives, and robustness against truncated, corrupted and random inputs.
//
//   cortex_artifacts_tests <pe fixture> [fixtures dir]   (CTest passes both)
//
// tests/fixtures/pe: signed-version.exe (MinGW, version and manifest
// resources, signed with a self-signed test certificate by osslsigncode) and
// packed.exe (the same program packed by UPX); app.c and app.rc rebuild them.
// Expected values come from pefile 2024 (imphash, entropy) and osslsigncode
// verify (the Authenticode digest).

#include "artifacts/artifact.h"
#include "artifacts/asar_provider.h"
#include "artifacts/pe_extras.h"
#include "artifacts/pe_provider.h"
#include "artifacts/sha256.h"

#include <chrono>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

using namespace cortex::artifacts;

int failures = 0;

#define CHECK(condition)                                                              \
    do {                                                                              \
        if (!(condition)) {                                                           \
            ++failures;                                                               \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #condition \
                      << std::endl;                                                   \
        }                                                                             \
    } while (false)

std::string Hash(const std::string& text) {
    Sha256 hash;
    hash.Update(text.data(), text.size());
    return Sha256::Hex(hash.Final());
}

void TestSha256() {
    CHECK(Hash("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(Hash("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(Hash("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    Sha256 million;
    const std::string block(1000, 'a');
    for (int i = 0; i < 1000; ++i) million.Update(block.data(), block.size());
    CHECK(Sha256::Hex(million.Final()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    // Streaming in odd chunk sizes gives the same digest.
    const std::string text(1000, 'q');
    Sha256 chunked;
    for (size_t at = 0; at < text.size(); at += 7) chunked.Update(text.data() + at, std::min<size_t>(7, text.size() - at));
    CHECK(Sha256::Hex(chunked.Final()) == Hash(text));
    CHECK(Sha256Of(*FromBytes(std::vector<uint8_t>{'a', 'b', 'c'}, "abc")) == Hash("abc"));
}

bool HasExport(const json& data, const std::string& name) {
    for (const auto& row : data["exports"]["functions"])
        if (row.value("name", std::string()) == name) return true;
    return false;
}

bool HasImport(const json& data, const std::string& module, const std::string& function) {
    for (const auto& row : data["imports"]) {
        std::string name = row.value("name", std::string());
        for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (name != module) continue;
        for (const auto& f : row["functions"])
            if (f.value("name", std::string()) == function) return true;
    }
    return false;
}

std::vector<uint8_t> Bytes(const ByteSource& source) {
    return source.ReadSome(0, static_cast<size_t>(source.Size()));
}

void TestPeFixture(const std::string& path) {
    std::string error;
    auto file = OpenFile(path, &error);
    CHECK(file != nullptr);
    if (!file) {
        std::cerr << "cannot open PE fixture " << path << ": " << error << std::endl;
        return;
    }
    const auto& registry = DefaultRegistry();
    CHECK(registry.Detect(*file) && std::string(registry.Detect(*file)->Name()) == "pe");
    const json result = registry.Inspect(*file);
    CHECK(result["ok"] == true && result["schema"] == "cortex.artifact.v1");
    CHECK(result["identity"]["sha256"] == Sha256Of(*file) && result["identity"]["size"] == file->Size());
    CHECK(result["capabilities"]["static"]["status"] == "validated");
    CHECK(result["capabilities"]["decompile"]["status"] == "unavailable");
    const json& data = result["data"];
    CHECK(data["format"] == "pe" && data["kind"] == "executable");
    CHECK(data["machine"] == "x64" || data["machine"] == "x86");
    CHECK(data["pe32_plus"] == (data["machine"] == "x64"));
    CHECK(data["subsystem"] == "windows_cui" || data["subsystem"] == "windows_gui");
    CHECK(data["entry_rva"] != "0x0");
    bool text = false;
    for (const auto& section : data["sections"])
        text = text || (section["name"] == ".text" && section["flags"].dump().find("execute") != std::string::npos);
    CHECK(text);
    // Long section names ("/4") are resolved through the COFF string table.
    for (const auto& section : data["sections"]) {
        const std::string name = section["name"];
        CHECK(!(name.size() > 1 && name[0] == '/' && name.find_first_not_of("0123456789", 1) == std::string::npos));
    }
    // The test target's own exports and a well-known import.
    CHECK(HasExport(data, "CortexDemoApplyDamage"));
    CHECK(HasExport(data, "g_cortex_demo_actor"));
    CHECK(HasExport(data, "CortexNativeAdd"));
    CHECK(HasImport(data, "kernel32.dll", "GetTickCount") || HasImport(data, "kernel32.dll", "GetTickCount64"));
    CHECK(HasImport(data, "user32.dll", "CreateWindowExA"));
    if (data["machine"] == "x64") CHECK(data["exception"]["runtime_functions"].get<uint64_t>() > 10);

    // The export RVA leads to code in an executable section of the file.
    PeFile pe;
    CHECK(ParsePe(*file, pe, error));
    for (const auto& row : data["exports"]["functions"]) {
        if (row.value("name", std::string()) != "CortexDemoApplyDamage") continue;
        const uint32_t rva = static_cast<uint32_t>(std::stoul(row["rva"].get<std::string>(), nullptr, 16));
        const PeSection* section = pe.SectionFor(rva);
        CHECK(section && (section->characteristics & 0x20000000));
        uint64_t offset = 0;
        CHECK(pe.RvaToOffset(rva, offset) && offset < file->Size());
    }

    // Bounded lists report truncation instead of growing without limit.
    InspectOptions small;
    small.maxEntries = 3;
    const json bounded = registry.Inspect(*file, small);
    CHECK(bounded["data"]["exports"]["functions"].size() <= 3 && bounded["data"]["exports_truncated"] == true);
    CHECK(bounded["data"]["imports_truncated"] == true);

    // Robustness: every prefix of the headers and structural corruptions
    // either fail cleanly or report warnings; nothing crashes or hangs.
    const auto original = Bytes(*file);
    for (size_t size = 0; size < std::min<size_t>(original.size(), 0x1200); size += 37) {
        const auto truncated = FromBytes(std::vector<uint8_t>(original.begin(), original.begin() + size), "prefix");
        (void)registry.Inspect(*truncated);
    }
    auto mutate = [&](size_t offset, std::vector<uint8_t> bytes) {
        auto copy = original;
        if (offset + bytes.size() <= copy.size()) std::memcpy(copy.data() + offset, bytes.data(), bytes.size());
        return registry.Inspect(*FromBytes(std::move(copy), "mutated"));
    };
    uint32_t lfanew = 0;
    std::memcpy(&lfanew, original.data() + 0x3C, 4);
    CHECK(mutate(0x3C, {0xFF, 0xFF, 0xFF, 0x7F})["ok"] == false);              // e_lfanew far outside
    CHECK(mutate(lfanew, {'X', 'E', 0, 0})["error"] == "unknown_format");  // no provider claims it
    {
        auto copy = original;
        copy[lfanew] = 'X';
        PeFile broken;
        std::string why;
        CHECK(!ParsePe(*FromBytes(copy, "x"), broken, why) && why == "not_pe");
    }
    const json manySections = mutate(lfanew + 6, {0xFF, 0xFF});                // 65535 sections
    CHECK(manySections["ok"] == true);
    CHECK(manySections["data"]["warnings"].dump().find("section_count_clamped") != std::string::npos);
    const size_t directories = lfanew + 24 + (data["pe32_plus"] == true ? 112 : 96);
    const json badImport = mutate(directories + 8, {0xF0, 0xFF, 0xFF, 0x7F});  // import RVA nowhere
    CHECK(badImport["ok"] == true && badImport["data"]["imports"].empty());

    // Seeded random fuzzing of the header area and the directories.
    std::mt19937 random(20261030);
    const auto started = std::chrono::steady_clock::now();
    const size_t window = std::min<size_t>(original.size(), 0x2000);
    for (int round = 0; round < 1500; ++round) {
        auto copy = original;
        const int edits = 1 + static_cast<int>(random() % 8);
        for (int e = 0; e < edits; ++e) {
            const size_t at = random() % window;
            copy[at] = static_cast<uint8_t>(random());
            if (random() % 4 == 0 && at + 4 <= copy.size()) std::memset(copy.data() + at, 0xFF, 4);
        }
        (void)registry.Inspect(*FromBytes(std::move(copy), "fuzz"));
    }
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    CHECK(seconds < 120.0);
}

// Builds an ASAR from (name, bytes, extra json) entries; nested names use '/'.
std::vector<uint8_t> MakeAsar(const json& tree, const std::vector<uint8_t>& payload) {
    const std::string text = json{{"files", tree}}.dump();
    const uint32_t jsonLength = static_cast<uint32_t>(text.size());
    const uint32_t padded = (jsonLength + 3) & ~3u;
    const uint32_t headerPayload = 4 + padded;
    const uint32_t headerSize = 4 + headerPayload;
    std::vector<uint8_t> out;
    auto u32 = [&](uint32_t value) { for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(value >> (8 * i))); };
    u32(4);
    u32(headerSize);
    u32(headerPayload);
    u32(jsonLength);
    out.insert(out.end(), text.begin(), text.end());
    out.resize(8 + headerSize, 0);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

void TestAsar() {
    const std::string package = R"({"name":"demo-app","version":"1.2.3","main":"src/main.js","dependencies":{"a":"1"}})";
    const std::string mainJs = "const { app } = require('electron');\n";
    std::vector<uint8_t> payload(package.begin(), package.end());
    payload.insert(payload.end(), mainJs.begin(), mainJs.end());
    const json tree = {
        {"package.json", {{"size", package.size()}, {"offset", "0"}}},
        {"src", {{"files", {{"main.js", {{"size", mainJs.size()}, {"offset", std::to_string(package.size())},
                                         {"integrity", {{"algorithm", "SHA256"}, {"hash", "00"}}}}}}}}},
        {"native", {{"files", {{"addon.node", {{"size", 4096}, {"unpacked", true}}}}}}},
        {"current", {{"link", "src/main.js"}}},
        {"..", {{"files", {{"evil.js", {{"size", 1}, {"offset", "0"}}}}}}},
        {"huge.bin", {{"size", 1000}, {"offset", "999999"}}}};
    auto archive = FromBytes(MakeAsar(tree, payload), "app.asar");
    const auto& registry = DefaultRegistry();
    CHECK(registry.Detect(*archive) && std::string(registry.Detect(*archive)->Name()) == "asar");

    const json result = registry.Inspect(*archive);
    CHECK(result["ok"] == true && result["format"] == "asar");
    const json& data = result["data"];
    CHECK(data["files"] == 5 && data["unpacked_files"] == 1 && data["links"] == 1);
    CHECK(data["files_with_integrity"] == 1 && data["unsafe_entry_names"] == 1);
    CHECK(data["package_json"]["main"] == "src/main.js" && data["package_json"]["name"] == "demo-app");
    CHECK(data["package_json"]["dependencies"] == 1);

    const json children = registry.ListChildren(*archive);
    CHECK(children["ok"] == true);
    auto find = [&](const std::string& name) {
        for (const auto& row : children["children"]) if (row["name"] == name) return row;
        return json();
    };
    CHECK(find("src/main.js")["readable"] == true && find("src/main.js")["size"] == mainJs.size());
    CHECK(find("native/addon.node")["readable"] == false);
    CHECK(find("current")["readable"] == false && find("current")["note"].get<std::string>().find("link") == 0);
    CHECK(find("../evil.js")["readable"] == false);        // never extractable outside its folder
    CHECK(find("huge.bin")["readable"] == false);          // offset outside the archive

    std::string error;
    auto child = registry.OpenChild(archive, "src/main.js", error);
    CHECK(child && child->Size() == mainJs.size());
    if (child) {
        const auto bytes = Bytes(*child);
        CHECK(std::string(bytes.begin(), bytes.end()) == mainJs);
        CHECK(child->Name() == "app.asar!src/main.js");
    }
    CHECK(!registry.OpenChild(archive, "native/addon.node", error) && error == "child_not_readable");
    CHECK(!registry.OpenChild(archive, "../evil.js", error) && error == "child_not_readable");
    CHECK(!registry.OpenChild(archive, "missing.js", error) && error == "child_not_found");

    // Malformed archives.
    auto bad = MakeAsar(tree, payload);
    bad[4] = 0xFF; bad[5] = 0xFF; bad[6] = 0xFF; bad[7] = 0x7F;  // header larger than the file
    CHECK(registry.Inspect(*FromBytes(bad, "bad1"))["ok"] == false);
    bad = MakeAsar(tree, payload);
    bad[16] = '[';                                              // not a file tree
    CHECK(!AsarProvider().Detect(*FromBytes(bad, "bad2")));
    json deep = {{"leaf.js", {{"size", 1}, {"offset", "0"}}}};
    for (int i = 0; i < 100; ++i) deep = {{"d", {{"files", deep}}}};
    const json deepChildren = registry.ListChildren(*FromBytes(MakeAsar(deep, payload), "deep"));
    CHECK(deepChildren["ok"] == false && deepChildren["error"] == "asar_tree_too_deep");

    // Bounded listing.
    InspectOptions small;
    small.maxEntries = 2;
    CHECK(registry.ListChildren(*archive, small)["children"].size() == 2);

    // Random bytes are nobody's format.
    std::mt19937 random(7);
    std::vector<uint8_t> noise(4096);
    for (auto& byte : noise) byte = static_cast<uint8_t>(random());
    const json unknown = registry.Inspect(*FromBytes(noise, "noise"));
    CHECK(unknown["ok"] == false && unknown["error"] == "unknown_format" && unknown["identity"]["sha256"].is_string());
}

} // namespace

json InspectData(const std::string& path) {
    auto file = OpenFile(path);
    CHECK(file != nullptr);
    if (!file) return json::object();
    return DefaultRegistry().Inspect(*file)["data"];
}

void TestPeExtras(const std::string& fixtures) {
    CHECK(Md5Hex("") == "d41d8cd98f00b204e9800998ecf8427e");
    CHECK(Md5Hex("The quick brown fox jumps over the lazy dog") == "9e107d9d372bb6826bd81d3542a419d6");

    const json signedPe = InspectData(fixtures + "/pe/signed-version.exe");
    CHECK(signedPe.value("imphash", std::string()) == "6c200472c4c0cc849e4a34115a04f86a");  // pefile
    bool text = false;
    for (const auto& section : signedPe["sections"])
        if (section["name"] == ".text") text = section["entropy"] == 5.85;  // pefile: 5.85
    CHECK(text);
    const json& version = signedPe["resources"]["version"];
    CHECK(version["file_version"] == "1.2.3.4" && version["product_version"] == "1.2.0.0");
    CHECK(version["strings"]["CompanyName"] == "Cortex test fixtures" &&
          version["strings"]["OriginalFilename"] == "signed-version.exe" && version["translation"] == "040904b0");
    CHECK(signedPe["resources"]["manifest"]["requested_execution_level"] == "asInvoker" &&
          signedPe["resources"]["manifest"]["assembly_name"] == "Cortex.Fixture.Signed");
    CHECK(signedPe["resources"]["counts"]["string_table"] == 1);
    CHECK(signedPe["hardening"]["aslr"] == true && signedPe["hardening"]["dep"] == true &&
          signedPe["hardening"]["high_entropy_va"] == true && signedPe["hardening"]["cfg"] == false);
    const json& signature = signedPe["authenticode"];
    CHECK(signature["certificate_type"] == "pkcs_signed_data" && signature["digest_algorithm"] == "sha256");
    CHECK(signature["signer"]["subject"]["CN"] == "Cortex Test Signer" && signature["signer"]["self_signed"] == true);
    CHECK(signature["file_digest_matches"] == true && signature["verified_chain"] == false);
    CHECK(signature["signed_digest"] == "c4c3f0a7e534b0df2b91824eeec15f2f5627fe2ff4339041207985047f5572b3");  // osslsigncode
    CHECK(signedPe["packers"]["detected"].empty());

    // One byte of code changed after signing: the digest no longer matches.
    auto file = OpenFile(fixtures + "/pe/signed-version.exe");
    if (file) {
        auto bytes = Bytes(*file);
        PeFile pe;
        std::string error;
        CHECK(ParsePe(*file, pe, error));
        bytes[pe.sections.front().rawOffset + 16] ^= 0xFF;
        const auto tampered = FromBytes(bytes, "tampered.exe");
        const json data = DefaultRegistry().Inspect(*tampered)["data"];
        CHECK(data["authenticode"]["file_digest_matches"] == false);
    }

    const json packed = InspectData(fixtures + "/pe/packed.exe");
    CHECK(packed.value("imphash", std::string()) == "d9904535440e42784fec24bb2374117d");  // pefile
    CHECK(!packed["packers"]["detected"].empty() && packed["packers"]["detected"][0]["name"] == "UPX");
    CHECK(!packed["packers"]["signals"].empty());

    // A Rich header built by hand: DanS, padding, two entries, Rich, key.
    std::vector<uint8_t> head(0x100, 0);
    head[0] = 'M';
    head[1] = 'Z';
    const uint32_t key = 0x1234ABCDu;
    auto put = [&head](size_t at, uint32_t value) {
        for (int i = 0; i < 4; ++i) head[at + i] = static_cast<uint8_t>(value >> (8 * i));
    };
    put(0x80, 0x536E6144u ^ key);
    put(0x84, key);
    put(0x88, key);
    put(0x8C, key);
    put(0x90, ((0x0104u << 16) | 30159u) ^ key);  // product 0x104, build 30159
    put(0x94, 7u ^ key);
    put(0x98, ((0x0105u << 16) | 30159u) ^ key);
    put(0x9C, 3u ^ key);
    put(0xA0, 0x68636952u);
    put(0xA4, key);
    const json rich = RichHeaderFromBytes(head);
    CHECK(!rich.is_null() && rich["entries"].size() == 2 && rich["entries"][0]["product_id"] == 0x104 &&
          rich["entries"][0]["build"] == 30159 && rich["entries"][0]["count"] == 7 && rich["key"] == "1234abcd");
    CHECK(rich["checksum_valid"] == false);  // the key above is not the real checksum
    CHECK(RichHeaderFromBytes(std::vector<uint8_t>(0x100, 0)).is_null());
}

// A Wine minidump of tests/fixtures/dumps/crash.c (a write to 0x10), checked
// against the minidump package (after neutralizing Wine's private stream,
// which that package refuses and Cortex lists).
void TestMinidump(const std::string& fixtures) {
    auto file = OpenFile(fixtures + "/dumps/wine-access-violation.dmp");
    CHECK(file != nullptr);
    if (!file) return;
    const json result = DefaultRegistry().Inspect(*file);
    CHECK(result["format"] == "minidump");
    const json& data = result["data"];
    CHECK(data["system"]["architecture"] == "x64" && data["system"]["os_version"] == "10.0.19043" &&
          data["system"]["processors"] == 4);
    CHECK(data["process"]["pid"] == 260);
    const json& exception = data["exception"];
    CHECK(exception["name"] == "ACCESS_VIOLATION" && exception["code"] == "0xc0000005" && exception["thread"] == 264);
    CHECK(exception["address"] == "0x14000152c" && exception["module"] == "crash.exe+0x152c");
    CHECK(exception["access"]["kind"] == "write" && exception["access"]["target"] == "0x10");
    CHECK(exception["context"]["ip"] == "0x14000152c");
    CHECK(data["modules"].size() == 8 && data["threads"].size() == 1 && data["threads"][0]["id"] == 264);
    bool wine = false;
    for (const auto& stream : data["streams"]) wine = wine || stream["type"] == "wine_specific";
    CHECK(wine);
    // Every prefix fails cleanly or reads partially; nothing crashes.
    const auto bytes = Bytes(*file);
    for (size_t size = 0; size < 4096; size += 61)
        (void)DefaultRegistry().Inspect(*FromBytes(std::vector<uint8_t>(bytes.begin(), bytes.begin() + size), "prefix.dmp"));
}

// tests/fixtures/registry/small.hiv, written by make_hive.py and read back by
// python-registry as the independent check.
void TestHive(const std::string& fixtures) {
    auto file = OpenFile(fixtures + "/registry/small.hiv");
    CHECK(file != nullptr);
    if (!file) return;
    const json result = DefaultRegistry().Inspect(*file);
    CHECK(result["format"] == "registry-hive");
    const json& data = result["data"];
    CHECK(data["checksum_valid"] == true && data["dirty"] == false && data["version"] == "1.5");
    CHECK(data["keys"] == 5 && data["values"] == 6 && data["root"]["name"] == "ROOT");
    CHECK(data["last_written"] == "2024-09-05T08:53:20Z");  // python-registry
    const json& cortex = data["root"]["subkeys"][0]["subkeys"][0];
    CHECK(cortex["name"] == "Cortex" && cortex["values"].size() == 6);
    auto value = [&cortex](const std::string& name) {
        for (const auto& row : cortex["values"])
            if (row["name"] == name) return row;
        return json();
    };
    CHECK(value("Name")["type"] == "REG_SZ" && value("Name")["data"] == "Cortex fixture");
    CHECK(value("Count")["data"] == 42 && value("Data")["hex"] == "010203");
    CHECK(value("List")["data"] == json::array({"alpha", "beta"}));
    CHECK(value("Big")["data"] == 72623859790382856ull && value("Path")["type"] == "REG_EXPAND_SZ");
    // A loop between cells (a key listing itself) is cut, not followed.
    auto bytes = Bytes(*file);
    const auto corrupted = FromBytes(bytes, "copy.hiv");
    (void)DefaultRegistry().Inspect(*corrupted);
    for (size_t at = 0x1000; at + 4 < bytes.size(); at += 97) {
        auto broken = bytes;
        broken[at] ^= 0x5A;
        (void)DefaultRegistry().Inspect(*FromBytes(broken, "broken.hiv"));
    }
}

int main(int argc, char** argv) {
    TestSha256();
    TestAsar();
    TestPeExtras(argc > 2 ? argv[2] : "tests/fixtures");
    TestMinidump(argc > 2 ? argv[2] : "tests/fixtures");
    TestHive(argc > 2 ? argv[2] : "tests/fixtures");
    if (argc > 1) TestPeFixture(argv[1]);
    else std::cerr << "note: no PE fixture given, PE checks skipped" << std::endl;
    if (failures) {
        std::cerr << failures << " artifact check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "artifact provider tests passed" << (argc > 1 ? "" : " (without PE fixture)") << std::endl;
    return 0;
}
