// Cortex baseline benchmarks (roadmap step 1.3).
//
// Measures the cost of Cortex's own algorithms on a deterministic, in-memory
// fixture so results are comparable between commits and machines:
//
//   * value scanner  - first/next scans (exact, unknown initial value, AOB)
//   * pointer scanner - multi-level path search over a noisy heap
//   * memory reads   - Session::ReadMemory in page and 64 KiB chunks
//   * disassembly    - Zydis decode + Intel formatting, x86 and x64
//
// What this does NOT measure: ReadProcessMemory/VirtualQueryEx cost, the
// named-pipe MCP round trip, UI frame time or a real target's memory layout.
// Those need a Windows host and are listed as open baselines in
// docs/cortex-2-roadmap.md. The fixture's ReadMemory is a memcpy, so scanner
// numbers are an upper bound on what the algorithms allow, not end-to-end
// figures against a live process.
//
// Usage: cortex_baseline_bench [--quick] [--mb N] [--repeat N] [--json FILE]
// Exit code is non-zero if any benchmark fails its correctness check, so the
// --quick mode doubles as a CTest smoke test.

#include "services/pointer_scanner.h"
#include "services/value_scanner.h"
#include "target/session.h"

#include <Zydis/Zydis.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

using json = nlohmann::json;
using namespace cortex::services;
using cortex::target::MemoryRegion;
using cortex::target::MemoryRegionType;

// Read-only during a measurement, so unlike tests/fake_process.h it takes no
// lock: the scanner's worker threads are measured, not a mutex.
class BenchProcess final : public cortex::target::Session {
public:
    BenchProcess() {
        target_.id = "bench";
        target_.name = "bench.exe";
        target_.capabilities.Add(cortex::target::Capability::MemoryRead)
            .Add(cortex::target::Capability::MemoryScan);
    }

    std::vector<uint8_t>& Add(uint64_t base, uint64_t size, bool executable, MemoryRegionType type) {
        Region region;
        region.info.base = base;
        region.info.size = size;
        region.info.readable = true;
        region.info.writable = !executable;
        region.info.executable = executable;
        region.info.type = type;
        region.bytes.assign(static_cast<size_t>(size), 0);
        regions_.push_back(std::move(region));
        return regions_.back().bytes;
    }

    template <typename T>
    void Put(uint64_t address, T value) {
        for (auto& region : regions_)
            if (address >= region.info.base && address + sizeof(T) <= region.info.base + region.info.size) {
                std::memcpy(region.bytes.data() + (address - region.info.base), &value, sizeof(T));
                return;
            }
    }

    const cortex::target::TargetDescriptor& Target() const override { return target_; }
    const cortex::target::CapabilitySet& Capabilities() const override { return target_.capabilities; }
    bool Alive() const override { return true; }

    bool ReadMemory(uint64_t address, void* buffer, size_t size, size_t* bytesRead) const override {
        if (bytesRead) *bytesRead = 0;
        for (const auto& region : regions_) {
            if (address < region.info.base || address + size > region.info.base + region.info.size) continue;
            std::memcpy(buffer, region.bytes.data() + (address - region.info.base), size);
            if (bytesRead) *bytesRead = size;
            return true;
        }
        return false;
    }

    bool WriteMemory(uint64_t, const void*, size_t, size_t*) override { return false; }

    std::vector<MemoryRegion> MemoryRegions() const override {
        std::vector<MemoryRegion> result;
        for (const auto& region : regions_) result.push_back(region.info);
        return result;
    }

private:
    struct Region {
        MemoryRegion info;
        std::vector<uint8_t> bytes;
    };
    cortex::target::TargetDescriptor target_;
    std::vector<Region> regions_;
};

struct Settings {
    uint64_t megabytes = 256;
    int repeat = 5;
    std::string jsonPath;
};

constexpr uint64_t kHeapBase = 0x10000000;
constexpr uint64_t kRegionSize = 1ull << 20;
constexpr uint64_t kModuleBase = 0x00400000;
constexpr uint64_t kModuleSize = 0x00100000;
constexpr int32_t kNeedle = 0x7A5C3E91;
constexpr size_t kPlanted = 1000;

double Median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

template <typename Fn>
std::vector<double> Time(int repeat, Fn&& body) {
    std::vector<double> samples;
    for (int i = 0; i < repeat; ++i) {
        const auto start = std::chrono::steady_clock::now();
        body();
        samples.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    return samples;
}

int failures = 0;

void Expect(bool condition, const std::string& what) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << what << std::endl;
}

