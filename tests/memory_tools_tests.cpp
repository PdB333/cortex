// Unit tests for the memory tools: PE header parsing, string and code cave
// search, AOB signatures and the pointer scanner, against an in-memory fake
// process.

#include "services/memory_tools.h"
#include "services/pointer_scanner.h"
#include "services/signature.h"
#include "services/instruction_operands.h"
#include "fake_process.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace cortex::services;
using cortex::tests::FakeProcess;
using cortex::target::MemoryRegionType;

int failures = 0;

#define CHECK(condition)                                                              \
    do {                                                                              \
        if (!(condition)) {                                                           \
            ++failures;                                                               \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #condition \
                      << std::endl;                                                   \
        }                                                                             \
    } while (false)

template <typename T>
void Put(std::vector<uint8_t>& image, size_t offset, T value) {
    std::memcpy(image.data() + offset, &value, sizeof(T));
}

void PutText(std::vector<uint8_t>& image, size_t offset, const char* text) {
    std::memcpy(image.data() + offset, text, std::strlen(text) + 1);
}

MemoryReader ReaderFor(const std::shared_ptr<FakeProcess>& process) {
    return [process](uint64_t address, void* buffer, size_t size) {
        return process->ReadMemory(address, buffer, size, nullptr);
    };
}

void TestPeImage() {
    constexpr uint64_t base = 0x140000000ull;
    std::vector<uint8_t> image(0x4000, 0);
    Put<uint16_t>(image, 0, 0x5A4D);
    Put<uint32_t>(image, 0x3C, 0x80);
    Put<uint32_t>(image, 0x80, 0x00004550);
    Put<uint16_t>(image, 0x84, 0x8664);
    Put<uint16_t>(image, 0x86, 2);
    Put<uint32_t>(image, 0x88, 0x12345678);
    Put<uint16_t>(image, 0x94, 0xF0);
    Put<uint16_t>(image, 0x96, 0x22);
    const size_t optional = 0x98;
    Put<uint16_t>(image, optional, 0x20B);
    Put<uint32_t>(image, optional + 16, 0x1010);
    Put<uint64_t>(image, optional + 24, base);
    Put<uint32_t>(image, optional + 32, 0x1000);
    Put<uint32_t>(image, optional + 36, 0x200);
    Put<uint32_t>(image, optional + 56, 0x4000);
    Put<uint32_t>(image, optional + 60, 0x400);
    Put<uint16_t>(image, optional + 68, 3);
    Put<uint32_t>(image, optional + 108, 16);
    Put<uint32_t>(image, optional + 112, 0x2000);  // export directory
    Put<uint32_t>(image, optional + 116, 0x100);
    Put<uint32_t>(image, optional + 120, 0x3000);  // import directory
    Put<uint32_t>(image, optional + 124, 0x40);
    const size_t sections = optional + 0xF0;
    std::memcpy(image.data() + sections, ".text", 5);
    Put<uint32_t>(image, sections + 8, 0x1000);
    Put<uint32_t>(image, sections + 12, 0x1000);
    Put<uint32_t>(image, sections + 36, 0x60000020);
    std::memcpy(image.data() + sections + 40, ".data", 5);
    Put<uint32_t>(image, sections + 48, 0x2000);
    Put<uint32_t>(image, sections + 52, 0x2000);
    Put<uint32_t>(image, sections + 76, 0xC0000040);

    Put<uint32_t>(image, 0x2000 + 12, 0x2080);  // name
    Put<uint32_t>(image, 0x2000 + 16, 1);       // ordinal base
    Put<uint32_t>(image, 0x2000 + 20, 2);       // functions
    Put<uint32_t>(image, 0x2000 + 24, 1);       // names
    Put<uint32_t>(image, 0x2000 + 28, 0x2040);
    Put<uint32_t>(image, 0x2000 + 32, 0x2050);
    Put<uint32_t>(image, 0x2000 + 36, 0x2060);
    Put<uint32_t>(image, 0x2040, 0x1010);
    Put<uint32_t>(image, 0x2044, 0x2090);  // inside the directory: forwarded
    Put<uint32_t>(image, 0x2050, 0x20A0);
    Put<uint16_t>(image, 0x2060, 0);
    PutText(image, 0x2080, "test.dll");
    PutText(image, 0x2090, "OTHER.Func");
    PutText(image, 0x20A0, "Alpha");

    Put<uint32_t>(image, 0x3000, 0x3100);
    Put<uint32_t>(image, 0x3000 + 12, 0x3200);
    Put<uint32_t>(image, 0x3000 + 16, 0x3180);
    Put<uint64_t>(image, 0x3100, 0x3300);
    Put<uint64_t>(image, 0x3108, 0x8000000000000010ull);
    Put<uint64_t>(image, 0x3180, 0x7FFA00001000ull);
    Put<uint64_t>(image, 0x3188, 0x7FFA00002000ull);
    PutText(image, 0x3200, "KERNEL32.dll");
    PutText(image, 0x3302, "Sleep");

    auto process = std::make_shared<FakeProcess>();
    process->Add(base, image.size(), false, false, MemoryRegionType::Image);
    process->Write(base, image.data(), image.size());

    PeImage pe;
    std::string error;
    CHECK(ParsePeImage(ReaderFor(process), base, pe, &error));
    CHECK(pe.pe32Plus && pe.machine == 0x8664 && pe.timeDateStamp == 0x12345678);
    CHECK(pe.entryPoint == 0x1010 && pe.sizeOfImage == 0x4000 && pe.subsystem == 3);
    CHECK(pe.sections.size() == 2 && pe.sections[0].name == ".text" && pe.sections[0].Executable());
    CHECK(pe.sections.size() == 2 && pe.sections[1].Writable() && !pe.sections[1].Executable());
    CHECK(pe.directories.size() == 16 && pe.directories[1].virtualAddress == 0x3000);
    CHECK(pe.exportName == "test.dll");
    CHECK(pe.exports.size() == 2);
    if (pe.exports.size() == 2) {
        CHECK(pe.exports[0].name == "Alpha" && pe.exports[0].ordinal == 1 && pe.exports[0].rva == 0x1010);
        CHECK(pe.exports[1].name.empty() && pe.exports[1].forwarder == "OTHER.Func");
    }
    CHECK(pe.imports.size() == 2);
    if (pe.imports.size() == 2) {
        CHECK(pe.imports[0].module == "KERNEL32.dll" && pe.imports[0].name == "Sleep");
        CHECK(pe.imports[0].slot == base + 0x3180 && pe.imports[0].value == 0x7FFA00001000ull);
        CHECK(pe.imports[1].name.empty() && pe.imports[1].ordinal == 16);
    }
    CHECK(PeSectionFlags(0x60000020) == "R-X code");
    CHECK(std::string(PeMachineName(0x14C)) == "x86 (I386)");

    std::vector<uint8_t> junk(0x1000, 0x41);
    process->Add(0x50000, junk.size());
    process->Write(0x50000, junk.data(), junk.size());
    CHECK(!ParsePeImage(ReaderFor(process), 0x50000, pe, &error) && !error.empty());
}

