// Unit tests for dissecting a structure and for the pointer spider, against
// a fake process holding two instances of the same little object.

#include "services/structure_dissect.h"

#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

using cortex::services::DissectKind;
using cortex::services::DissectOptions;
using cortex::services::DissectResult;
using cortex::services::DissectStructure;
using cortex::services::SpiderNode;
using cortex::services::SpiderOffsets;
using cortex::services::SpiderOptions;
using cortex::services::SpiderPath;
using cortex::services::SpiderPointers;

int failures = 0;

#define CHECK(condition)                                                              \
    do {                                                                              \
        if (!(condition)) {                                                           \
            ++failures;                                                               \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #condition \
                      << std::endl;                                                   \
        }                                                                             \
    } while (false)

constexpr uint64_t kBase = 0x140000000ull;
constexpr uint64_t kFirst = kBase + 0x1000;
constexpr uint64_t kSecond = kBase + 0x1100;
constexpr uint64_t kName = kBase + 0x2000;
constexpr uint64_t kInner = kBase + 0x3000;

// 0x00 pointer to a name, 0x08 health (int), 0x0C speed (float),
// 0x10 pointer to an inner object, 0x18 a constant, 0x20 inline text.
struct Fake {
    std::vector<uint8_t> memory = std::vector<uint8_t>(0x8000, 0);

    uint8_t* At(uint64_t address) { return memory.data() + (address - kBase); }

    Fake() {
        Write64(kFirst + 0x00, kName);
        Write32(kFirst + 0x08, 100);
        WriteFloat(kFirst + 0x0C, 1.5f);
        Write64(kFirst + 0x10, kInner);
        Write32(kFirst + 0x18, 0xABCD);
        std::memcpy(At(kFirst + 0x20), "hero", 5);

        Write64(kSecond + 0x00, kName);
        Write32(kSecond + 0x08, 42);
        WriteFloat(kSecond + 0x0C, 9.25f);
        Write64(kSecond + 0x10, kInner);
        Write32(kSecond + 0x18, 0xABCD);
        std::memcpy(At(kSecond + 0x20), "mage", 5);

        std::memcpy(At(kName), "player name", 12);
        // The inner object points at the name too, two levels down.
        Write64(kInner + 0x08, kName);
    }

    void Write32(uint64_t address, uint32_t value) { std::memcpy(At(address), &value, 4); }
    void Write64(uint64_t address, uint64_t value) { std::memcpy(At(address), &value, 8); }
    void WriteFloat(uint64_t address, float value) { std::memcpy(At(address), &value, 4); }

    cortex::services::MemoryReader Reader() {
        return [this](uint64_t address, void* buffer, size_t size) {
            if (address < kBase || address + size > kBase + memory.size()) return false;
            std::memcpy(buffer, At(address), size);
            return true;
        };
    }
};

bool PlausibleAddress(uint64_t value) { return value >= kBase && value < kBase + 0x8000; }

std::string Describe(uint64_t value) {
    char buffer[64] = {};
    std::snprintf(buffer, sizeof(buffer), "fake+%llX", static_cast<unsigned long long>(value - kBase));
    return buffer;
}

const cortex::services::DissectField* FieldAt(const DissectResult& result, uint64_t offset) {
    for (const auto& field : result.fields)
        if (field.offset == offset) return &field;
    return nullptr;
}

