// Unit tests for the value scanner, run against an in-memory fake process so
// they need no target, no Windows API and no window.

#include "services/value_scanner.h"

#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

using namespace cortex::services;
using cortex::target::Capability;
using cortex::target::MemoryRegion;
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

class FakeProcess final : public cortex::target::Session {
public:
    struct Region {
        MemoryRegion info;
        std::vector<uint8_t> bytes;
        std::vector<bool> unreadablePages;
    };

    FakeProcess() {
        target_.id = "fake";
        target_.name = "fake.exe";
        target_.capabilities.Add(Capability::MemoryRead).Add(Capability::MemoryScan).Add(Capability::MemoryWrite);
    }

    Region& Add(uint64_t base, uint64_t size, bool writable = true, bool executable = false,
                MemoryRegionType type = MemoryRegionType::Private, bool copyOnWrite = false) {
        Region region;
        region.info.base = base;
        region.info.size = size;
        region.info.readable = true;
        region.info.writable = writable;
        region.info.executable = executable;
        region.info.copyOnWrite = copyOnWrite;
        region.info.type = type;
        region.bytes.assign(static_cast<size_t>(size), 0);
        region.unreadablePages.assign(static_cast<size_t>((size + 4095) / 4096), false);
        regions_.push_back(std::move(region));
        return regions_.back();
    }

    template <typename T>
    void Put(uint64_t address, T value) { Write(address, &value, sizeof(T)); }

    void Write(uint64_t address, const void* data, size_t size) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& region : regions_) {
            if (address >= region.info.base && address + size <= region.info.base + region.info.size) {
                std::memcpy(region.bytes.data() + (address - region.info.base), data, size);
                return;
            }
        }
        std::cerr << "fake write outside memory" << std::endl;
        ++failures;
    }

    const cortex::target::TargetDescriptor& Target() const override { return target_; }
    const cortex::target::CapabilitySet& Capabilities() const override { return target_.capabilities; }
    bool Alive() const override { return true; }

    bool ReadMemory(uint64_t address, void* buffer, size_t size, size_t* bytesRead) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (bytesRead) *bytesRead = 0;
        for (const auto& region : regions_) {
            const uint64_t end = region.info.base + region.info.size;
            if (address < region.info.base || address + size > end) continue;
            const uint64_t offset = address - region.info.base;
            for (uint64_t page = offset / 4096; page <= (offset + size - 1) / 4096; ++page)
                if (region.unreadablePages[static_cast<size_t>(page)]) return false;
            std::memcpy(buffer, region.bytes.data() + offset, size);
            if (bytesRead) *bytesRead = size;
            return true;
        }
        return false;
    }

    bool WriteMemory(uint64_t address, const void* buffer, size_t size, size_t* written) override {
        Write(address, buffer, size);
        if (written) *written = size;
        return true;
    }

    std::vector<MemoryRegion> MemoryRegions() const override {
        std::vector<MemoryRegion> result;
        for (const auto& region : regions_) result.push_back(region.info);
        return result;
    }

private:
    cortex::target::TargetDescriptor target_;
    std::vector<Region> regions_;
    mutable std::mutex mutex_;
};

ScanQuery Query(ScanDataType type, ScanCompare compare, std::string value = {}, std::string value2 = {}) {
    ScanQuery query;
    query.type = type;
    query.compare = compare;
    query.value = std::move(value);
    query.value2 = std::move(value2);
    return query;
}

ScanOptions Options(size_t maxResults = 100000, unsigned threads = 3) {
    ScanOptions options;
    options.maxResults = maxResults;
    options.threads = threads;
    return options;
}

bool Contains(const ScanStatePtr& state, uint64_t address) {
    if (!state) return false;
    for (const auto value : state->addresses)
        if (value == address) return true;
    return false;
}

ScanStatePtr First(const std::shared_ptr<FakeProcess>& process, const ScanQuery& query,
                   const ScanOptions& options = Options()) {
    std::string error;
    auto state = ValueScanner::FirstScan(process, query, options, &error);
    if (!state) std::cerr << "first scan failed: " << error << std::endl;
    return state;
}

ScanStatePtr Next(const std::shared_ptr<FakeProcess>& process, const ScanStatePtr& previous,
                  const ScanQuery& query) {
    std::string error;
    auto state = ValueScanner::NextScan(process, previous, query, &error);
    if (!state) std::cerr << "next scan failed: " << error << std::endl;
    return state;
}