void TestStringsAndCaves() {
    std::vector<uint8_t> data(256, 0x01);
    const char ascii[] = "Hello world";
    std::memcpy(data.data() + 10, ascii, sizeof(ascii) - 1);
    const char wide[] = {'W', 0, 'i', 0, 'd', 0, 'e', 0, '!', 0};
    std::memcpy(data.data() + 41, wide, sizeof(wide));
    const char tail[] = "end";
    std::memcpy(data.data() + 253, tail, 3);
    std::vector<FoundString> strings;
    FindStrings(data.data(), data.size(), 0x1000, 3, true, true, 100, strings);
    CHECK(strings.size() == 3);
    if (strings.size() == 3) {
        CHECK(strings[0].address == 0x100A && strings[0].text == "Hello world" && !strings[0].utf16);
        CHECK(strings[1].address == 0x1029 && strings[1].text == "Wide!" && strings[1].utf16);
        CHECK(strings[2].text == "end");
    }
    strings.clear();
    FindStrings(data.data(), data.size(), 0x1000, 3, true, false, 1, strings);
    CHECK(strings.size() == 1);

    std::vector<uint8_t> code(200, 0x90);
    std::memset(code.data() + 20, 0xCC, 20);
    std::memset(code.data() + 100, 0x00, 40);
    std::memset(code.data() + 160, 0xCC, 5);
    std::vector<CodeCave> caves;
    FindCodeCaves(code.data(), code.size(), 0x400000, 16, 100, caves);
    CHECK(caves.size() == 2);
    if (caves.size() == 2) {
        CHECK(caves[0].address == 0x400014 && caves[0].size == 20 && caves[0].filler == 0xCC);
        CHECK(caves[1].address == 0x400064 && caves[1].size == 40 && caves[1].filler == 0x00);
    }

    uint64_t first = 0;
    CHECK(CountPatternMatches(code.data(), code.size(), {0x90, 0xCC}, {0xFF, 0xFF}, 10, &first) == 2 && first == 19);
    CHECK(CountPatternMatches(code.data(), code.size(), {0xCC, 0x00, 0x90}, {0xFF, 0x00, 0xFF}, 10) == 4);
}