json Record(const std::string& name, const std::vector<double>& samples, uint64_t bytes, json extra = json::object()) {
    const double median = Median(samples);
    json row = {{"name", name},
                {"median_ms", median},
                {"min_ms", *std::min_element(samples.begin(), samples.end())},
                {"max_ms", *std::max_element(samples.begin(), samples.end())},
                {"samples", samples.size()}};
    if (bytes) row["throughput_mib_s"] = (static_cast<double>(bytes) / (1024.0 * 1024.0)) / (median / 1000.0);
    for (auto it = extra.begin(); it != extra.end(); ++it) row[it.key()] = it.value();
    std::printf("%-44s %10.2f ms", name.c_str(), median);
    if (bytes) std::printf("  %9.1f MiB/s", row["throughput_mib_s"].get<double>());
    std::printf("\n");
    return row;
}

// A process with `megabytes` of pseudo-random private heap plus one image
// module, and kPlanted copies of kNeedle at known aligned addresses.
std::shared_ptr<BenchProcess> BuildProcess(uint64_t megabytes, std::vector<uint64_t>& planted) {
    auto process = std::make_shared<BenchProcess>();
    std::mt19937_64 random(0xC0DE2026);
    for (uint64_t index = 0; index < megabytes; ++index) {
        auto& bytes = process->Add(kHeapBase + index * kRegionSize * 2, kRegionSize, false, MemoryRegionType::Private);
        for (size_t offset = 0; offset + 8 <= bytes.size(); offset += 8) {
            uint64_t word = random();
            if (word == static_cast<uint32_t>(kNeedle)) ++word;  // only planted needles match
            std::memcpy(bytes.data() + offset, &word, 8);
        }
    }
    process->Add(kModuleBase, kModuleSize, false, MemoryRegionType::Image);

    std::uniform_int_distribution<uint64_t> region(0, megabytes - 1);
    std::uniform_int_distribution<uint64_t> slot(0, kRegionSize / 4 - 1);
    while (planted.size() < kPlanted) {
        const uint64_t address = kHeapBase + region(random) * kRegionSize * 2 + slot(random) * 4;
        if (std::find(planted.begin(), planted.end(), address) != planted.end()) continue;
        planted.push_back(address);
        process->Put<int32_t>(address, kNeedle);
    }
    std::sort(planted.begin(), planted.end());
    return process;
}

ScanQuery Query(ScanDataType type, ScanCompare compare, std::string value = {}) {
    ScanQuery query;
    query.type = type;
    query.compare = compare;
    query.value = std::move(value);
    return query;
}

ScanOptions Options(unsigned threads) {
    ScanOptions options;
    options.threads = threads;
    options.includeImage = true;
    return options;
}