void TestExactIntegers() {
    auto process = std::make_shared<FakeProcess>();
    process->Add(0x10000, 0x3000);
    process->Put<int32_t>(0x10010, 1234567);
    process->Put<int32_t>(0x10021, 1234567);  // unaligned
    process->Put<int16_t>(0x10100, -2);
    process->Put<int64_t>(0x10200, 0x1122334455667788ll);
    process->Put<uint8_t>(0x10300, 0xFE);

    auto state = First(process, Query(ScanDataType::Int32, ScanCompare::Exact, "1234567"));
    CHECK(state && state->Listed() && state->count == 1 && Contains(state, 0x10010));

    auto options = Options();
    options.fastScan = false;
    state = First(process, Query(ScanDataType::Int32, ScanCompare::Exact, "1234567"), options);
    CHECK(state && state->count == 2 && Contains(state, 0x10021));

    state = First(process, Query(ScanDataType::Int16, ScanCompare::Exact, "-2"));
    CHECK(state && Contains(state, 0x10100));
    state = First(process, Query(ScanDataType::Int16, ScanCompare::Exact, "65534"));
    CHECK(state && Contains(state, 0x10100));

    auto hex = Query(ScanDataType::Int64, ScanCompare::Exact, "1122334455667788");
    hex.hex = true;
    state = First(process, hex);
    CHECK(state && state->count == 1 && Contains(state, 0x10200));

    state = First(process, Query(ScanDataType::Byte, ScanCompare::Exact, "0xFE"));
    CHECK(state && Contains(state, 0x10300));

    std::string error;
    CHECK(!ValueScanner::FirstScan(process, Query(ScanDataType::Byte, ScanCompare::Exact, "300"), Options(), &error));
    CHECK(!error.empty());
    CHECK(!ValueScanner::FirstScan(process, Query(ScanDataType::Int32, ScanCompare::Exact, "12x"), Options(), &error));
}

void TestRanges() {
    auto process = std::make_shared<FakeProcess>();
    process->Add(0x20000, 0x1000);
    process->Put<int32_t>(0x20000, 10);
    process->Put<int32_t>(0x20004, 20);
    process->Put<int32_t>(0x20008, 30);
    process->Put<int32_t>(0x2000C, -5);

    auto state = First(process, Query(ScanDataType::Int32, ScanCompare::Between, "15", "30"));
    CHECK(state && state->count == 2 && Contains(state, 0x20004) && Contains(state, 0x20008));

    state = First(process, Query(ScanDataType::Int32, ScanCompare::Between, "30", "15"));
    CHECK(state && state->count == 2);

    state = First(process, Query(ScanDataType::Int32, ScanCompare::Bigger, "19"));
    CHECK(state && state->count == 2);

    state = First(process, Query(ScanDataType::Int32, ScanCompare::Smaller, "0"));
    CHECK(state && state->count == 1 && Contains(state, 0x2000C));

    auto unsignedQuery = Query(ScanDataType::Int32, ScanCompare::Bigger, "100");
    unsignedQuery.unsignedValues = true;
    state = First(process, unsignedQuery);
    CHECK(state && state->count == 1 && Contains(state, 0x2000C));

    auto notQuery = Query(ScanDataType::Int32, ScanCompare::Exact, "0");
    notQuery.invert = true;
    state = First(process, notQuery);
    CHECK(state && state->count == 4);
}