void TestDissect() {
    Fake fake;
    DissectOptions options;
    options.size = 0x30;
    DissectResult result;
    std::string error;
    CHECK(DissectStructure(fake.Reader(), {kFirst, kSecond}, options, PlausibleAddress, Describe, result,
                           &error));
    CHECK(result.instances.size() == 2 && result.unreadable == 0);
    // A field wider than the step covers the offsets it spans, so the two
    // pointers and the string each take one row rather than several.
    CHECK(result.fields.size() == 10);
    for (size_t i = 1; i < result.fields.size(); ++i)
        CHECK(result.fields[i].offset >= result.fields[i - 1].offset + result.fields[i - 1].size);

    const auto* name = FieldAt(result, 0x00);
    CHECK(name && name->kind == DissectKind::Pointer && name->size == 8);
    CHECK(name && name->note == Describe(kName) && !name->differs);

    const auto* health = FieldAt(result, 0x08);
    CHECK(health && health->kind == DissectKind::Integer);
    CHECK(health && health->values.size() == 2 && health->values[0] == "100" && health->values[1] == "42");
    CHECK(health && health->differs);

    const auto* speed = FieldAt(result, 0x0C);
    CHECK(speed && speed->kind == DissectKind::Float && speed->differs);
    CHECK(speed && speed->values[0] == "1.5" && speed->values[1] == "9.25");

    const auto* inner = FieldAt(result, 0x10);
    CHECK(inner && inner->kind == DissectKind::Pointer && !inner->differs);

    const auto* constant = FieldAt(result, 0x18);
    CHECK(constant && constant->kind == DissectKind::Integer && !constant->differs && !constant->zero);

    const auto* text = FieldAt(result, 0x20);
    CHECK(text && text->kind == DissectKind::Text && text->note == "hero");
    CHECK(text && text->values.size() == 2 && text->values[1] == "mage" && text->differs);

    // A field nothing ever writes reads as zero, which is how padding shows.
    const auto* padding = FieldAt(result, 0x1C);
    CHECK(padding && padding->zero && !padding->differs);

    // One instance alone still describes the layout.
    DissectResult single;
    CHECK(DissectStructure(fake.Reader(), {kFirst}, options, PlausibleAddress, Describe, single, &error));
    CHECK(single.instances.size() == 1);
    for (const auto& field : single.fields) CHECK(!field.differs);

    // An unreadable instance is counted, not fatal; all of them is fatal.
    DissectResult partial;
    CHECK(DissectStructure(fake.Reader(), {kFirst, 0x10ull}, options, PlausibleAddress, Describe, partial,
                           &error));
    CHECK(partial.unreadable == 1 && partial.instances.size() == 1);
    DissectResult none;
    CHECK(!DissectStructure(fake.Reader(), {0x10ull}, options, PlausibleAddress, Describe, none, &error));
    CHECK(error.find("could be read") != std::string::npos);
    CHECK(!DissectStructure(fake.Reader(), {}, options, PlausibleAddress, Describe, none, &error));
}

void TestSpider() {
    Fake fake;
    SpiderOptions options;
    options.maxLevel = 2;
    options.maxOffset = 0x20;
    options.alignment = 8;
    std::vector<SpiderNode> nodes;
    std::string error;
    CHECK(SpiderPointers(fake.Reader(), kFirst, options, PlausibleAddress, Describe, nodes, &error));
    CHECK(!nodes.empty());

    // The two pointers at the root, at +0 and +10.
    size_t roots = 0;
    int innerIndex = -1;
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].level != 0) continue;
        ++roots;
        CHECK(nodes[i].parent == -1);
        if (nodes[i].value == kInner) innerIndex = static_cast<int>(i);
    }
    CHECK(roots == 2);
    CHECK(innerIndex >= 0);
    CHECK(nodes[static_cast<size_t>(innerIndex)].offset == 0x10);
    CHECK(nodes[static_cast<size_t>(innerIndex)].note == Describe(kInner));

    // The inner object's own pointer is one level further in.
    int deep = -1;
    for (size_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].level == 1 && nodes[i].parent == innerIndex) deep = static_cast<int>(i);
    CHECK(deep >= 0);
    if (deep >= 0) {
        CHECK(nodes[static_cast<size_t>(deep)].offset == 0x08);
        CHECK(SpiderPath(nodes, static_cast<size_t>(deep)) == "base -> +10 -> +8");
        CHECK(SpiderOffsets(nodes, static_cast<size_t>(deep)) == std::vector<uint32_t>({0x10, 0x8}));
    }

    // One level only lists what the root itself points at.
    SpiderOptions shallow = options;
    shallow.maxLevel = 1;
    std::vector<SpiderNode> flat;
    CHECK(SpiderPointers(fake.Reader(), kFirst, shallow, PlausibleAddress, Describe, flat, &error));
    for (const auto& node : flat) CHECK(node.level == 0);

    // The node limit stops the walk rather than running away.
    SpiderOptions capped = options;
    capped.maxNodes = 1;
    std::vector<SpiderNode> few;
    CHECK(SpiderPointers(fake.Reader(), kFirst, capped, PlausibleAddress, Describe, few, &error));
    CHECK(few.size() <= 1);

    CHECK(!SpiderPointers(fake.Reader(), kFirst, options, nullptr, Describe, nodes, &error));
    SpiderOptions bad = options;
    bad.maxLevel = 0;
    CHECK(!SpiderPointers(fake.Reader(), kFirst, bad, PlausibleAddress, Describe, nodes, &error));
}

} // namespace

int main() {
    TestDissect();
    TestSpider();
    if (failures) {
        std::cerr << failures << " check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "structure dissect tests passed" << std::endl;
    return 0;
}