json BenchValueScanner(const Settings& settings, BenchProcess& raw, const std::shared_ptr<BenchProcess>& process,
                       const std::vector<uint64_t>& planted) {
    json rows = json::array();
    const uint64_t bytes = settings.megabytes * kRegionSize + kModuleSize;
    const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
    std::string error;

    for (unsigned threads : {1u, hardware}) {
        ScanStatePtr state;
        const auto samples = Time(settings.repeat, [&] {
            state = ValueScanner::FirstScan(process, Query(ScanDataType::Int32, ScanCompare::Exact, std::to_string(kNeedle)),
                                            Options(threads), &error);
        });
        Expect(state && state->count == planted.size(), "exact i32 first scan finds every planted value");
        rows.push_back(Record("scan.first.exact_i32.threads_" + std::to_string(threads), samples, bytes,
                              {{"threads", threads}, {"results", state ? state->count : 0}}));
    }

    // Next scan over the listed results after half of them change.
    auto first = ValueScanner::FirstScan(process, Query(ScanDataType::Int32, ScanCompare::Exact, std::to_string(kNeedle)),
                                         Options(0), &error);
    for (size_t i = 0; i < planted.size(); i += 2) raw.Put<int32_t>(planted[i], kNeedle + 1);
    {
        ScanStatePtr next;
        const auto samples = Time(settings.repeat, [&] {
            next = ValueScanner::NextScan(process, first, Query(ScanDataType::Int32, ScanCompare::Changed), &error);
        });
        Expect(next && next->count == planted.size() / 2, "changed next scan keeps the modified half");
        rows.push_back(Record("scan.next.changed_i32.listed_1000", samples, 0, {{"results", next ? next->count : 0}}));
    }
    for (size_t i = 0; i < planted.size(); i += 2) raw.Put<int32_t>(planted[i], kNeedle);

    // Unknown initial value: snapshot the whole space, then narrow it.
    {
        ScanStatePtr unknown;
        const auto samples = Time(std::max(1, settings.repeat / 2), [&] {
            unknown = ValueScanner::FirstScan(process, Query(ScanDataType::Int32, ScanCompare::Unknown), Options(0), &error);
        });
        Expect(unknown && unknown->snapshot, "unknown initial value scan keeps a snapshot");
        rows.push_back(Record("scan.first.unknown_i32", samples, bytes));

        for (const uint64_t address : planted) raw.Put<int32_t>(address, kNeedle - 5);
        ScanStatePtr decreased;
        const auto nextSamples = Time(std::max(1, settings.repeat / 2), [&] {
            decreased = ValueScanner::NextScan(process, unknown, Query(ScanDataType::Int32, ScanCompare::Decreased), &error);
        });
        // Random words already decreased by chance never change, so only the
        // planted addresses can be found.
        Expect(decreased && decreased->count == planted.size(), "decreased next scan over a snapshot isolates planted values");
        rows.push_back(Record("scan.next.decreased_i32.from_snapshot", nextSamples, bytes,
                              {{"results", decreased ? decreased->count : 0}}));
        for (const uint64_t address : planted) raw.Put<int32_t>(address, kNeedle);
    }

    // Array of bytes with wildcards (unaligned search).
    {
        uint8_t signature[] = {0x48, 0x8B, 0x05, 0x11, 0x22, 0x33, 0x44, 0x89, 0xC1};
        for (size_t i = 0; i < 16; ++i)
            for (size_t b = 0; b < sizeof(signature); ++b)
                raw.Put<uint8_t>(kModuleBase + 0x1000 + i * 0x800 + b + (i % 3), signature[b]);
        ScanStatePtr aob;
        ScanOptions options = Options(0);
        options.fastScan = false;
        const auto samples = Time(settings.repeat, [&] {
            aob = ValueScanner::FirstScan(process, Query(ScanDataType::ByteArray, ScanCompare::Exact, "48 8B 05 ?? ?? ?? ?? 89 C1"),
                                          options, &error);
        });
        Expect(aob && aob->count == 16, "AOB scan finds every planted signature");
        rows.push_back(Record("scan.first.aob_wildcards", samples, bytes, {{"results", aob ? aob->count : 0}}));
    }
    return rows;
}

json BenchPointerScanner(const Settings& settings, BenchProcess& raw, const std::shared_ptr<BenchProcess>& process) {
    // game.exe+0x100 -> obj (+0x20) -> player (+0x48) = target, plus a
    // pointer-dense heap: every 64th word points somewhere into the heap.
    const uint64_t object = kHeapBase + 0x1000;
    const uint64_t player = kHeapBase + kRegionSize * 2 + 0x2000;
    const uint64_t target = player + 0x48;
    raw.Put<uint64_t>(kModuleBase + 0x100, object);
    raw.Put<uint64_t>(object + 0x20, player);

    const uint64_t heapRegions = std::min<uint64_t>(settings.megabytes, 64);
    std::mt19937_64 random(7);
    std::uniform_int_distribution<uint64_t> region(0, heapRegions - 1);
    std::uniform_int_distribution<uint64_t> word(0, kRegionSize / 8 - 1);
    uint64_t noise = 0;
    for (uint64_t r = 0; r < heapRegions; ++r)
        for (uint64_t offset = 0x4000; offset + 8 <= kRegionSize; offset += 512, ++noise)
            raw.Put<uint64_t>(kHeapBase + r * kRegionSize * 2 + offset,
                              kHeapBase + region(random) * kRegionSize * 2 + word(random) * 8);
    for (uint64_t offset = 0x200; offset < 0x8000; offset += 8)
        raw.Put<uint64_t>(kModuleBase + offset, kHeapBase + region(random) * kRegionSize * 2 + word(random) * 8);

    PointerScanOptions options;
    options.target = target;
    options.maxLevel = 4;
    options.maxOffset = 0x400;
    options.pointerSize = 8;
    options.modules = {{"game.exe", kModuleBase, kModuleSize}};
    PointerScanResult result;
    std::string error;
    bool ok = false;
    const auto samples = Time(std::max(1, settings.repeat / 2), [&] { ok = PointerScanner::Scan(process, options, result, &error); });
    bool found = false;
    for (const auto& path : result.paths)
        if (PointerScanner::Format(result, path) == "game.exe+100 -> 20 -> 48") found = true;
    Expect(ok && found, "pointer scan recovers the planted two-level chain");
    json rows = json::array();
    rows.push_back(Record("pointer.scan.level4_offset400", samples, 0,
                          {{"paths", result.paths.size()},
                           {"pointers_indexed", result.pointersIndexed},
                           {"noise_pointers", noise},
                           {"truncated", result.truncated}}));
    return rows;
}