void TestFloats() {
    auto process = std::make_shared<FakeProcess>();
    process->Add(0x30000, 0x1000);
    process->Put<float>(0x30000, 3.14159f);
    process->Put<float>(0x30010, 100.4f);
    process->Put<double>(0x30020, -2.5);

    auto state = First(process, Query(ScanDataType::Float, ScanCompare::Exact, "3.14"));
    CHECK(state && state->count == 1 && Contains(state, 0x30000));

    auto truncated = Query(ScanDataType::Float, ScanCompare::Exact, "3.14");
    truncated.rounding = FloatRounding::Truncated;
    state = First(process, truncated);
    CHECK(state && state->count == 1);

    auto exact = Query(ScanDataType::Float, ScanCompare::Exact, "3.14");
    exact.rounding = FloatRounding::Exact;
    state = First(process, exact);
    CHECK(state && state->count == 0);

    state = First(process, Query(ScanDataType::Float, ScanCompare::Exact, "100"));
    CHECK(state && Contains(state, 0x30010));

    auto extreme = Query(ScanDataType::Float, ScanCompare::Exact, "4");
    extreme.rounding = FloatRounding::Extreme;
    state = First(process, extreme);
    CHECK(state && Contains(state, 0x30000));

    state = First(process, Query(ScanDataType::Double, ScanCompare::Exact, "-2,5"));
    CHECK(state && state->count == 1 && Contains(state, 0x30020));

    CHECK(ValueScanner::Format(state->Value(0), 8, ScanDataType::Double) == "-2.5");
    const float pi = 3.14159f;
    CHECK(ValueScanner::Format(reinterpret_cast<const uint8_t*>(&pi), 4, ScanDataType::Float) == "3.14159");
}

void TestRelativeListScans() {
    auto process = std::make_shared<FakeProcess>();
    process->Add(0x40000, 0x2000);
    for (int i = 0; i < 8; ++i) process->Put<int32_t>(0x40000 + i * 0x100, 50);

    auto state = First(process, Query(ScanDataType::Int32, ScanCompare::Exact, "50"));
    CHECK(state && state->count == 8);

    process->Put<int32_t>(0x40000, 60);
    process->Put<int32_t>(0x40100, 40);
    process->Put<int32_t>(0x40200, 55);
    auto increased = Next(process, state, Query(ScanDataType::Int32, ScanCompare::Increased));
    CHECK(increased && increased->count == 2 && Contains(increased, 0x40000) && Contains(increased, 0x40200));
    CHECK(increased && increased->scanNumber == 2);

    auto by = Next(process, state, Query(ScanDataType::Int32, ScanCompare::IncreasedBy, "10"));
    CHECK(by && by->count == 1 && Contains(by, 0x40000));

    auto decreasedBy = Next(process, state, Query(ScanDataType::Int32, ScanCompare::DecreasedBy, "10"));
    CHECK(decreasedBy && decreasedBy->count == 1 && Contains(decreasedBy, 0x40100));

    auto changed = Next(process, state, Query(ScanDataType::Int32, ScanCompare::Changed));
    CHECK(changed && changed->count == 3);
    auto unchanged = Next(process, state, Query(ScanDataType::Int32, ScanCompare::Unchanged));
    CHECK(unchanged && unchanged->count == 5);

    // Previous values move with each scan; first values stay.
    process->Put<int32_t>(0x40000, 50);
    auto decreased = Next(process, changed, Query(ScanDataType::Int32, ScanCompare::Decreased));
    CHECK(decreased && decreased->count == 1 && Contains(decreased, 0x40000));
    auto same = Next(process, changed, Query(ScanDataType::Int32, ScanCompare::SameAsFirst));
    CHECK(same && same->count == 1 && Contains(same, 0x40000));
    if (same && same->count == 1) {
        int32_t firstValue = 0;
        std::memcpy(&firstValue, same->FirstValue(0), 4);
        CHECK(firstValue == 50);
    }

    auto toFirst = Query(ScanDataType::Int32, ScanCompare::Bigger);
    toFirst.compare = ScanCompare::Increased;
    toFirst.compareToFirst = true;
    auto increasedFromFirst = Next(process, changed, toFirst);
    CHECK(increasedFromFirst && increasedFromFirst->count == 1 && Contains(increasedFromFirst, 0x40200));

    auto removed = ValueScanner::RemoveResults(state, {0, 2});
    CHECK(removed && removed->count == 6 && !Contains(removed, 0x40000) && !Contains(removed, 0x40200));

    std::string error;
    CHECK(!ValueScanner::NextScan(process, state, Query(ScanDataType::Int32, ScanCompare::Unknown), &error));
}

