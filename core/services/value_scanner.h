#pragma once

#include "target/session.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cortex::services {

// Typed value scanner in the style of Cheat Engine: first and next scans over
// a target's memory, unknown initial values, relative comparisons and region
// filters. Every scan produces a new immutable ScanState, so a scan can run
// on a worker thread while the UI keeps showing the previous one, and undoing
// a scan is just going back to the previous state.

enum class ScanDataType : uint8_t {
    Byte = 0,
    Int16,
    Int32,
    Int64,
    Float,
    Double,
    String,
    ByteArray,
    AllNumeric
};

enum class ScanCompare : uint8_t {
    Exact = 0,
    Bigger,
    Smaller,
    Between,
    Unknown,
    Increased,
    IncreasedBy,
    Decreased,
    DecreasedBy,
    Changed,
    Unchanged,
    SameAsFirst
};

// How a typed decimal value matches a stored float or double.
enum class FloatRounding : uint8_t {
    Rounded = 0,  // 1.25 matches [1.245, 1.255)
    Extreme,      // 1.25 matches (0.25, 2.25)
    Truncated,    // 1.25 matches [1.25, 1.26)
    Exact         // the nearest representable value only
};

enum class ScanTristate : uint8_t { Any = 0, Yes, No };

struct ScanQuery {
    ScanDataType type = ScanDataType::Int32;
    ScanCompare compare = ScanCompare::Exact;
    std::string value;
    std::string value2;             // upper bound of Between
    bool hex = false;               // integer values are hexadecimal
    bool unsignedValues = false;    // integers compare as unsigned
    bool invert = false;            // keep the addresses that do NOT match
    bool compareToFirst = false;    // relative scans compare to the first scan
    FloatRounding rounding = FloatRounding::Rounded;
    bool utf16 = false;             // String: UTF-16LE instead of UTF-8
    bool caseSensitive = true;      // String
};

struct ScanOptions {
    uint64_t start = 0;
    uint64_t stop = ~0ull;          // exclusive
    ScanTristate writable = ScanTristate::Yes;
    ScanTristate executable = ScanTristate::Any;
    ScanTristate copyOnWrite = ScanTristate::No;
    bool includePrivate = true;
    bool includeImage = true;
    bool includeMapped = false;
    bool fastScan = true;           // only aligned addresses
    uint32_t alignment = 0;         // 0 = the value size
    unsigned threads = 0;           // 0 = one per hardware thread
    size_t maxResults = 1000000;    // listed results kept by a scan
    std::string tempDirectory;      // unknown-value snapshots; empty = system temp
};

struct ScanProgress {
    std::atomic<uint64_t> total{0};
    std::atomic<uint64_t> done{0};
};

class ScanFile;

struct ScanState {
    ScanDataType type = ScanDataType::Int32;
    size_t valueSize = 4;           // bytes kept per result
    uint32_t alignment = 4;
    ScanQuery firstQuery;           // how the first scan read its values
    ScanOptions options;
    unsigned scanNumber = 1;
    uint64_t count = 0;
    bool limitReached = false;
    uint64_t bytesScanned = 0;
    double milliseconds = 0.0;

    // Listed results, sorted by address.
    std::vector<uint64_t> addresses;
    std::vector<uint8_t> hitTypes;  // AllNumeric only: ScanDataType per result
    std::vector<uint8_t> values;    // valueSize bytes per result, at this scan
    std::vector<uint8_t> firstValues;

    // Unknown initial value scans keep a snapshot of memory instead of a list
    // until a next scan narrows the candidates down to maxResults.
    struct Piece {
        uint64_t base = 0;
        uint64_t size = 0;           // positions start in [base, base + size)
        uint64_t stored = 0;         // bytes kept (size plus overlap)
        uint32_t file = 0;
        uint64_t offset = 0;
        uint32_t firstFile = 0;
        uint64_t firstOffset = 0;
        bool all = true;             // every aligned position is a candidate
        std::vector<uint64_t> bits;  // otherwise, one bit per aligned position
        uint64_t count = 0;
    };
    bool snapshot = false;
    std::vector<Piece> pieces;
    std::vector<std::shared_ptr<ScanFile>> files;
    std::vector<std::shared_ptr<ScanFile>> firstFiles;

    bool Listed() const { return !snapshot; }
    size_t Size() const { return addresses.size(); }
    ScanDataType HitType(size_t index) const;
    size_t HitSize(size_t index) const;
    const uint8_t* Value(size_t index) const { return values.data() + index * valueSize; }
    const uint8_t* FirstValue(size_t index) const { return firstValues.data() + index * valueSize; }
};

using ScanStatePtr = std::shared_ptr<const ScanState>;

class ValueScanner {
public:
    static ScanStatePtr FirstScan(const target::SessionPtr& session,
                                  const ScanQuery& query,
                                  const ScanOptions& options,
                                  std::string* error = nullptr,
                                  const std::atomic_bool* cancelled = nullptr,
                                  ScanProgress* progress = nullptr);

    static ScanStatePtr NextScan(const target::SessionPtr& session,
                                 const ScanStatePtr& previous,
                                 const ScanQuery& query,
                                 std::string* error = nullptr,
                                 const std::atomic_bool* cancelled = nullptr,
                                 ScanProgress* progress = nullptr);

    // A copy of a listed state without the given result indices.
    static ScanStatePtr RemoveResults(const ScanStatePtr& state, std::vector<size_t> indices);

    // The regions a scan with these options reads.
    static std::vector<target::MemoryRegion> SelectRegions(const std::vector<target::MemoryRegion>& regions,
                                                           const ScanOptions& options);

    static size_t TypeSize(ScanDataType type);  // 0 for String and ByteArray
    static bool IsNumeric(ScanDataType type);
    static bool IsInteger(ScanDataType type);
    static const char* TypeName(ScanDataType type);
    static const char* CompareName(ScanCompare compare);
    static bool AllowedFirst(ScanCompare compare);
    static bool AllowedNext(ScanCompare compare, ScanDataType type);
    static bool NeedsValue(ScanCompare compare);
    static bool NeedsSecondValue(ScanCompare compare) { return compare == ScanCompare::Between; }

    // Text conversions shared by the scanner, its results and value editors.
    static bool Encode(const std::string& text, ScanDataType type, bool hex, bool utf16,
                       std::vector<uint8_t>& bytes, std::string* error = nullptr);
    static std::string Format(const uint8_t* data, size_t size, ScanDataType type,
                              bool hex = false, bool unsignedValues = false, bool utf16 = false);
};

} // namespace cortex::services
