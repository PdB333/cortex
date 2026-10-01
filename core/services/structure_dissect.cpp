#include "structure_dissect.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>

namespace cortex::services {
namespace {

bool Fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}

std::string Hex(uint64_t value) {
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%llX", static_cast<unsigned long long>(value));
    return buffer;
}

template <typename T>
T Get(const std::vector<uint8_t>& data, size_t offset) {
    T value{};
    if (offset + sizeof(T) <= data.size()) std::memcpy(&value, data.data() + offset, sizeof(T));
    return value;
}

// A float a game would actually store: not a denormal, not enormous, and
// not the bit pattern of a small integer that happens to decode as one.
bool LooksLikeFloat(float value) {
    if (!std::isfinite(value) || value == 0.0f) return false;
    const float magnitude = std::fabs(value);
    return magnitude > 1e-6f && magnitude < 1e9f;
}

bool LooksLikeDouble(double value) {
    if (!std::isfinite(value) || value == 0.0) return false;
    const double magnitude = std::fabs(value);
    return magnitude > 1e-6 && magnitude < 1e12;
}

// Printable ASCII, at least four characters, ending at a NUL.
size_t AsciiRun(const std::vector<uint8_t>& data, size_t offset) {
    size_t length = 0;
    while (offset + length < data.size() && length < 64) {
        const uint8_t c = data[offset + length];
        if (c == 0) break;
        if (c < 0x20 || c > 0x7E) return 0;
        ++length;
    }
    return length >= 4 && offset + length < data.size() && data[offset + length] == 0 ? length : 0;
}

// The same, as UTF-16: printable characters with a zero high byte.
size_t Utf16Run(const std::vector<uint8_t>& data, size_t offset) {
    size_t length = 0;
    while (offset + length * 2 + 1 < data.size() && length < 64) {
        const uint8_t low = data[offset + length * 2];
        const uint8_t high = data[offset + length * 2 + 1];
        if (low == 0 && high == 0) break;
        if (high != 0 || low < 0x20 || low > 0x7E) return 0;
        ++length;
    }
    return length >= 4 ? length : 0;
}

std::string TextAt(const std::vector<uint8_t>& data, size_t offset, size_t length, bool utf16) {
    std::string text;
    for (size_t i = 0; i < length; ++i) text += static_cast<char>(data[offset + i * (utf16 ? 2 : 1)]);
    return text;
}

std::string FormatNumber(uint64_t value, size_t size) {
    char buffer[64] = {};
    // Small values read better as decimal, large ones as hex.
    int64_t signedValue = 0;
    switch (size) {
        case 1: signedValue = static_cast<int8_t>(value); break;
        case 2: signedValue = static_cast<int16_t>(value); break;
        case 4: signedValue = static_cast<int32_t>(value); break;
        default: signedValue = static_cast<int64_t>(value); break;
    }
    if (signedValue > -1000000 && signedValue < 1000000)
        std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(signedValue));
    else
        std::snprintf(buffer, sizeof(buffer), "%llX", static_cast<unsigned long long>(value));
    return buffer;
}

std::string FormatReal(double value) {
    char buffer[64] = {};
    std::snprintf(buffer, sizeof(buffer), "%g", value);
    return buffer;
}

} // namespace

const char* DissectKindName(DissectKind kind) {
    switch (kind) {
        case DissectKind::Integer: return "integer";
        case DissectKind::Float: return "float";
        case DissectKind::Double: return "double";
        case DissectKind::Pointer: return "pointer";
        case DissectKind::Text: return "text";
        case DissectKind::TextUtf16: return "text (UTF-16)";
        default: return "?";
    }
}