void TestSignature() {
    const uint8_t function[] = {
        0x48, 0x8B, 0x05, 0x11, 0x22, 0x33, 0x44,  // mov rax, [rip+0x44332211]
        0x48, 0x85, 0xC0,                          // test rax, rax
        0x74, 0x05,                                // je +5
        0x8B, 0x48, 0x10,                          // mov ecx, [rax+0x10]
        0xC3};
    std::vector<uint8_t> haystack(64, 0xCC);
    haystack.insert(haystack.end(), std::begin(function), std::end(function));
    haystack.insert(haystack.end(), 32, 0xCC);
    std::vector<uint8_t> other(std::begin(function), std::end(function));
    other[3] = 0x99;   // another RIP displacement
    other[14] = 0x18;  // another field
    haystack.insert(haystack.end(), other.begin(), other.end());

    Signature signature;
    std::string error;
    CHECK(GenerateSignature(function, sizeof(function), haystack.data(), haystack.size(), SignatureOptions{},
                            signature, &error));
    CHECK(signature.Text() == "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 ?? 8B 48 10");
    CHECK(signature.matches == 1 && signature.instructions == 4);

    const uint8_t bad[] = {0x0F, 0x0B + 0xF0};
    Signature invalid;
    CHECK(!GenerateSignature(bad, 1, haystack.data(), haystack.size(), SignatureOptions{}, invalid, &error) ||
          invalid.bytes.size() > 0);
}

void TestMemoryAccess() {
    auto registers = [](const std::string& name, uint64_t& value) {
        if (name == "RAX") value = 0x10000000;
        else if (name == "RCX") value = 3;
        else if (name == "EBX") value = 0x2000;
        else return false;
        return true;
    };
    MemoryAccess access;
    std::string error;
    const uint8_t indexed[] = {0x89, 0x44, 0x88, 0x10};  // mov [rax+rcx*4+0x10], eax
    CHECK(ResolveMemoryAccess(indexed, sizeof(indexed), 0x401000, true, registers, access, &error));
    CHECK(access.address == 0x1000001C && access.size == 4 && access.write);
    const uint8_t rip[] = {0x48, 0x8B, 0x05, 0x10, 0x00, 0x00, 0x00};  // mov rax, [rip+0x10]
    CHECK(ResolveMemoryAccess(rip, sizeof(rip), 0x401000, true, registers, access, &error));
    CHECK(access.address == 0x401017 && access.size == 8 && !access.write);
    const uint8_t x86[] = {0x8B, 0x43, 0x08};  // mov eax, [ebx+8]
    CHECK(ResolveMemoryAccess(x86, sizeof(x86), 0x401000, false, registers, access, &error));
    CHECK(access.address == 0x2008 && access.size == 4);
    const uint8_t noMemory[] = {0x48, 0x85, 0xC0};  // test rax, rax
    CHECK(!ResolveMemoryAccess(noMemory, sizeof(noMemory), 0x401000, true, registers, access, &error));
    const uint8_t unknown[] = {0x8B, 0x02};  // mov eax, [rdx]
    CHECK(!ResolveMemoryAccess(unknown, sizeof(unknown), 0x401000, true, registers, access, &error));
}

