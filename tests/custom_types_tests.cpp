// Unit tests for user-defined value types: endianness, bitfields, scaling,
// writing back without disturbing the neighbours, and the table's file format.

#include "services/custom_types.h"
#include "services/memory_tools.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

using cortex::services::CustomType;
using cortex::services::CustomTypeBase;
using cortex::services::CustomTypeTable;
using cortex::services::FormatCustomValue;
using cortex::services::ReadCustomType;
using cortex::services::ValidateCustomType;
using cortex::services::WriteCustomType;

int failures = 0;

#define CHECK(condition)                                                              \
    do {                                                                              \
        if (!(condition)) {                                                           \
            ++failures;                                                               \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #condition \
                      << std::endl;                                                   \
        }                                                                             \
    } while (false)

void TestIntegers() {
    std::string error;
    CustomType type;
    type.name = "big-endian health";
    type.size = 4;
    type.bigEndian = true;
    type.signedValue = false;

    const uint8_t bytes[] = {0x00, 0x00, 0x01, 0x2C};  // 300, most significant first
    double value = 0;
    CHECK(ReadCustomType(type, bytes, sizeof(bytes), value, &error));
    CHECK(value == 300.0);

    uint8_t written[4] = {};
    CHECK(WriteCustomType(type, 513.0, written, sizeof(written), &error));
    CHECK(written[0] == 0 && written[1] == 0 && written[2] == 0x02 && written[3] == 0x01);

    // The same bytes little-endian are a very different number.
    type.bigEndian = false;
    CHECK(ReadCustomType(type, bytes, sizeof(bytes), value, &error));
    CHECK(value == 0x2C010000u);

    // Signed, and narrow: one byte of -3.
    CustomType small;
    small.name = "delta";
    small.size = 1;
    const uint8_t negative[] = {0xFD};
    CHECK(ReadCustomType(small, negative, 1, value, &error));
    CHECK(value == -3.0);
    uint8_t back[1] = {};
    CHECK(WriteCustomType(small, -3.0, back, 1, &error));
    CHECK(back[0] == 0xFD);

    small.signedValue = false;
    CHECK(ReadCustomType(small, negative, 1, value, &error));
    CHECK(value == 253.0);
    CHECK(!WriteCustomType(small, -1.0, back, 1, &error));
    CHECK(error.find("negative") != std::string::npos);
}

void TestBitfields() {
    std::string error;
    CustomType flags;
    flags.name = "level";
    flags.size = 2;
    flags.signedValue = false;
    flags.bitOffset = 4;
    flags.bitCount = 5;

    // 0b0000'0001'0101'0000: bits 4..8 hold 0b10101 = 21.
    const uint16_t stored = 0x0150;
    double value = 0;
    CHECK(ReadCustomType(flags, &stored, sizeof(stored), value, &error));
    CHECK(value == 21.0);

    // Writing the field leaves the bits around it alone.
    uint16_t buffer = 0xFFFF;
    CHECK(WriteCustomType(flags, 3.0, &buffer, sizeof(buffer), &error));
    CHECK(buffer == static_cast<uint16_t>(0xFE3F));
    CHECK(ReadCustomType(flags, &buffer, sizeof(buffer), value, &error));
    CHECK(value == 3.0);

    // A signed bitfield sign-extends from its own width.
    flags.signedValue = true;
    const uint16_t minusOne = 0x01F0;  // bits 4..8 all set
    CHECK(ReadCustomType(flags, &minusOne, sizeof(minusOne), value, &error));
    CHECK(value == -1.0);
}

void TestScaling() {
    std::string error;
    CustomType tenths;
    tenths.name = "health x10";
    tenths.size = 4;
    tenths.scale = 0.1;

    const uint32_t stored = 1234;
    double value = 0;
    CHECK(ReadCustomType(tenths, &stored, sizeof(stored), value, &error));
    CHECK(value > 123.39 && value < 123.41);

    uint32_t written = 0;
    CHECK(WriteCustomType(tenths, 50.0, &written, sizeof(written), &error));
    CHECK(written == 500);

    // A float type with an offset, and its own size rule.
    CustomType celsius;
    celsius.name = "celsius";
    celsius.base = CustomTypeBase::Float;
    celsius.size = 4;
    celsius.offset = -273.15;
    const float kelvin = 300.0f;
    CHECK(ReadCustomType(celsius, &kelvin, sizeof(kelvin), value, &error));
    CHECK(value > 26.8 && value < 26.9);

    celsius.size = 8;
    CHECK(!ValidateCustomType(celsius, &error) && error.find("4 bytes") != std::string::npos);
}