bool DissectStructure(const MemoryReader& read, const std::vector<uint64_t>& instances,
                      const DissectOptions& options, const AddressPredicate& isAddress,
                      const AddressDescriber& describe, DissectResult& result, std::string* error) {
    result = DissectResult{};
    if (!read) return Fail(error, "No reader");
    if (instances.empty()) return Fail(error, "Give at least one address");
    if (!options.size) return Fail(error, "Nothing to dissect: the size is zero");
    const size_t alignment = options.alignment ? options.alignment : 4;
    const size_t pointerSize = options.pointerSize == 4 ? 4u : 8u;

    // Every instance is read whole, so the fields all describe the same
    // moment rather than drifting while the target runs.
    std::vector<std::vector<uint8_t>> blocks;
    for (const auto instance : instances) {
        std::vector<uint8_t> block(options.size);
        if (!read(instance, block.data(), block.size())) {
            ++result.unreadable;
            continue;
        }
        result.instances.push_back(instance);
        blocks.push_back(std::move(block));
    }
    if (blocks.empty()) return Fail(error, "None of those addresses could be read");

    const auto& first = blocks.front();
    for (size_t offset = 0; offset + alignment <= options.size;) {
        DissectField field;
        field.offset = offset;
        field.size = alignment;
        field.name = "field_" + Hex(offset);

        // What the first instance holds decides how the offset is read; the
        // others are then shown the same way, so they can be compared.
        size_t textLength = 0;
        bool textUtf16 = false;
        if (options.guessText) {
            textLength = AsciiRun(first, offset);
            if (!textLength) {
                textLength = Utf16Run(first, offset);
                textUtf16 = textLength != 0;
            }
        }
        const uint64_t wide = pointerSize == 8 ? Get<uint64_t>(first, offset)
                                               : static_cast<uint64_t>(Get<uint32_t>(first, offset));
        const bool pointer = options.guessPointers && offset + pointerSize <= options.size &&
                             offset % pointerSize == 0 && wide && isAddress && isAddress(wide);

        if (textLength) {
            field.kind = textUtf16 ? DissectKind::TextUtf16 : DissectKind::Text;
            field.size = textUtf16 ? textLength * 2 : textLength;
            field.note = TextAt(first, offset, textLength, textUtf16);
        } else if (pointer) {
            field.kind = DissectKind::Pointer;
            field.size = pointerSize;
            if (describe) field.note = describe(wide);
        } else if (options.guessFloats && alignment >= 4 && LooksLikeFloat(Get<float>(first, offset))) {
            field.kind = DissectKind::Float;
            field.size = 4;
        } else if (options.guessFloats && alignment >= 8 && offset + 8 <= options.size &&
                   LooksLikeDouble(Get<double>(first, offset))) {
            field.kind = DissectKind::Double;
            field.size = 8;
        } else {
            field.kind = DissectKind::Integer;
        }

        bool allZero = true;
        for (const auto& block : blocks) {
            std::string text;
            switch (field.kind) {
                case DissectKind::Text:
                case DissectKind::TextUtf16: {
                    const size_t length = field.kind == DissectKind::TextUtf16 ? Utf16Run(block, offset)
                                                                              : AsciiRun(block, offset);
                    text = length ? TextAt(block, offset, length, field.kind == DissectKind::TextUtf16) : "";
                    allZero = false;
                    break;
                }
                case DissectKind::Pointer: {
                    const uint64_t value = pointerSize == 8 ? Get<uint64_t>(block, offset)
                                                            : static_cast<uint64_t>(Get<uint32_t>(block, offset));
                    text = Hex(value);
                    allZero = allZero && value == 0;
                    break;
                }
                case DissectKind::Float: {
                    const float value = Get<float>(block, offset);
                    text = FormatReal(value);
                    allZero = allZero && value == 0.0f;
                    break;
                }
                case DissectKind::Double: {
                    const double value = Get<double>(block, offset);
                    text = FormatReal(value);
                    allZero = allZero && value == 0.0;
                    break;
                }
                default: {
                    uint64_t value = 0;
                    switch (alignment) {
                        case 1: value = Get<uint8_t>(block, offset); break;
                        case 2: value = Get<uint16_t>(block, offset); break;
                        case 8: value = Get<uint64_t>(block, offset); break;
                        default: value = Get<uint32_t>(block, offset); break;
                    }
                    text = FormatNumber(value, alignment);
                    allZero = allZero && value == 0;
                    break;
                }
            }
            field.differs = field.differs || (!field.values.empty() && field.values.front() != text);
            field.values.push_back(std::move(text));
        }
        field.zero = allZero;
        // A field wider than the step covers the offsets after it, so a
        // pointer does not come back as two integers and a string is not
        // repeated once per character group.
        const size_t covered = std::max<size_t>(alignment, ((field.size + alignment - 1) / alignment) * alignment);
        result.fields.push_back(std::move(field));
        offset += covered;
    }
    if (error) error->clear();
    return true;
}