void TestUnknownInitialValue(size_t maxResults) {
    auto process = std::make_shared<FakeProcess>();
    process->Add(0x100000, 0x600000);  // spans two scan pieces
    process->Add(0x800000, 0x1000, false);  // read-only: skipped
    for (uint64_t i = 0; i < 0x600000; i += 4) process->Put<int32_t>(0x100000 + i, static_cast<int32_t>(i));

    auto state = First(process, Query(ScanDataType::Int32, ScanCompare::Unknown), Options(maxResults));
    CHECK(state && state->count == 0x600000 / 4);
    CHECK(state && state->Listed() == (state->count <= maxResults));

    const uint64_t tracked = 0x100000 + 0x3FFFFC;  // last value of the first piece
    const uint64_t other = 0x100000 + 0x400000;    // first value of the second piece
    process->Put<int32_t>(tracked, 7000000);
    process->Put<int32_t>(other, 9000000);
    auto increased = Next(process, state, Query(ScanDataType::Int32, ScanCompare::Increased));
    CHECK(increased && increased->count == 2 && increased->Listed());
    CHECK(Contains(increased, tracked) && Contains(increased, other));
    if (increased && increased->Size() == 2) {
        int32_t current = 0;
        int32_t first = 0;
        std::memcpy(&current, increased->Value(0), 4);
        std::memcpy(&first, increased->FirstValue(0), 4);
        CHECK(current == 7000000 && first == 0x3FFFFC);
    }

    // Unchanged keeps nearly everything, so a small limit stays a snapshot.
    auto unchanged = Next(process, state, Query(ScanDataType::Int32, ScanCompare::Unchanged));
    CHECK(unchanged && unchanged->count == 0x600000 / 4 - 2);
    CHECK(unchanged && unchanged->Listed() == (unchanged->count <= maxResults));

    process->Put<int32_t>(tracked, 0x3FFFFC);
    auto again = Next(process, unchanged, Query(ScanDataType::Int32, ScanCompare::SameAsFirst));
    CHECK(again && again->count == 0x600000 / 4 - 2);
    auto fromStart = Next(process, state, Query(ScanDataType::Int32, ScanCompare::SameAsFirst));
    CHECK(fromStart && fromStart->count == 0x600000 / 4 - 1 && !Contains(fromStart, other));

    auto backToFirst = Query(ScanDataType::Int32, ScanCompare::Unchanged);
    backToFirst.compareToFirst = true;
    auto unchangedFromFirst = Next(process, increased, backToFirst);
    CHECK(unchangedFromFirst && unchangedFromFirst->count == 1 && Contains(unchangedFromFirst, tracked));
}

void TestUnreadablePages() {
    auto process = std::make_shared<FakeProcess>();
    auto& region = process->Add(0x200000, 0x4000);
    region.unreadablePages[1] = true;
    process->Put<int32_t>(0x200010, 77);
    process->Put<int32_t>(0x201010, 77);  // on the unreadable page
    process->Put<int32_t>(0x202010, 77);
    auto state = First(process, Query(ScanDataType::Int32, ScanCompare::Exact, "77"));
    CHECK(state && state->count == 2 && !Contains(state, 0x201010));

    auto unknown = First(process, Query(ScanDataType::Int32, ScanCompare::Unknown), Options(10));
    CHECK(unknown && unknown->count == 0x3000 / 4 && !unknown->Listed());
}

void TestStringsAndBytes() {
    auto process = std::make_shared<FakeProcess>();
    process->Add(0x50000, 0x2000);
    const char text[] = "Player Health";
    process->Write(0x50100, text, sizeof(text) - 1);
    const uint8_t wide[] = {'H', 0, 'e', 0, 'L', 0, 'l', 0, 'O', 0};
    process->Write(0x50300, wide, sizeof(wide));
    const uint8_t code[] = {0x8B, 0x05, 0x44, 0x33, 0x22, 0x11, 0x89, 0xC1};
    process->Write(0x50500, code, sizeof(code));

    auto state = First(process, Query(ScanDataType::String, ScanCompare::Exact, "Health"));
    CHECK(state && state->count == 1 && Contains(state, 0x50107));

    auto folded = Query(ScanDataType::String, ScanCompare::Exact, "player health");
    folded.caseSensitive = false;
    state = First(process, folded);
    CHECK(state && state->count == 1 && Contains(state, 0x50100));
    CHECK(state && ValueScanner::Format(state->Value(0), state->valueSize, ScanDataType::String) == "Player Health");

    auto utf16 = Query(ScanDataType::String, ScanCompare::Exact, "hello");
    utf16.utf16 = true;
    utf16.caseSensitive = false;
    state = First(process, utf16);
    CHECK(state && state->count == 1 && Contains(state, 0x50300));
    CHECK(state && ValueScanner::Format(state->Value(0), state->valueSize, ScanDataType::String, false, false, true) == "HeLlO");

    state = First(process, Query(ScanDataType::ByteArray, ScanCompare::Exact, "8B 05 ?? ?? 2? 11 89"));
    CHECK(state && state->count == 1 && Contains(state, 0x50500));
    state = First(process, Query(ScanDataType::ByteArray, ScanCompare::Exact, "8B05????2211"));
    CHECK(state && state->count == 1);

    process->Write(0x50501, "\x06", 1);
    auto changed = Next(process, state, Query(ScanDataType::ByteArray, ScanCompare::Changed));
    CHECK(changed && changed->count == 1);

    std::string error;
    CHECK(!ValueScanner::NextScan(process, state, Query(ScanDataType::ByteArray, ScanCompare::Increased), &error));
    CHECK(!ValueScanner::FirstScan(process, Query(ScanDataType::ByteArray, ScanCompare::Exact, "?? ??"), Options(), &error));
}