void TestValidationAndFormat() {
    std::string error;
    CustomType type;
    CHECK(!ValidateCustomType(type, &error) && error.find("needs a name") != std::string::npos);
    type.name = "a|b";
    CHECK(!ValidateCustomType(type, &error) && error.find("'|'") != std::string::npos);
    type.name = "ok";
    type.size = 9;
    CHECK(!ValidateCustomType(type, &error) && error.find("1 to 8") != std::string::npos);
    type.size = 4;
    type.bitOffset = 32;
    CHECK(!ValidateCustomType(type, &error) && error.find("past the end") != std::string::npos);
    type.bitOffset = 30;
    type.bitCount = 8;
    CHECK(!ValidateCustomType(type, &error) && error.find("not that many bits") != std::string::npos);
    type.bitOffset = 0;
    type.bitCount = 0;
    type.scale = 0.0;
    CHECK(!ValidateCustomType(type, &error) && error.find("scale of zero") != std::string::npos);
    type.scale = 1.0;
    CHECK(ValidateCustomType(type, &error));

    CHECK(FormatCustomValue(type, 42.0) == "42");
    CustomType scaled = type;
    scaled.scale = 0.1;
    CHECK(FormatCustomValue(scaled, 12.5) == "12.5");
    // Not enough bytes is an error, not a silent zero.
    double value = 0;
    const uint8_t two[2] = {};
    CHECK(!ReadCustomType(type, two, sizeof(two), value, &error));
    CHECK(error.find("Not enough bytes") != std::string::npos);
}

void TestTable() {
    std::string error;
    CustomTypeTable table;
    CustomType type;
    type.name = "health x10";
    type.scale = 0.1;
    CHECK(table.Set(type, &error));
    CustomType flags;
    flags.name = "level";
    flags.size = 2;
    flags.bitOffset = 4;
    flags.bitCount = 5;
    flags.bigEndian = true;
    flags.signedValue = false;
    CHECK(table.Set(flags, &error));
    CHECK(table.Types().size() == 2);

    // Setting the same name replaces rather than duplicates.
    type.scale = 0.01;
    CHECK(table.Set(type, &error));
    CHECK(table.Types().size() == 2);
    CHECK(table.Find("health x10") && table.Find("health x10")->scale == 0.01);
    CHECK(!table.Find("nothing"));

    CustomTypeTable other;
    CHECK(other.Deserialize(table.Serialize(), &error));
    CHECK(other.Types().size() == 2);
    const auto* read = other.Find("level");
    CHECK(read && read->size == 2 && read->bitOffset == 4 && read->bitCount == 5);
    CHECK(read && read->bigEndian && !read->signedValue);

    CHECK(table.Remove("level"));
    CHECK(!table.Remove("level"));
    CHECK(table.Types().size() == 1);

    // A bad file is refused whole, leaving what was loaded before.
    CHECK(!other.Deserialize("health|integer|4\n", &error));
    CHECK(error.find("nine fields") != std::string::npos);
    CHECK(other.Types().size() == 2);
    CHECK(!other.Deserialize("health|integer|99|0|1|0|0|1|0\n", &error));
    CHECK(error.find("1 to 8") != std::string::npos);
}

void TestMemoryDump() {
    using cortex::services::DumpMemoryToFile;
    using cortex::services::FileByteSize;
    using cortex::services::LoadFileToMemory;
    using cortex::services::MemoryDumpReport;

    const uint64_t base = 0x140000000ull;
    std::vector<uint8_t> target(0x3000);
    for (size_t i = 0; i < target.size(); ++i) target[i] = static_cast<uint8_t>(i);

    // The middle page cannot be read, the way a guard page behaves.
    auto read = [&](uint64_t address, void* buffer, size_t size) {
        if (address < base || address + size > base + target.size()) return false;
        const size_t offset = static_cast<size_t>(address - base);
        if (offset < 0x2000 && offset + size > 0x1000) return false;
        std::memcpy(buffer, target.data() + offset, size);
        return true;
    };
    auto write = [&](uint64_t address, const void* buffer, size_t size) {
        if (address < base || address + size > base + target.size()) return false;
        std::memcpy(target.data() + (address - base), buffer, size);
        return true;
    };

    const std::string path = "cortex_custom_types_dump.bin";
    std::string error;
    MemoryDumpReport report;
    CHECK(DumpMemoryToFile(read, path, base, target.size(), report, &error));
    CHECK(report.requested == target.size());
    CHECK(report.holes == 0x1000 && report.read == 0x2000);

    uint64_t size = 0;
    CHECK(FileByteSize(path, size, &error));
    CHECK(size == target.size());

    // Reading it back writes the same bytes, with zeros where the hole was.
    std::fill(target.begin(), target.end(), 0xEE);
    uint64_t written = 0;
    CHECK(LoadFileToMemory(write, path, base, 0, 0, written, &error));
    CHECK(written == size);
    CHECK(target[0] == 0 && target[0x0FFF] == 0xFF);
    CHECK(target[0x1000] == 0 && target[0x1FFF] == 0);
    CHECK(target[0x2000] == 0x00 && target[0x2001] == 0x01);

    // A slice of the file, at a chosen offset.
    written = 0;
    CHECK(LoadFileToMemory(write, path, base + 0x10, 0x2000, 0x10, written, &error));
    CHECK(written == 0x10);
    CHECK(target[0x10] == 0x00 && target[0x11] == 0x01);

    CHECK(!LoadFileToMemory(write, path, base, size + 1, 0, written, &error));
    CHECK(error.find("past the end") != std::string::npos);
    CHECK(!DumpMemoryToFile(read, path, base, 0, report, &error));
    CHECK(!FileByteSize("cortex_no_such_file.bin", size, &error));
    std::remove(path.c_str());
}

} // namespace

int main() {
    TestIntegers();
    TestBitfields();
    TestScaling();
    TestValidationAndFormat();
    TestTable();
    TestMemoryDump();
    if (failures) {
        std::cerr << failures << " check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "custom types tests passed" << std::endl;
    return 0;
}