// ------------------------------------------------------------------ spider

bool SpiderPointers(const MemoryReader& read, uint64_t root, const SpiderOptions& options,
                    const AddressPredicate& isAddress, const AddressDescriber& describe,
                    std::vector<SpiderNode>& nodes, std::string* error) {
    nodes.clear();
    if (!read) return Fail(error, "No reader");
    if (!isAddress) return Fail(error, "No way to tell an address from a number");
    if (options.maxLevel < 1) return Fail(error, "Follow at least one level");
    const size_t pointerSize = options.pointerSize == 4 ? 4u : 8u;
    const size_t alignment = options.alignment ? options.alignment : pointerSize;
    const uint32_t span = options.maxOffset;

    // The same address is expanded once, however many paths reach it.
    std::set<uint64_t> seen{root};

    // Reads every pointer in [base, base + span] and records it.
    const auto expand = [&](uint64_t base, int level, int parent) {
        std::vector<uint8_t> block(static_cast<size_t>(span) + pointerSize);
        if (!read(base, block.data(), block.size())) return;
        for (size_t offset = 0; offset + pointerSize <= block.size(); offset += alignment) {
            if (nodes.size() >= options.maxNodes) return;
            const uint64_t value = pointerSize == 8
                                       ? Get<uint64_t>(block, offset)
                                       : static_cast<uint64_t>(Get<uint32_t>(block, offset));
            if (!value || !isAddress(value)) continue;
            SpiderNode node;
            node.address = base + offset;
            node.value = value;
            node.offset = static_cast<uint32_t>(offset);
            node.level = level;
            node.parent = parent;
            if (describe) node.note = describe(value);
            nodes.push_back(std::move(node));
        }
    };

    expand(root, 0, -1);
    // Breadth first, so the shortest path to an address is the one kept.
    for (size_t index = 0; index < nodes.size() && nodes.size() < options.maxNodes; ++index) {
        const auto node = nodes[index];
        if (node.level + 1 >= options.maxLevel) continue;
        if (!seen.insert(node.value).second) continue;
        expand(node.value, node.level + 1, static_cast<int>(index));
    }
    if (error) error->clear();
    return true;
}

std::string SpiderPath(const std::vector<SpiderNode>& nodes, size_t index) {
    if (index >= nodes.size()) return {};
    std::vector<uint32_t> offsets = SpiderOffsets(nodes, index);
    std::string text = "base";
    for (const auto offset : offsets) text += " -> +" + Hex(offset);
    return text;
}

std::vector<uint32_t> SpiderOffsets(const std::vector<SpiderNode>& nodes, size_t index) {
    std::vector<uint32_t> offsets;
    if (index >= nodes.size()) return offsets;
    for (int at = static_cast<int>(index); at >= 0 && at < static_cast<int>(nodes.size());
         at = nodes[static_cast<size_t>(at)].parent) {
        offsets.push_back(nodes[static_cast<size_t>(at)].offset);
        if (nodes[static_cast<size_t>(at)].parent < 0) break;
    }
    std::reverse(offsets.begin(), offsets.end());
    return offsets;
}

} // namespace cortex::services