void TestPatternAcrossPieces() {
    auto process = std::make_shared<FakeProcess>();
    process->Add(0x1000000, 0x800000);
    const uint64_t boundary = 0x1000000 + 0x400000 - 3;
    const uint8_t bytes[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02};
    process->Write(boundary, bytes, sizeof(bytes));
    auto state = First(process, Query(ScanDataType::ByteArray, ScanCompare::Exact, "DE AD BE EF 01 02"));
    CHECK(state && state->count == 1 && Contains(state, boundary));

    process->Put<int64_t>(0x1000000 + 0x400000 - 4, 0x0102030405060708ll);
    auto options = Options();
    options.fastScan = false;
    auto wide = Query(ScanDataType::Int64, ScanCompare::Exact, "0102030405060708");
    wide.hex = true;
    state = First(process, wide, options);
    CHECK(state && state->count == 1 && Contains(state, 0x1000000 + 0x400000 - 4));
}

void TestAllNumeric() {
    auto process = std::make_shared<FakeProcess>();
    process->Add(0x60000, 0x1000);
    process->Put<int32_t>(0x60000, 100);
    process->Put<float>(0x60010, 100.0f);
    process->Put<double>(0x60020, 100.0);
    process->Put<int16_t>(0x60032, 100);

    auto state = First(process, Query(ScanDataType::AllNumeric, ScanCompare::Exact, "100"));
    CHECK(state && state->count >= 5);
    bool sawFloat = false;
    bool sawDouble = false;
    bool sawInt32 = false;
    for (size_t i = 0; state && i < state->Size(); ++i) {
        sawFloat |= state->addresses[i] == 0x60010 && state->HitType(i) == ScanDataType::Float;
        sawDouble |= state->addresses[i] == 0x60020 && state->HitType(i) == ScanDataType::Double;
        sawInt32 |= state->addresses[i] == 0x60000 && state->HitType(i) == ScanDataType::Int32;
    }
    CHECK(sawFloat && sawDouble && sawInt32);

    process->Put<float>(0x60010, 150.0f);
    auto increased = Next(process, state, Query(ScanDataType::AllNumeric, ScanCompare::Increased));
    CHECK(increased && increased->count >= 1 && Contains(increased, 0x60010));

    state = First(process, Query(ScanDataType::AllNumeric, ScanCompare::Exact, "150.0"));
    CHECK(state && state->count >= 1);
    for (size_t i = 0; state && i < state->Size(); ++i)
        CHECK(state->HitType(i) == ScanDataType::Float || state->HitType(i) == ScanDataType::Double);
}