void TestPointerScanner() {
    auto process = std::make_shared<FakeProcess>();
    process->Add(0x400000, 0x2000, true, false, MemoryRegionType::Image);
    process->Add(0x10000000, 0x10000, true, false, MemoryRegionType::Private);
    process->Put<uint64_t>(0x400100, 0x10000000);    // game.exe+100 -> object
    process->Put<uint64_t>(0x10000020, 0x10001000);  // object+20 -> player
    process->Put<uint64_t>(0x400200, 0x10001040);    // game.exe+200 -> near the target
    const uint64_t target = 0x10001048;

    PointerScanOptions options;
    options.target = target;
    options.maxLevel = 3;
    options.maxOffset = 0x100;
    options.modules = {{"game.exe", 0x400000, 0x2000}};
    PointerScanResult result;
    std::string error;
    CHECK(PointerScanner::Scan(process, options, result, &error));
    CHECK(result.paths.size() == 2);
    std::vector<std::string> formatted;
    for (const auto& path : result.paths) formatted.push_back(PointerScanner::Format(result, path));
    CHECK(formatted.size() == 2 && formatted[0] == "game.exe+200 -> 8");
    CHECK(formatted.size() == 2 && formatted[1] == "game.exe+100 -> 20 -> 48");

    uint64_t resolved = 0;
    CHECK(result.paths.size() == 2 && PointerScanner::Resolve(process, result, result.paths[1], options.modules, resolved) &&
          resolved == target);

    // The player object moves: only the two-level chain follows it.
    process->Put<uint64_t>(0x10000020, 0x10002000);
    const std::string file = (std::filesystem::temp_directory_path() / "cortex-pointer-test.json").u8string();
    CHECK(PointerScanner::Save(file, result, &error));
    PointerScanResult loaded;
    CHECK(PointerScanner::Load(file, loaded, &error) && loaded.paths.size() == 2);
    std::filesystem::remove(std::filesystem::u8path(file));
    CHECK(PointerScanner::Rescan(process, loaded, 0x10002048, {{"game.exe", 0x400000, 0x2000}}) == 1);
    CHECK(loaded.paths.size() == 1 && PointerScanner::Format(loaded, loaded.paths[0]) == "game.exe+100 -> 20 -> 48");

    options.maxLevel = 1;
    CHECK(PointerScanner::Scan(process, options, result, &error) && result.paths.size() == 1);
    options.maxResults = 1;
    options.maxLevel = 3;
    options.target = 0x10002048;
    CHECK(PointerScanner::Scan(process, options, result, &error) && result.paths.size() == 1);
    options.modules.clear();
    CHECK(!PointerScanner::Scan(process, options, result, &error));
}

} // namespace