json BenchReads(const Settings& settings, const std::shared_ptr<BenchProcess>& process) {
    json rows = json::array();
    std::vector<uint8_t> buffer(64 * 1024);
    const uint64_t regions = std::min<uint64_t>(settings.megabytes, 64);
    for (size_t chunk : {size_t(4096), size_t(65536)}) {
        uint64_t total = 0;
        const auto samples = Time(settings.repeat, [&] {
            total = 0;
            for (uint64_t r = 0; r < regions; ++r)
                for (uint64_t offset = 0; offset < kRegionSize; offset += chunk) {
                    size_t read = 0;
                    process->ReadMemory(kHeapBase + r * kRegionSize * 2 + offset, buffer.data(), chunk, &read);
                    total += read;
                }
        });
        Expect(total == regions * kRegionSize, "reads cover every byte");
        rows.push_back(Record("memory.read.chunk_" + std::to_string(chunk), samples, total));
    }
    return rows;
}

// A representative mix of compiler-emitted encodings (prologues, moves,
// RIP-relative loads, calls, conditional branches, SSE, ret/padding).
std::vector<std::vector<uint8_t>> InstructionMix(bool x64) {
    if (x64) {
        return {{0x48, 0x89, 0x5C, 0x24, 0x08},             // mov [rsp+8], rbx
                {0x55},                                     // push rbp
                {0x48, 0x83, 0xEC, 0x20},                   // sub rsp, 0x20
                {0x48, 0x8B, 0x05, 0x10, 0x20, 0x00, 0x00}, // mov rax, [rip+0x2010]
                {0x48, 0x8D, 0x4C, 0x24, 0x30},             // lea rcx, [rsp+0x30]
                {0xE8, 0x00, 0x01, 0x00, 0x00},             // call +0x100
                {0x85, 0xC0},                               // test eax, eax
                {0x74, 0x10},                               // je +0x10
                {0x0F, 0x85, 0x00, 0x02, 0x00, 0x00},       // jne +0x200
                {0xF3, 0x0F, 0x10, 0x41, 0x48},             // movss xmm0, [rcx+0x48]
                {0xF3, 0x0F, 0x58, 0xC1},                   // addss xmm0, xmm1
                {0x8B, 0x81, 0x30, 0x01, 0x00, 0x00},       // mov eax, [rcx+0x130]
                {0x89, 0x81, 0x30, 0x01, 0x00, 0x00},       // mov [rcx+0x130], eax
                {0xFF, 0x15, 0x00, 0x10, 0x00, 0x00},       // call [rip+0x1000]
                {0x48, 0x83, 0xC4, 0x20},                   // add rsp, 0x20
                {0x5D},                                     // pop rbp
                {0xC3},                                     // ret
                {0xCC}};                                    // int3 padding
    }
    return {{0x55},                                   // push ebp
            {0x8B, 0xEC},                             // mov ebp, esp
            {0x83, 0xEC, 0x10},                       // sub esp, 0x10
            {0xA1, 0x00, 0x20, 0x40, 0x00},           // mov eax, [0x402000]
            {0x8D, 0x4D, 0xF0},                       // lea ecx, [ebp-0x10]
            {0xE8, 0x00, 0x01, 0x00, 0x00},           // call +0x100
            {0x85, 0xC0},                             // test eax, eax
            {0x74, 0x10},                             // je +0x10
            {0x0F, 0x85, 0x00, 0x02, 0x00, 0x00},     // jne +0x200
            {0xD9, 0x41, 0x48},                       // fld dword [ecx+0x48]
            {0xF3, 0x0F, 0x10, 0x41, 0x48},           // movss xmm0, [ecx+0x48]
            {0x8B, 0x81, 0x30, 0x01, 0x00, 0x00},     // mov eax, [ecx+0x130]
            {0x89, 0x81, 0x30, 0x01, 0x00, 0x00},     // mov [ecx+0x130], eax
            {0xFF, 0x15, 0x00, 0x10, 0x40, 0x00},     // call [0x401000]
            {0x8B, 0xE5},                             // mov esp, ebp
            {0x5D},                                   // pop ebp
            {0xC3},                                   // ret
            {0xCC}};                                  // int3 padding
}