void TestRegionFilters() {
    auto process = std::make_shared<FakeProcess>();
    process->Add(0x70000, 0x1000, true);
    process->Add(0x71000, 0x1000, false);
    process->Add(0x72000, 0x1000, true, true, MemoryRegionType::Image);
    process->Add(0x73000, 0x1000, true, false, MemoryRegionType::Mapped);
    process->Add(0x74000, 0x1000, true, false, MemoryRegionType::Image, true);
    for (uint64_t base = 0x70000; base < 0x75000; base += 0x1000) process->Put<int32_t>(base + 0x40, 4242);

    auto state = First(process, Query(ScanDataType::Int32, ScanCompare::Exact, "4242"));
    CHECK(state && state->count == 2 && Contains(state, 0x70040) && Contains(state, 0x72040));

    auto options = Options();
    options.writable = ScanTristate::Any;
    options.copyOnWrite = ScanTristate::Any;
    options.includeMapped = true;
    state = First(process, Query(ScanDataType::Int32, ScanCompare::Exact, "4242"), options);
    CHECK(state && state->count == 5);

    options.executable = ScanTristate::Yes;
    state = First(process, Query(ScanDataType::Int32, ScanCompare::Exact, "4242"), options);
    CHECK(state && state->count == 1 && Contains(state, 0x72040));

    options = Options();
    options.includeImage = false;
    state = First(process, Query(ScanDataType::Int32, ScanCompare::Exact, "4242"), options);
    CHECK(state && state->count == 1 && Contains(state, 0x70040));

    options = Options();
    options.start = 0x72000;
    options.stop = 0x72044;
    state = First(process, Query(ScanDataType::Int32, ScanCompare::Exact, "4242"), options);
    CHECK(state && state->count == 1 && Contains(state, 0x72040));
    options.stop = 0x72043;
    std::string error;
    auto clipped = ValueScanner::FirstScan(process, Query(ScanDataType::Int32, ScanCompare::Exact, "4242"), options, &error);
    CHECK(clipped && clipped->count == 0);
}

void TestLimitAndCancel() {
    auto process = std::make_shared<FakeProcess>();
    process->Add(0x80000, 0x10000);
    auto state = First(process, Query(ScanDataType::Int32, ScanCompare::Exact, "0"), Options(100));
    CHECK(state && state->count == 100 && state->limitReached);
    auto refined = Next(process, state, Query(ScanDataType::Int32, ScanCompare::Unchanged));
    CHECK(refined && refined->count == 100 && refined->limitReached);

    std::atomic_bool cancelled{true};
    std::string error;
    CHECK(!ValueScanner::FirstScan(process, Query(ScanDataType::Int32, ScanCompare::Exact, "0"), Options(), &error, &cancelled));
    CHECK(error == "scan_cancelled");
}

void TestEncodeAndFormat() {
    std::vector<uint8_t> bytes;
    CHECK(ValueScanner::Encode("-1", ScanDataType::Int16, false, false, bytes) && bytes.size() == 2 && bytes[0] == 0xFF && bytes[1] == 0xFF);
    CHECK(ValueScanner::Format(bytes.data(), 2, ScanDataType::Int16) == "-1");
    CHECK(ValueScanner::Format(bytes.data(), 2, ScanDataType::Int16, false, true) == "65535");
    CHECK(ValueScanner::Format(bytes.data(), 2, ScanDataType::Int16, true) == "FFFF");
    CHECK(ValueScanner::Encode("ff", ScanDataType::Byte, true, false, bytes) && bytes.size() == 1 && bytes[0] == 0xFF);
    CHECK(ValueScanner::Encode("1.5", ScanDataType::Float, false, false, bytes) && bytes.size() == 4);
    CHECK(ValueScanner::Encode("AB", ScanDataType::String, false, true, bytes) && bytes.size() == 4 && bytes[1] == 0);
    CHECK(ValueScanner::Encode("90 90 C3", ScanDataType::ByteArray, false, false, bytes) && bytes.size() == 3 && bytes[2] == 0xC3);
    CHECK(!ValueScanner::Encode("90 ??", ScanDataType::ByteArray, false, false, bytes));
    CHECK(!ValueScanner::Encode("", ScanDataType::String, false, false, bytes));
    CHECK(ValueScanner::Format(bytes.data(), 0, ScanDataType::Int32) == "?");
}

} // namespace

int main() {
    TestExactIntegers();
    TestRanges();
    TestFloats();
    TestRelativeListScans();
    TestUnknownInitialValue(100000000);  // listed right away
    TestUnknownInitialValue(1000);       // kept as a snapshot until narrowed
    TestUnreadablePages();
    TestStringsAndBytes();
    TestPatternAcrossPieces();
    TestAllNumeric();
    TestRegionFilters();
    TestLimitAndCancel();
    TestEncodeAndFormat();
    if (failures) {
        std::cerr << failures << " check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "value scanner tests passed" << std::endl;
    return 0;
}