void TestGroupedScan() {
    using cortex::services::FindGroupedValues;
    using cortex::services::GroupedElement;
    using cortex::services::GroupedHit;
    using cortex::services::ParseGroupedScan;

    std::vector<GroupedElement> elements;
    std::string error;
    CHECK(ParseGroupedScan("4:64 f:1.5 2:14", 4, elements, &error));
    CHECK(elements.size() == 3);
    CHECK(elements[0].size == 4 && elements[0].bytes[0] == 0x64 && elements[0].bytes[1] == 0);
    CHECK(elements[1].size == 4);
    CHECK(elements[2].size == 2 && elements[2].bytes[0] == 0x14);
    // Decimal with '#', and a default size when no prefix is given.
    CHECK(ParseGroupedScan("#100 *", 2, elements, &error));
    CHECK(elements.size() == 2 && elements[0].size == 2 && elements[0].bytes[0] == 100);
    CHECK(elements[1].wildcard);
    // A wildcard can carry a width: a structure field whose value is unknown.
    CHECK(ParseGroupedScan("4:64 4:* 2:14", 4, elements, &error));
    CHECK(elements.size() == 3 && elements[1].wildcard && elements[1].size == 4);
    CHECK(!ParseGroupedScan("", 4, elements, &error));
    CHECK(!ParseGroupedScan("*", 4, elements, &error) && error.find("at least one value") != std::string::npos);
    CHECK(!ParseGroupedScan("q:5", 4, elements, &error) && error.find("Unknown element type") != std::string::npos);
    CHECK(!ParseGroupedScan("4:zz", 4, elements, &error));

    // A buffer holding a little structure: 100, a float 1.5, then 20.
    std::vector<uint8_t> data(512, 0);
    const float pi = 1.5f;
    const uint32_t hundred = 100;
    const uint16_t twenty = 20;
    std::memcpy(data.data() + 64, &hundred, 4);
    std::memcpy(data.data() + 72, &pi, 4);
    std::memcpy(data.data() + 80, &twenty, 2);
    // A decoy: the same first value with nothing after it.
    std::memcpy(data.data() + 300, &hundred, 4);

    CHECK(ParseGroupedScan("4:64 f:1.5 2:14", 4, elements, &error));
    std::vector<GroupedHit> hits;
    FindGroupedValues(data.data(), data.size(), 0x140000000ull, elements, 64, true, 16, hits);
    CHECK(hits.size() == 1);
    if (hits.size() == 1) {
        CHECK(hits[0].address == 0x140000000ull + 64);
        CHECK(hits[0].offsets.size() == 3 && hits[0].offsets[1] == 8 && hits[0].offsets[2] == 16);
    }

    // A sized wildcard stands for a field: the value after it is looked for
    // past that field, and the wildcard reports where it sits.
    CHECK(ParseGroupedScan("4:64 4:* f:1.5", 4, elements, &error));
    FindGroupedValues(data.data(), data.size(), 0x140000000ull, elements, 64, true, 16, hits);
    CHECK(hits.size() == 1);
    if (hits.size() == 1) {
        CHECK(hits[0].address == 0x140000000ull + 64);
        CHECK(hits[0].offsets.size() == 3 && hits[0].offsets[1] == 4 && hits[0].offsets[2] == 8);
    }

    // A window that stops before the last value finds nothing.
    CHECK(ParseGroupedScan("4:64 f:1.5 2:14", 4, elements, &error));
    FindGroupedValues(data.data(), data.size(), 0, elements, 12, true, 16, hits);
    CHECK(hits.empty());

    // Out of order: ordered refuses, unordered accepts.
    CHECK(ParseGroupedScan("4:64 2:14 f:1.5", 4, elements, &error));
    FindGroupedValues(data.data(), data.size(), 0, elements, 64, true, 16, hits);
    CHECK(hits.empty());
    FindGroupedValues(data.data(), data.size(), 0, elements, 64, false, 16, hits);
    CHECK(hits.size() == 1);
}

int main() {
    TestGroupedScan();
    TestPeImage();
    TestStringsAndCaves();
    TestSignature();
    TestMemoryAccess();
    TestPointerScanner();
    if (failures) {
        std::cerr << failures << " check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "memory tools tests passed" << std::endl;
    return 0;
}