json BenchDisassembly(const Settings& settings) {
    json rows = json::array();
    const size_t codeBytes = settings.megabytes >= 64 ? (8u << 20) : (1u << 20);
    for (bool x64 : {false, true}) {
        const auto mix = InstructionMix(x64);
        std::vector<uint8_t> code;
        std::mt19937 random(x64 ? 64 : 32);
        std::uniform_int_distribution<size_t> pick(0, mix.size() - 1);
        size_t expected = 0;
        while (code.size() + 16 < codeBytes) {
            const auto& instruction = mix[pick(random)];
            code.insert(code.end(), instruction.begin(), instruction.end());
            ++expected;
        }

        ZydisDecoder decoder;
        if (x64) ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
        else ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32);
        ZydisFormatter formatter;
        ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);

        size_t decoded = 0, invalid = 0;
        uint64_t sink = 0;
        const auto samples = Time(settings.repeat, [&] {
            decoded = invalid = 0;
            size_t offset = 0;
            char text[256];
            while (offset < code.size()) {
                ZydisDecodedInstruction instruction;
                ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
                if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, code.data() + offset, code.size() - offset,
                                                         &instruction, operands))) {
                    ++invalid;
                    ++offset;
                    continue;
                }
                ZydisFormatterFormatInstruction(&formatter, &instruction, operands, instruction.operand_count_visible,
                                                text, sizeof(text), 0x401000 + offset, nullptr);
                sink += static_cast<unsigned char>(text[0]);
                offset += instruction.length;
                ++decoded;
            }
        });
        Expect(decoded == expected && invalid == 0, std::string("disassembly decodes every ") + (x64 ? "x64" : "x86") + " instruction");
        const double median = Median(samples);
        rows.push_back(Record(std::string("disasm.decode_format.") + (x64 ? "x64" : "x86"), samples, code.size(),
                              {{"instructions", decoded},
                               {"instructions_per_second", static_cast<double>(decoded) / (median / 1000.0)},
                               {"sink", sink}}));
    }
    return rows;
}

json Environment() {
    json environment = {
        {"hardware_threads", std::thread::hardware_concurrency()},
        {"pointer_bits", sizeof(void*) * 8},
#if defined(_WIN32)
        {"os", "windows"},
#elif defined(__linux__)
        {"os", "linux"},
#else
        {"os", "other"},
#endif
#if defined(__clang__)
        {"compiler", std::string("clang ") + __clang_version__},
#elif defined(__GNUC__)
        {"compiler", std::string("gcc ") + __VERSION__},
#elif defined(_MSC_VER)
        {"compiler", "msvc " + std::to_string(_MSC_VER)},
#endif
#if defined(NDEBUG)
        {"optimized", true},
#else
        {"optimized", false},
#endif
    };
    return environment;
}

} // namespace

int main(int argc, char** argv) {
    Settings settings;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--quick") {
            settings.megabytes = 8;
            settings.repeat = 1;
        } else if (arg == "--mb" && i + 1 < argc) {
            settings.megabytes = std::max<uint64_t>(4, std::strtoull(argv[++i], nullptr, 10));
        } else if (arg == "--repeat" && i + 1 < argc) {
            settings.repeat = std::max(1, std::atoi(argv[++i]));
        } else if (arg == "--json" && i + 1 < argc) {
            settings.jsonPath = argv[++i];
        } else {
            std::cerr << "usage: cortex_baseline_bench [--quick] [--mb N] [--repeat N] [--json FILE]\n";
            return 2;
        }
    }

    std::printf("Cortex baseline: %llu MiB heap fixture, %d repetition(s)\n",
                static_cast<unsigned long long>(settings.megabytes), settings.repeat);
    std::vector<uint64_t> planted;
    auto process = BuildProcess(settings.megabytes, planted);

    json report = {{"schema", "cortex.baseline.v1"},
                   {"environment", Environment()},
                   {"fixture", {{"heap_mib", settings.megabytes}, {"planted_values", planted.size()}, {"repeat", settings.repeat}}},
                   {"results", json::array()}};
    auto append = [&](json rows) { for (auto& row : rows) report["results"].push_back(std::move(row)); };
    append(BenchValueScanner(settings, *process, process, planted));
    append(BenchPointerScanner(settings, *process, process));
    append(BenchReads(settings, process));
    append(BenchDisassembly(settings));
    report["failures"] = failures;

    if (!settings.jsonPath.empty()) {
        std::ofstream output(settings.jsonPath, std::ios::trunc);
        output << report.dump(2) << '\n';
    }
    if (failures) {
        std::cerr << failures << " benchmark correctness check(s) failed" << std::endl;
        return 1;
    }
    return 0;
}
