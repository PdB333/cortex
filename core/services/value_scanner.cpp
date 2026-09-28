#include "value_scanner.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <locale>
#include <mutex>
#include <new>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <type_traits>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace cortex::services {

// Unknown initial value snapshots live in temporary files so a scan of a
// large process does not have to keep its whole writable memory in RAM.
class ScanFile {
public:
    static std::shared_ptr<ScanFile> Create(const std::string& directory, std::string* error) {
        std::error_code fsError;
        std::filesystem::path root = directory.empty()
            ? std::filesystem::temp_directory_path(fsError)
            : std::filesystem::u8path(directory);
        if (root.empty()) {
            if (error) *error = "scan_temp_directory_unavailable";
            return nullptr;
        }
        static std::atomic<uint64_t> counter{0};
        const auto stamp = static_cast<unsigned long long>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        const auto thread = std::hash<std::thread::id>{}(std::this_thread::get_id());
        std::shared_ptr<ScanFile> file(new ScanFile());
        file->path_ = root / ("cortex-scan-" + std::to_string(stamp) + "-" +
                              std::to_string(thread % 100000) + "-" +
                              std::to_string(counter.fetch_add(1)) + ".tmp");
        file->stream_.open(file->path_, std::ios::in | std::ios::out |
                                            std::ios::binary | std::ios::trunc);
        if (!file->stream_) {
            if (error) *error = "scan_temp_file_failed:" + file->path_.u8string();
            return nullptr;
        }
        return file;
    }

    ~ScanFile() {
        stream_.close();
        std::error_code fsError;
        std::filesystem::remove(path_, fsError);
    }

    bool Append(const uint8_t* data, size_t size, uint64_t& offset) {
        std::lock_guard<std::mutex> lock(mutex_);
        offset = size_;
        stream_.seekp(static_cast<std::streamoff>(size_));
        stream_.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        if (!stream_) {
            stream_.clear();
            return false;
        }
        size_ += size;
        return true;
    }

    bool Read(uint64_t offset, uint8_t* data, size_t size) const {
        std::lock_guard<std::mutex> lock(mutex_);
        stream_.seekg(static_cast<std::streamoff>(offset));
        stream_.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(size));
        const bool ok = stream_ && static_cast<size_t>(stream_.gcount()) == size;
        if (!ok) stream_.clear();
        return ok;
    }

private:
    ScanFile() = default;

    std::filesystem::path path_;
    mutable std::mutex mutex_;
    mutable std::fstream stream_;
    uint64_t size_ = 0;
};

namespace {

constexpr uint64_t kPieceSize = 4ull * 1024 * 1024;
constexpr uint64_t kPageSize = 4096;
constexpr size_t kListBatch = 8192;
constexpr uint64_t kListWindow = 64 * 1024;

const ScanDataType kNumericTypes[] = {
    ScanDataType::Byte, ScanDataType::Int16, ScanDataType::Int32,
    ScanDataType::Int64, ScanDataType::Float, ScanDataType::Double
};

bool IsCancelled(const std::atomic_bool* cancelled) {
    return cancelled && cancelled->load(std::memory_order_relaxed);
}

void SetError(std::string* error, std::string text) {
    if (error) *error = std::move(text);
}

std::string Trim(const std::string& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return text.substr(begin, end - begin);
}

int CountTrailingZeros(uint64_t value) {
#if defined(_MSC_VER)
    unsigned long index = 0;
    _BitScanForward64(&index, value);
    return static_cast<int>(index);
#else
    return __builtin_ctzll(value);
#endif
}

uint8_t LowerAscii(uint8_t value) {
    return value >= 'A' && value <= 'Z' ? static_cast<uint8_t>(value + 32) : value;
}

// ---------------------------------------------------------------- parsing

bool ParseInteger(const std::string& raw, bool hex, size_t size, uint64_t& out, std::string* error) {
    std::string text = Trim(raw);
    bool negative = false;
    if (!text.empty() && (text[0] == '-' || text[0] == '+')) {
        negative = text[0] == '-';
        text.erase(0, 1);
    }
    int base = hex ? 16 : 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text.erase(0, 2);
    } else if (!text.empty() && text[0] == '$') {
        base = 16;
        text.erase(0, 1);
    }
    if (text.empty()) {
        SetError(error, "Enter a value");
        return false;
    }
    for (const char ch : text) {
        const bool valid = base == 16 ? std::isxdigit(static_cast<unsigned char>(ch)) != 0
                                      : std::isdigit(static_cast<unsigned char>(ch)) != 0;
        if (!valid) {
            SetError(error, base == 16 ? "Not a hexadecimal integer" : "Not an integer");
            return false;
        }
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long long magnitude = std::strtoull(text.c_str(), &end, base);
    if (errno == ERANGE || !end || *end != '\0') {
        SetError(error, "Value is out of range");
        return false;
    }
    const unsigned bits = static_cast<unsigned>(size * 8);
    const uint64_t mask = bits >= 64 ? ~0ull : ((1ull << bits) - 1);
    if (negative) {
        const uint64_t limit = 1ull << (bits - 1);
        if (magnitude > limit) {
            SetError(error, "Value is out of range for this type");
            return false;
        }
        out = (0ull - static_cast<uint64_t>(magnitude)) & mask;
    } else {
        if (magnitude > mask) {
            SetError(error, "Value is out of range for this type");
            return false;
        }
        out = static_cast<uint64_t>(magnitude);
    }
    return true;
}

bool ParseReal(const std::string& raw, double& value, int& decimals, std::string* error) {
    std::string text = Trim(raw);
    std::replace(text.begin(), text.end(), ',', '.');
    if (text.empty()) {
        SetError(error, "Enter a value");
        return false;
    }
    std::istringstream stream(text);
    stream.imbue(std::locale::classic());
    stream >> value;
    if (!stream || !stream.eof()) {
        // eof is not set when the whole text was consumed exactly; check the rest.
        if (!stream) {
            SetError(error, "Not a number");
            return false;
        }
        std::string rest;
        stream >> rest;
        if (!rest.empty()) {
            SetError(error, "Not a number");
            return false;
        }
    }
    if (!std::isfinite(value)) {
        SetError(error, "Not a finite number");
        return false;
    }
    decimals = 0;
    const auto dot = text.find('.');
    const auto exponent = text.find_first_of("eE");
    if (dot != std::string::npos) {
        const size_t stop = exponent == std::string::npos ? text.size() : exponent;
        for (size_t i = dot + 1; i < stop && std::isdigit(static_cast<unsigned char>(text[i])); ++i)
            ++decimals;
    }
    if (exponent != std::string::npos) {
        const int power = std::atoi(text.c_str() + exponent + 1);
        decimals = std::max(0, decimals - power);
    }
    decimals = std::min(decimals, 15);
    return true;
}

void AppendUtf16(std::vector<uint8_t>& out, uint32_t unit) {
    out.push_back(static_cast<uint8_t>(unit & 0xff));
    out.push_back(static_cast<uint8_t>((unit >> 8) & 0xff));
}

std::vector<uint8_t> Utf8ToUtf16(const std::string& text) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        uint32_t code = 0xFFFD;
        size_t length = 1;
        if (c < 0x80) {
            code = c;
        } else if ((c >> 5) == 0x6 && i + 1 < text.size()) {
            code = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(text[i + 1]) & 0x3Fu);
            length = 2;
        } else if ((c >> 4) == 0xE && i + 2 < text.size()) {
            code = ((c & 0x0Fu) << 12) | ((static_cast<unsigned char>(text[i + 1]) & 0x3Fu) << 6) |
                   (static_cast<unsigned char>(text[i + 2]) & 0x3Fu);
            length = 3;
        } else if ((c >> 3) == 0x1E && i + 3 < text.size()) {
            code = ((c & 0x07u) << 18) | ((static_cast<unsigned char>(text[i + 1]) & 0x3Fu) << 12) |
                   ((static_cast<unsigned char>(text[i + 2]) & 0x3Fu) << 6) |
                   (static_cast<unsigned char>(text[i + 3]) & 0x3Fu);
            length = 4;
        }
        i += length;
        if (code >= 0x10000) {
            code -= 0x10000;
            AppendUtf16(out, 0xD800 + (code >> 10));
            AppendUtf16(out, 0xDC00 + (code & 0x3FF));
        } else {
            AppendUtf16(out, code);
        }
    }
    return out;
}

void AppendUtf8(std::string& out, uint32_t code) {
    if (code < 0x80) {
        out += static_cast<char>(code);
    } else if (code < 0x800) {
        out += static_cast<char>(0xC0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
        out += static_cast<char>(0xE0 | (code >> 12));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (code >> 18));
        out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
}

std::string PrintableUtf16(const uint8_t* data, size_t size) {
    std::string out;
    for (size_t i = 0; i + 1 < size; i += 2) {
        uint32_t unit = data[i] | (static_cast<uint32_t>(data[i + 1]) << 8);
        if (unit >= 0xD800 && unit <= 0xDBFF && i + 3 < size) {
            const uint32_t low = data[i + 2] | (static_cast<uint32_t>(data[i + 3]) << 8);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
                i += 2;
            }
        }
        if (unit < 0x20 || unit == 0x7F) unit = '.';
        AppendUtf8(out, unit);
    }
    return out;
}

std::string PrintableUtf8(const uint8_t* data, size_t size) {
    std::string out;
    out.reserve(size);
    for (size_t i = 0; i < size; ++i)
        out += data[i] < 0x20 || data[i] == 0x7F ? '.' : static_cast<char>(data[i]);
    return out;
}

struct PatternSpec {
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> mask;
    bool fold = false;
    bool utf16 = false;
    int anchor = -1;
};

bool ParseByteArray(const std::string& raw, PatternSpec& spec, std::string* error) {
    spec.bytes.clear();
    spec.mask.clear();
    std::istringstream stream(raw);
    std::string token;
    auto nibble = [](char ch, uint8_t& value) {
        if (ch >= '0' && ch <= '9') value = static_cast<uint8_t>(ch - '0');
        else if (ch >= 'a' && ch <= 'f') value = static_cast<uint8_t>(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') value = static_cast<uint8_t>(ch - 'A' + 10);
        else return false;
        return true;
    };
    while (stream >> token) {
        if (token == "?" || token == "*" || token == "??" || token == "**") {
            spec.bytes.push_back(0);
            spec.mask.push_back(0);
            continue;
        }
        if (token.size() % 2 != 0) {
            SetError(error, "Bytes must look like: 8B 05 ?? ?? 4? 00");
            return false;
        }
        for (size_t i = 0; i < token.size(); i += 2) {
            uint8_t value = 0;
            uint8_t mask = 0;
            for (int half = 0; half < 2; ++half) {
                const char ch = token[i + static_cast<size_t>(half)];
                const int shift = half == 0 ? 4 : 0;
                uint8_t digit = 0;
                if (ch == '?' || ch == '*') continue;
                if (!nibble(ch, digit)) {
                    SetError(error, "Bytes must look like: 8B 05 ?? ?? 4? 00");
                    return false;
                }
                value = static_cast<uint8_t>(value | (digit << shift));
                mask = static_cast<uint8_t>(mask | (0xF << shift));
            }
            spec.bytes.push_back(value);
            spec.mask.push_back(mask);
        }
    }
    if (spec.bytes.empty()) {
        SetError(error, "Enter one or more bytes");
        return false;
    }
    if (spec.bytes.size() > 4096) {
        SetError(error, "The pattern is longer than 4096 bytes");
        return false;
    }
    if (std::all_of(spec.mask.begin(), spec.mask.end(), [](uint8_t m) { return m == 0; })) {
        SetError(error, "The pattern needs at least one known byte");
        return false;
    }
    return true;
}

void FinishPattern(PatternSpec& spec) {
    spec.anchor = -1;
    if (spec.fold) {
        if (spec.utf16) {
            for (size_t i = 0; i + 1 < spec.bytes.size(); i += 2)
                if (spec.bytes[i + 1] == 0) spec.bytes[i] = LowerAscii(spec.bytes[i]);
        } else {
            for (auto& byte : spec.bytes) byte = LowerAscii(byte);
        }
        return;  // letters have two spellings; no single anchor byte
    }
    for (size_t i = 0; i < spec.mask.size(); ++i) {
        if (spec.mask[i] == 0xFF) {
            spec.anchor = static_cast<int>(i);
            break;
        }
    }
}

bool BuildPattern(const ScanQuery& query, ScanDataType type, bool utf16, PatternSpec& spec,
                  std::string* error) {
    spec = PatternSpec{};
    if (type == ScanDataType::ByteArray) {
        if (!ParseByteArray(query.value, spec, error)) return false;
    } else {
        if (query.value.empty()) {
            SetError(error, "Enter a non-empty string");
            return false;
        }
        spec.utf16 = utf16;
        spec.bytes = utf16 ? Utf8ToUtf16(query.value)
                           : std::vector<uint8_t>(query.value.begin(), query.value.end());
        if (spec.bytes.size() > 4096) {
            SetError(error, "The string is longer than 4096 bytes");
            return false;
        }
        spec.mask.assign(spec.bytes.size(), 0xFF);
        spec.fold = !query.caseSensitive;
    }
    FinishPattern(spec);
    return true;
}

bool PatternAt(const PatternSpec& spec, const uint8_t* data) {
    const size_t size = spec.bytes.size();
    if (!spec.fold) {
        for (size_t i = 0; i < size; ++i)
            if ((data[i] & spec.mask[i]) != spec.bytes[i]) return false;
        return true;
    }
    if (!spec.utf16) {
        for (size_t i = 0; i < size; ++i)
            if (LowerAscii(data[i]) != spec.bytes[i]) return false;
        return true;
    }
    for (size_t i = 0; i + 1 < size; i += 2) {
        const uint8_t high = data[i + 1];
        const uint8_t low = high == 0 ? LowerAscii(data[i]) : data[i];
        if (low != spec.bytes[i] || high != spec.bytes[i + 1]) return false;
    }
    return true;
}

// ---------------------------------------------------------------- numbers

struct Interval {
    double lo = 0.0;
    double hi = 0.0;
    bool loInclusive = true;
    bool hiInclusive = false;
    bool Contains(double value) const {
        return (loInclusive ? value >= lo : value > lo) && (hiInclusive ? value <= hi : value < hi);
    }
};

Interval RoundingInterval(double value, int decimals, FloatRounding rounding) {
    const double step = std::pow(10.0, -static_cast<double>(std::clamp(decimals, 0, 15)));
    switch (rounding) {
        case FloatRounding::Extreme: return {value - 1.0, value + 1.0, false, false};
        case FloatRounding::Truncated:
            return value >= 0.0 ? Interval{value, value + step, true, false}
                                : Interval{value - step, value, false, true};
        case FloatRounding::Exact: return {value, value, true, true};
        case FloatRounding::Rounded:
        default: return {value - step / 2.0, value + step / 2.0, true, false};
    }
}

struct NumericSpec {
    ScanDataType type = ScanDataType::Int32;
    bool isUnsigned = false;
    bool valid = false;
    ScanCompare compare = ScanCompare::Exact;
    bool invert = false;
    bool toFirst = false;
    uint64_t x = 0;
    uint64_t y = 0;
    double fx = 0.0;
    double fy = 0.0;
    Interval interval;
};

bool BuildNumericSpec(ScanDataType type, const ScanQuery& query, NumericSpec& spec, std::string* error) {
    spec = NumericSpec{};
    spec.type = type;
    spec.isUnsigned = query.unsignedValues;
    spec.compare = query.compare;
    spec.invert = query.invert;
    spec.toFirst = query.compareToFirst;
    if (!ValueScanner::NeedsValue(query.compare)) {
        spec.valid = true;
        return true;
    }
    const size_t size = ValueScanner::TypeSize(type);
    if (ValueScanner::IsInteger(type)) {
        if (!ParseInteger(query.value, query.hex, size, spec.x, error)) return false;
        if (query.compare == ScanCompare::Between &&
            !ParseInteger(query.value2, query.hex, size, spec.y, error)) return false;
    } else {
        int decimals = 0;
        if (!ParseReal(query.value, spec.fx, decimals, error)) return false;
        if (query.compare == ScanCompare::Between) {
            int ignored = 0;
            if (!ParseReal(query.value2, spec.fy, ignored, error)) return false;
        }
        double exact = spec.fx;
        if (query.rounding == FloatRounding::Exact && query.compare == ScanCompare::Exact)
            exact = type == ScanDataType::Float ? static_cast<double>(static_cast<float>(spec.fx)) : spec.fx;
        spec.interval = RoundingInterval(exact, decimals, query.rounding);
    }
    spec.valid = true;
    return true;
}

template <typename T>
T FromBits(uint64_t bits) {
    using U = std::make_unsigned_t<T>;
    const U value = static_cast<U>(bits);
    T result{};
    std::memcpy(&result, &value, sizeof(T));
    return result;
}

template <typename T>
T WrapAdd(T left, T right) {
    using U = std::make_unsigned_t<T>;
    return static_cast<T>(static_cast<U>(static_cast<U>(left) + static_cast<U>(right)));
}

template <typename T>
T WrapSub(T left, T right) {
    using U = std::make_unsigned_t<T>;
    return static_cast<T>(static_cast<U>(static_cast<U>(left) - static_cast<U>(right)));
}

template <typename T>
bool SameBits(T left, T right) {
    return std::memcmp(&left, &right, sizeof(T)) == 0;
}

template <typename T>
struct Matcher {
    ScanCompare compare = ScanCompare::Exact;
    bool invert = false;
    bool toFirst = false;
    T x{};
    T y{};
    Interval interval;

    explicit Matcher(const NumericSpec& spec)
        : compare(spec.compare), invert(spec.invert), toFirst(spec.toFirst), interval(spec.interval) {
        if constexpr (std::is_integral_v<T>) {
            x = FromBits<T>(spec.x);
            y = FromBits<T>(spec.y);
        } else {
            x = static_cast<T>(spec.fx);
            y = static_cast<T>(spec.fy);
        }
        if (compare == ScanCompare::Between && y < x) std::swap(x, y);
    }

    bool Match(T now, T previous, T first) const {
        const T base = toFirst ? first : previous;
        bool ok = false;
        switch (compare) {
            case ScanCompare::Exact:
                if constexpr (std::is_floating_point_v<T>) ok = interval.Contains(static_cast<double>(now));
                else ok = now == x;
                break;
            case ScanCompare::Bigger: ok = now > x; break;
            case ScanCompare::Smaller: ok = now < x; break;
            case ScanCompare::Between: ok = now >= x && now <= y; break;
            case ScanCompare::Unknown: ok = true; break;
            case ScanCompare::Increased: ok = now > base; break;
            case ScanCompare::Decreased: ok = now < base; break;
            case ScanCompare::IncreasedBy:
                if constexpr (std::is_floating_point_v<T>)
                    ok = interval.Contains(static_cast<double>(now) - static_cast<double>(base));
                else ok = now == WrapAdd(base, x);
                break;
            case ScanCompare::DecreasedBy:
                if constexpr (std::is_floating_point_v<T>)
                    ok = interval.Contains(static_cast<double>(base) - static_cast<double>(now));
                else ok = now == WrapSub(base, x);
                break;
            case ScanCompare::Changed: ok = !SameBits(now, base); break;
            case ScanCompare::Unchanged: ok = SameBits(now, base); break;
            case ScanCompare::SameAsFirst: ok = SameBits(now, first); break;
        }
        return ok != invert;
    }
};

template <typename F>
void DispatchNumeric(ScanDataType type, bool isUnsigned, F&& function) {
    switch (type) {
        case ScanDataType::Byte:
            if (isUnsigned) function(uint8_t{}); else function(int8_t{});
            break;
        case ScanDataType::Int16:
            if (isUnsigned) function(uint16_t{}); else function(int16_t{});
            break;
        case ScanDataType::Int32:
            if (isUnsigned) function(uint32_t{}); else function(int32_t{});
            break;
        case ScanDataType::Int64:
            if (isUnsigned) function(uint64_t{}); else function(int64_t{});
            break;
        case ScanDataType::Float: function(float{}); break;
        case ScanDataType::Double: function(double{}); break;
        default: break;
    }
}

template <typename T>
T Load(const uint8_t* data) {
    T value{};
    std::memcpy(&value, data, sizeof(T));
    return value;
}

// Tests one stored value of any numeric type; used where the type varies
// per result (All) and for listed next scans.
bool MatchBytes(const NumericSpec& spec, const uint8_t* now, const uint8_t* previous, const uint8_t* first) {
    if (!spec.valid) return spec.invert;
    bool ok = false;
    DispatchNumeric(spec.type, spec.isUnsigned, [&](auto tag) {
        using T = decltype(tag);
        const Matcher<T> matcher(spec);
        ok = matcher.Match(Load<T>(now), Load<T>(previous), Load<T>(first));
    });
    return ok;
}

// ---------------------------------------------------------------- memory

uint64_t FirstAligned(uint64_t base, uint32_t alignment) {
    if (alignment <= 1) return 0;
    const uint64_t remainder = base % alignment;
    return remainder == 0 ? 0 : alignment - remainder;
}

// Positions tested in a piece: p = first + k * alignment, k < count.
uint64_t PositionCount(uint64_t base, uint64_t size, uint64_t stored, size_t valueSize, uint32_t alignment) {
    if (stored < valueSize || size == 0) return 0;
    const uint64_t first = FirstAligned(base, alignment);
    const uint64_t last = std::min<uint64_t>(size - 1, stored - valueSize);
    if (last < first) return 0;
    return (last - first) / std::max<uint32_t>(1, alignment) + 1;
}

struct PieceBuffer {
    std::vector<uint8_t> data;
    std::vector<uint8_t> badPages;  // empty when every page was read
    uint64_t firstPage = 0;

    bool Touches(uint64_t address, size_t size) const {
        if (badPages.empty()) return false;
        const uint64_t from = (address / kPageSize) - firstPage;
        const uint64_t to = ((address + size - 1) / kPageSize) - firstPage;
        for (uint64_t page = from; page <= to && page < badPages.size(); ++page)
            if (badPages[page]) return true;
        return false;
    }
};

// Reads a piece; pages that cannot be read are zero-filled and flagged.
bool ReadPiece(const target::SessionPtr& session, uint64_t base, uint64_t size, PieceBuffer& buffer) {
    buffer.data.resize(static_cast<size_t>(size));
    buffer.badPages.clear();
    buffer.firstPage = base / kPageSize;
    size_t read = 0;
    if (session->ReadMemory(base, buffer.data.data(), buffer.data.size(), &read)) return true;

    const uint64_t lastPage = (base + size - 1) / kPageSize;
    buffer.badPages.assign(static_cast<size_t>(lastPage - buffer.firstPage + 1), 0);
    bool any = false;
    for (uint64_t page = buffer.firstPage; page <= lastPage; ++page) {
        const uint64_t from = std::max(base, page * kPageSize);
        const uint64_t to = std::min(base + size, (page + 1) * kPageSize);
        uint8_t* target = buffer.data.data() + (from - base);
        if (session->ReadMemory(from, target, static_cast<size_t>(to - from), &read)) {
            any = true;
        } else {
            std::memset(target, 0, static_cast<size_t>(to - from));
            buffer.badPages[static_cast<size_t>(page - buffer.firstPage)] = 1;
        }
    }
    return any;
}

struct PieceTask {
    uint64_t base = 0;
    uint64_t size = 0;
    uint64_t stored = 0;
};

std::vector<PieceTask> MakePieces(const std::vector<target::MemoryRegion>& regions, size_t overlap) {
    std::vector<PieceTask> pieces;
    for (const auto& region : regions) {
        for (uint64_t offset = 0; offset < region.size; offset += kPieceSize) {
            PieceTask piece;
            piece.base = region.base + offset;
            piece.size = std::min(kPieceSize, region.size - offset);
            const uint64_t remaining = region.size - offset - piece.size;
            piece.stored = piece.size + std::min<uint64_t>(overlap, remaining);
            pieces.push_back(piece);
        }
    }
    return pieces;
}

unsigned WorkerCount(unsigned requested, size_t work) {
    unsigned count = requested ? requested : std::thread::hardware_concurrency();
    count = std::clamp(count, 1u, 32u);
    return static_cast<unsigned>(std::min<size_t>(count, std::max<size_t>(1, work)));
}

// Runs work(index, worker) over [0, count) on a small thread pool. work
// returns false to stop early without an error (result limit reached).
template <typename Work>
bool RunParallel(size_t count, unsigned workers, const std::atomic_bool* cancelled,
                 std::string* error, Work&& work) {
    std::atomic<size_t> next{0};
    std::atomic<bool> stop{false};
    std::mutex errorMutex;
    std::string firstError;
    auto body = [&](unsigned worker) {
        try {
            for (;;) {
                if (stop.load(std::memory_order_relaxed) || IsCancelled(cancelled)) break;
                const size_t index = next.fetch_add(1);
                if (index >= count) break;
                if (!work(index, worker)) stop.store(true);
            }
        } catch (const std::bad_alloc&) {
            std::lock_guard<std::mutex> lock(errorMutex);
            if (firstError.empty()) firstError = "scan_out_of_memory";
            stop.store(true);
        } catch (const std::exception& ex) {
            std::lock_guard<std::mutex> lock(errorMutex);
            if (firstError.empty()) firstError = ex.what();
            stop.store(true);
        }
    };
    if (workers <= 1) {
        body(0);
    } else {
        std::vector<std::thread> threads;
        threads.reserve(workers - 1);
        for (unsigned worker = 1; worker < workers; ++worker) threads.emplace_back(body, worker);
        body(0);
        for (auto& thread : threads) thread.join();
    }
    if (IsCancelled(cancelled)) {
        SetError(error, "scan_cancelled");
        return false;
    }
    if (!firstError.empty()) {
        SetError(error, firstError);
        return false;
    }
    return true;
}

struct Hits {
    std::vector<uint64_t> addresses;
    std::vector<uint8_t> types;
    std::vector<uint8_t> values;
    std::vector<uint8_t> firstValues;

    void Add(uint64_t address, const uint8_t* value, size_t size, size_t stride) {
        addresses.push_back(address);
        const size_t offset = values.size();
        values.resize(offset + stride, 0);
        std::memcpy(values.data() + offset, value, std::min(size, stride));
    }
};

void AppendHits(ScanState& state, std::vector<Hits>& parts, bool withFirst) {
    size_t total = 0;
    for (const auto& part : parts) total += part.addresses.size();
    state.addresses.reserve(total);
    state.values.reserve(total * state.valueSize);
    if (state.type == ScanDataType::AllNumeric) state.hitTypes.reserve(total);
    if (withFirst) state.firstValues.reserve(total * state.valueSize);
    for (auto& part : parts) {
        state.addresses.insert(state.addresses.end(), part.addresses.begin(), part.addresses.end());
        state.values.insert(state.values.end(), part.values.begin(), part.values.end());
        state.hitTypes.insert(state.hitTypes.end(), part.types.begin(), part.types.end());
        if (withFirst)
            state.firstValues.insert(state.firstValues.end(), part.firstValues.begin(), part.firstValues.end());
        part = Hits();
    }
    if (!withFirst) state.firstValues = state.values;
    state.count = state.addresses.size();
}

class Stopwatch {
public:
    double Milliseconds() const {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_).count();
    }

private:
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

bool CheckSession(const target::SessionPtr& session, std::string* error) {
    if (!session || !session->Alive()) {
        SetError(error, "no_active_session");
        return false;
    }
    if (!session->Capabilities().Has(target::Capability::MemoryRead)) {
        SetError(error, "memory_read_not_supported");
        return false;
    }
    return true;
}

// Worker-owned snapshot files, created on first use.
struct WorkerFiles {
    std::vector<std::shared_ptr<ScanFile>> files;
    std::string directory;
    std::mutex mutex;
    std::string error;

    ScanFile* Get(unsigned worker) {
        if (files[worker]) return files[worker].get();
        std::string createError;
        auto file = ScanFile::Create(directory, &createError);
        if (!file) throw std::runtime_error(createError);
        files[worker] = std::move(file);
        return files[worker].get();
    }
};

// ---------------------------------------------------------------- first scans

ScanStatePtr ListFirstScan(const target::SessionPtr& session, std::shared_ptr<ScanState> state,
                           const std::vector<PieceTask>& pieces,
                           const std::vector<NumericSpec>& specs, const PatternSpec* pattern,
                           std::string* error, const std::atomic_bool* cancelled,
                           ScanProgress* progress) {
    const size_t cap = std::max<size_t>(1, state->options.maxResults);
    const unsigned workers = WorkerCount(state->options.threads, pieces.size());
    std::vector<Hits> hits(pieces.size());
    std::vector<PieceBuffer> buffers(workers);
    std::atomic<uint64_t> found{0};
    std::atomic<bool> limit{false};
    const bool all = state->type == ScanDataType::AllNumeric;
    const size_t stride = state->valueSize;
    const ScanOptions& options = state->options;

    const bool ok = RunParallel(pieces.size(), workers, cancelled, error, [&](size_t index, unsigned worker) {
        const PieceTask& piece = pieces[index];
        PieceBuffer& buffer = buffers[worker];
        Hits& out = hits[index];
        const bool readable = ReadPiece(session, piece.base, piece.stored, buffer);
        if (progress) progress->done.fetch_add(piece.stored, std::memory_order_relaxed);
        if (!readable) return true;
        const uint8_t* data = buffer.data.data();

        auto take = [&]() {
            if (found.fetch_add(1) >= cap) {
                limit.store(true);
                return false;
            }
            return true;
        };

        if (pattern) {
            const size_t length = pattern->bytes.size();
            if (piece.stored < length) return true;
            const uint64_t last = std::min<uint64_t>(piece.size, piece.stored - length + 1);
            for (uint64_t p = 0; p < last; ++p) {
                if (pattern->anchor >= 0) {
                    const void* hit = std::memchr(data + p + pattern->anchor,
                                                  pattern->bytes[static_cast<size_t>(pattern->anchor)],
                                                  static_cast<size_t>(last - p));
                    if (!hit) break;
                    p = static_cast<uint64_t>(static_cast<const uint8_t*>(hit) - data) -
                        static_cast<uint64_t>(pattern->anchor);
                }
                if (!PatternAt(*pattern, data + p) || buffer.Touches(piece.base + p, length)) continue;
                if (!take()) return false;
                out.Add(piece.base + p, data + p, length, stride);
            }
            return true;
        }

        if (!all) {
            bool keepGoing = true;
            DispatchNumeric(specs.front().type, specs.front().isUnsigned, [&](auto tag) {
                using T = decltype(tag);
                const Matcher<T> matcher(specs.front());
                const uint32_t alignment = state->alignment;
                const uint64_t first = FirstAligned(piece.base, alignment);
                if (piece.stored < sizeof(T)) return;
                const uint64_t last = std::min<uint64_t>(piece.size, piece.stored - sizeof(T) + 1);
                for (uint64_t p = first; p < last; p += alignment) {
                    const T now = Load<T>(data + p);
                    if (!matcher.Match(now, now, now)) continue;
                    if (buffer.Touches(piece.base + p, sizeof(T))) continue;
                    if (!take()) {
                        keepGoing = false;
                        return;
                    }
                    out.Add(piece.base + p, data + p, sizeof(T), stride);
                }
            });
            return keepGoing;
        }

        for (uint64_t p = 0; p < piece.size; ++p) {
            const uint64_t address = piece.base + p;
            for (const auto& spec : specs) {
                if (!spec.valid) continue;
                const size_t size = ValueScanner::TypeSize(spec.type);
                const uint32_t alignment = options.alignment
                    ? options.alignment
                    : (options.fastScan ? static_cast<uint32_t>(size) : 1u);
                if (p + size > piece.stored || (alignment > 1 && address % alignment != 0)) continue;
                const uint8_t* value = data + p;
                if (!MatchBytes(spec, value, value, value) || buffer.Touches(address, size)) continue;
                if (!take()) return false;
                out.Add(address, value, size, stride);
                out.types.push_back(static_cast<uint8_t>(spec.type));
            }
        }
        return true;
    });
    if (!ok) return nullptr;

    AppendHits(*state, hits, false);
    state->limitReached = limit.load();
    return state;
}

ScanStatePtr UnknownFirstScan(const target::SessionPtr& session, std::shared_ptr<ScanState> state,
                              const std::vector<PieceTask>& pieces, std::string* error,
                              const std::atomic_bool* cancelled, ScanProgress* progress) {
    const size_t cap = std::max<size_t>(1, state->options.maxResults);
    const unsigned workers = WorkerCount(state->options.threads, pieces.size());
    const size_t size = state->valueSize;
    const uint32_t alignment = state->alignment;
    std::vector<PieceBuffer> buffers(workers);
    std::vector<ScanState::Piece> out(pieces.size());
    std::vector<Hits> hits(pieces.size());
    std::atomic<uint64_t> listed{0};
    WorkerFiles files;
    files.files.resize(workers);
    files.directory = state->options.tempDirectory;

    const bool ok = RunParallel(pieces.size(), workers, cancelled, error, [&](size_t index, unsigned worker) {
        const PieceTask& task = pieces[index];
        PieceBuffer& buffer = buffers[worker];
        const bool readable = ReadPiece(session, task.base, task.stored, buffer);
        if (progress) progress->done.fetch_add(task.stored, std::memory_order_relaxed);
        if (!readable) return true;

        ScanState::Piece& piece = out[index];
        piece.base = task.base;
        piece.size = task.size;
        piece.stored = task.stored;
        const uint64_t positions = PositionCount(task.base, task.size, task.stored, size, alignment);
        const uint64_t first = FirstAligned(task.base, alignment);
        if (buffer.badPages.empty()) {
            piece.all = true;
            piece.count = positions;
        } else {
            piece.all = false;
            piece.bits.assign(static_cast<size_t>((positions + 63) / 64), 0);
            for (uint64_t k = 0; k < positions; ++k) {
                if (buffer.Touches(task.base + first + k * alignment, size)) continue;
                piece.bits[static_cast<size_t>(k / 64)] |= 1ull << (k % 64);
                ++piece.count;
            }
        }
        if (piece.count == 0) return true;

        uint32_t file = worker;
        uint64_t offset = 0;
        if (!files.Get(worker)->Append(buffer.data.data(), buffer.data.size(), offset))
            throw std::runtime_error("scan_temp_file_write_failed");
        piece.file = piece.firstFile = file;
        piece.offset = piece.firstOffset = offset;

        // Small processes are listed right away.
        if (listed.load(std::memory_order_relaxed) <= cap) {
            Hits& list = hits[index];
            for (uint64_t k = 0; k < positions; ++k) {
                if (!piece.all && !(piece.bits[static_cast<size_t>(k / 64)] & (1ull << (k % 64)))) continue;
                if (listed.fetch_add(1) >= cap) break;
                const uint64_t p = first + k * alignment;
                list.Add(task.base + p, buffer.data.data() + p, size, size);
            }
        }
        return true;
    });
    if (!ok) return nullptr;

    uint64_t total = 0;
    for (const auto& piece : out) total += piece.count;
    state->count = total;
    if (total <= cap) {
        AppendHits(*state, hits, false);
        state->count = total;
        return state;
    }
    state->snapshot = true;
    for (auto& piece : out)
        if (piece.count > 0) state->pieces.push_back(std::move(piece));
    state->files = files.files;
    state->firstFiles = files.files;
    return state;
}

// ---------------------------------------------------------------- next scans

ScanStatePtr ListNextScan(const target::SessionPtr& session, const ScanState& previous,
                          std::shared_ptr<ScanState> state, const std::vector<NumericSpec>& specs,
                          const PatternSpec* pattern, ScanCompare compare, bool invert,
                          std::string* error, const std::atomic_bool* cancelled, ScanProgress* progress) {
    const size_t count = previous.Size();
    const size_t batches = (count + kListBatch - 1) / kListBatch;
    const unsigned workers = WorkerCount(previous.options.threads, batches);
    std::vector<Hits> hits(batches);
    const size_t stride = previous.valueSize;

    auto specFor = [&](ScanDataType type) -> const NumericSpec* {
        for (const auto& spec : specs)
            if (spec.type == type) return &spec;
        return nullptr;
    };

    const bool ok = RunParallel(batches, workers, cancelled, error, [&](size_t batch, unsigned) {
        const size_t begin = batch * kListBatch;
        const size_t end = std::min(count, begin + kListBatch);
        Hits& out = hits[batch];
        std::vector<uint8_t> window;
        std::vector<uint8_t> single(std::max<size_t>(stride, 8));
        for (size_t i = begin; i < end;) {
            const uint64_t start = previous.addresses[i];
            size_t j = i;
            uint64_t stop = start;
            while (j < end && previous.addresses[j] + previous.HitSize(j) <= start + kListWindow) {
                stop = std::max<uint64_t>(stop, previous.addresses[j] + previous.HitSize(j));
                ++j;
            }
            if (j == i) {  // a single result larger than the window
                stop = start + previous.HitSize(i);
                j = i + 1;
            }
            window.resize(static_cast<size_t>(stop - start));
            size_t read = 0;
            const bool windowOk = session->ReadMemory(start, window.data(), window.size(), &read);
            for (size_t k = i; k < j; ++k) {
                const uint64_t address = previous.addresses[k];
                const size_t size = previous.HitSize(k);
                const uint8_t* now = nullptr;
                if (windowOk) {
                    now = window.data() + (address - start);
                } else {
                    if (single.size() < size) single.resize(size);
                    if (!session->ReadMemory(address, single.data(), size, &read)) continue;
                    now = single.data();
                }
                const uint8_t* before = previous.Value(k);
                const uint8_t* first = previous.FirstValue(k);
                bool match = false;
                if (pattern) {
                    switch (compare) {
                        case ScanCompare::Exact: match = PatternAt(*pattern, now); break;
                        case ScanCompare::Changed: match = std::memcmp(now, before, size) != 0; break;
                        case ScanCompare::Unchanged: match = std::memcmp(now, before, size) == 0; break;
                        case ScanCompare::SameAsFirst: match = std::memcmp(now, first, size) == 0; break;
                        default: break;
                    }
                    match = match != invert;
                } else {
                    const ScanDataType type = previous.HitType(k);
                    const NumericSpec* spec = specFor(type);
                    match = spec && MatchBytes(*spec, now, before, first);
                }
                if (!match) continue;
                out.Add(address, now, size, stride);
                const size_t offset = out.firstValues.size();
                out.firstValues.resize(offset + stride);
                std::memcpy(out.firstValues.data() + offset, first, stride);
                if (previous.type == ScanDataType::AllNumeric)
                    out.types.push_back(previous.hitTypes[k]);
            }
            if (progress) progress->done.fetch_add(j - i, std::memory_order_relaxed);
            i = j;
        }
        return true;
    });
    if (!ok) return nullptr;
    AppendHits(*state, hits, true);
    state->limitReached = previous.limitReached;
    return state;
}

ScanStatePtr SnapshotNextScan(const target::SessionPtr& session, const ScanState& previous,
                              std::shared_ptr<ScanState> state, const NumericSpec& spec,
                              std::string* error, const std::atomic_bool* cancelled,
                              ScanProgress* progress) {
    const size_t cap = std::max<size_t>(1, previous.options.maxResults);
    const size_t count = previous.pieces.size();
    const unsigned workers = WorkerCount(previous.options.threads, count);
    const size_t size = previous.valueSize;
    const uint32_t alignment = previous.alignment;
    const bool needFirst = spec.toFirst || spec.compare == ScanCompare::SameAsFirst;
    std::vector<PieceBuffer> buffers(workers);
    std::vector<std::vector<uint8_t>> before(workers);
    std::vector<std::vector<uint8_t>> firsts(workers);
    std::vector<ScanState::Piece> out(count);
    std::vector<Hits> hits(count);
    std::atomic<uint64_t> listed{0};
    WorkerFiles files;
    files.files.resize(workers);
    files.directory = previous.options.tempDirectory;

    const bool ok = RunParallel(count, workers, cancelled, error, [&](size_t index, unsigned worker) {
        const ScanState::Piece& old = previous.pieces[index];
        PieceBuffer& buffer = buffers[worker];
        const bool readable = ReadPiece(session, old.base, old.stored, buffer);
        if (progress) progress->done.fetch_add(old.stored, std::memory_order_relaxed);
        if (!readable) return true;

        auto& prior = before[worker];
        prior.resize(static_cast<size_t>(old.stored));
        const auto& priorFile = previous.files.at(old.file);
        if (!priorFile || !priorFile->Read(old.offset, prior.data(), prior.size()))
            throw std::runtime_error("scan_snapshot_read_failed");
        auto& firstBytes = firsts[worker];
        bool firstLoaded = false;
        auto loadFirst = [&]() {
            if (firstLoaded) return;
            firstBytes.resize(static_cast<size_t>(old.stored));
            const auto& firstFile = previous.firstFiles.at(old.firstFile);
            if (!firstFile || !firstFile->Read(old.firstOffset, firstBytes.data(), firstBytes.size()))
                throw std::runtime_error("scan_snapshot_read_failed");
            firstLoaded = true;
        };
        if (needFirst) loadFirst();

        ScanState::Piece piece;
        piece.base = old.base;
        piece.size = old.size;
        piece.stored = old.stored;
        piece.firstFile = old.firstFile;
        piece.firstOffset = old.firstOffset;
        piece.all = false;
        const uint64_t positions = PositionCount(old.base, old.size, old.stored, size, alignment);
        piece.bits.assign(static_cast<size_t>((positions + 63) / 64), 0);
        const uint64_t first = FirstAligned(old.base, alignment);
        const uint8_t* now = buffer.data.data();
        const uint8_t* was = prior.data();
        const uint8_t* initial = needFirst ? firstBytes.data() : was;

        DispatchNumeric(spec.type, spec.isUnsigned, [&](auto tag) {
            using T = decltype(tag);
            const Matcher<T> matcher(spec);
            auto test = [&](uint64_t k) {
                const uint64_t p = first + k * alignment;
                if (!matcher.Match(Load<T>(now + p), Load<T>(was + p), Load<T>(initial + p))) return;
                if (buffer.Touches(old.base + p, sizeof(T))) return;
                piece.bits[static_cast<size_t>(k / 64)] |= 1ull << (k % 64);
                ++piece.count;
            };
            if (old.all) {
                for (uint64_t k = 0; k < positions; ++k) test(k);
            } else {
                for (size_t word = 0; word < old.bits.size(); ++word) {
                    uint64_t bits = old.bits[word];
                    while (bits) {
                        const int bit = CountTrailingZeros(bits);
                        bits &= bits - 1;
                        test(static_cast<uint64_t>(word) * 64 + static_cast<uint64_t>(bit));
                    }
                }
            }
        });
        if (piece.count == 0) return true;

        uint64_t offset = 0;
        if (!files.Get(worker)->Append(now, static_cast<size_t>(old.stored), offset))
            throw std::runtime_error("scan_temp_file_write_failed");
        piece.file = worker;
        piece.offset = offset;

        if (listed.load(std::memory_order_relaxed) <= cap) {
            loadFirst();
            Hits& list = hits[index];
            for (size_t word = 0; word < piece.bits.size(); ++word) {
                uint64_t bits = piece.bits[word];
                while (bits) {
                    const int bit = CountTrailingZeros(bits);
                    bits &= bits - 1;
                    if (listed.fetch_add(1) >= cap) break;
                    const uint64_t p = first + (static_cast<uint64_t>(word) * 64 + static_cast<uint64_t>(bit)) * alignment;
                    list.Add(old.base + p, now + p, size, size);
                    const size_t at = list.firstValues.size();
                    list.firstValues.resize(at + size);
                    std::memcpy(list.firstValues.data() + at, firstBytes.data() + p, size);
                }
            }
        }
        out[index] = std::move(piece);
        return true;
    });
    if (!ok) return nullptr;

    uint64_t total = 0;
    for (const auto& piece : out) total += piece.count;
    if (total <= cap) {
        AppendHits(*state, hits, true);
        state->count = total;
        return state;
    }
    state->snapshot = true;
    state->count = total;
    for (auto& piece : out)
        if (piece.count > 0) state->pieces.push_back(std::move(piece));
    state->files = files.files;
    state->firstFiles = previous.firstFiles;
    return state;
}

} // namespace

// ---------------------------------------------------------------- ScanState

ScanDataType ScanState::HitType(size_t index) const {
    if (type == ScanDataType::AllNumeric && index < hitTypes.size())
        return static_cast<ScanDataType>(hitTypes[index]);
    return type;
}

size_t ScanState::HitSize(size_t index) const {
    const size_t size = ValueScanner::TypeSize(HitType(index));
    return size ? size : valueSize;
}

// ---------------------------------------------------------------- ValueScanner

size_t ValueScanner::TypeSize(ScanDataType type) {
    switch (type) {
        case ScanDataType::Byte: return 1;
        case ScanDataType::Int16: return 2;
        case ScanDataType::Int32:
        case ScanDataType::Float: return 4;
        case ScanDataType::Int64:
        case ScanDataType::Double: return 8;
        default: return 0;
    }
}

bool ValueScanner::IsNumeric(ScanDataType type) {
    return TypeSize(type) != 0;
}

bool ValueScanner::IsInteger(ScanDataType type) {
    return type == ScanDataType::Byte || type == ScanDataType::Int16 ||
           type == ScanDataType::Int32 || type == ScanDataType::Int64;
}

const char* ValueScanner::TypeName(ScanDataType type) {
    switch (type) {
        case ScanDataType::Byte: return "Byte";
        case ScanDataType::Int16: return "2 Bytes";
        case ScanDataType::Int32: return "4 Bytes";
        case ScanDataType::Int64: return "8 Bytes";
        case ScanDataType::Float: return "Float";
        case ScanDataType::Double: return "Double";
        case ScanDataType::String: return "String";
        case ScanDataType::ByteArray: return "Array of bytes";
        case ScanDataType::AllNumeric: return "All";
    }
    return "?";
}

const char* ValueScanner::CompareName(ScanCompare compare) {
    switch (compare) {
        case ScanCompare::Exact: return "Exact value";
        case ScanCompare::Bigger: return "Bigger than...";
        case ScanCompare::Smaller: return "Smaller than...";
        case ScanCompare::Between: return "Value between...";
        case ScanCompare::Unknown: return "Unknown initial value";
        case ScanCompare::Increased: return "Increased value";
        case ScanCompare::IncreasedBy: return "Increased value by...";
        case ScanCompare::Decreased: return "Decreased value";
        case ScanCompare::DecreasedBy: return "Decreased value by...";
        case ScanCompare::Changed: return "Changed value";
        case ScanCompare::Unchanged: return "Unchanged value";
        case ScanCompare::SameAsFirst: return "Same as first scan";
    }
    return "?";
}

bool ValueScanner::AllowedFirst(ScanCompare compare) {
    return compare == ScanCompare::Exact || compare == ScanCompare::Bigger ||
           compare == ScanCompare::Smaller || compare == ScanCompare::Between ||
           compare == ScanCompare::Unknown;
}

bool ValueScanner::AllowedNext(ScanCompare compare, ScanDataType type) {
    if (compare == ScanCompare::Unknown) return false;
    if (type == ScanDataType::String || type == ScanDataType::ByteArray)
        return compare == ScanCompare::Exact || compare == ScanCompare::Changed ||
               compare == ScanCompare::Unchanged || compare == ScanCompare::SameAsFirst;
    return true;
}

bool ValueScanner::NeedsValue(ScanCompare compare) {
    switch (compare) {
        case ScanCompare::Exact:
        case ScanCompare::Bigger:
        case ScanCompare::Smaller:
        case ScanCompare::Between:
        case ScanCompare::IncreasedBy:
        case ScanCompare::DecreasedBy: return true;
        default: return false;
    }
}

std::vector<target::MemoryRegion> ValueScanner::SelectRegions(
        const std::vector<target::MemoryRegion>& regions, const ScanOptions& options) {
    auto accepts = [](ScanTristate rule, bool value) {
        return rule == ScanTristate::Any || (rule == ScanTristate::Yes) == value;
    };
    std::vector<target::MemoryRegion> selected;
    for (auto region : regions) {
        if (!region.readable || region.size == 0) continue;
        if (!accepts(options.writable, region.writable) ||
            !accepts(options.executable, region.executable) ||
            !accepts(options.copyOnWrite, region.copyOnWrite)) continue;
        if ((region.type == target::MemoryRegionType::Private && !options.includePrivate) ||
            (region.type == target::MemoryRegionType::Image && !options.includeImage) ||
            (region.type == target::MemoryRegionType::Mapped && !options.includeMapped)) continue;
        const uint64_t end = region.base + region.size;
        const uint64_t from = std::max(region.base, options.start);
        const uint64_t to = std::min(end, options.stop);
        if (to <= from) continue;
        region.base = from;
        region.size = to - from;
        selected.push_back(region);
    }
    std::sort(selected.begin(), selected.end(),
              [](const auto& left, const auto& right) { return left.base < right.base; });
    return selected;
}

ScanStatePtr ValueScanner::FirstScan(const target::SessionPtr& session, const ScanQuery& query,
                                     const ScanOptions& options, std::string* error,
                                     const std::atomic_bool* cancelled, ScanProgress* progress) {
    if (error) error->clear();
    if (!CheckSession(session, error)) return nullptr;
    if (!session->Capabilities().Has(target::Capability::MemoryScan)) {
        SetError(error, "memory_scan_not_supported");
        return nullptr;
    }
    if (!AllowedFirst(query.compare)) {
        SetError(error, "Only exact, bigger, smaller, between and unknown value work for a first scan");
        return nullptr;
    }

    const Stopwatch stopwatch;
    auto state = std::make_shared<ScanState>();
    state->type = query.type;
    state->firstQuery = query;
    state->options = options;
    state->scanNumber = 1;

    std::vector<NumericSpec> specs;
    PatternSpec pattern;
    const bool isPattern = query.type == ScanDataType::String || query.type == ScanDataType::ByteArray;
    if (isPattern) {
        if (query.compare != ScanCompare::Exact) {
            SetError(error, "Strings and byte arrays are found by exact value");
            return nullptr;
        }
        if (!BuildPattern(query, query.type, query.utf16, pattern, error)) return nullptr;
        state->valueSize = pattern.bytes.size();
        state->alignment = 1;
    } else if (query.type == ScanDataType::AllNumeric) {
        if (query.compare == ScanCompare::Unknown) {
            SetError(error, "Pick one value type for an unknown initial value");
            return nullptr;
        }
        std::string firstError;
        for (const auto type : kNumericTypes) {
            NumericSpec spec;
            std::string typeError;
            if (BuildNumericSpec(type, query, spec, &typeError)) specs.push_back(spec);
            else if (firstError.empty()) firstError = typeError;
        }
        if (specs.empty()) {
            SetError(error, firstError.empty() ? "invalid_scan_value" : firstError);
            return nullptr;
        }
        state->valueSize = 8;
        state->alignment = 1;
    } else if (IsNumeric(query.type)) {
        NumericSpec spec;
        if (!BuildNumericSpec(query.type, query, spec, error)) return nullptr;
        specs.push_back(spec);
        state->valueSize = TypeSize(query.type);
        state->alignment = options.alignment
            ? options.alignment
            : (options.fastScan ? static_cast<uint32_t>(state->valueSize) : 1u);
    } else {
        SetError(error, "unsupported_scan_type");
        return nullptr;
    }

    const auto regions = SelectRegions(session->MemoryRegions(), options);
    if (regions.empty()) {
        SetError(error, "No memory region matches the scan options");
        return nullptr;
    }
    const auto pieces = MakePieces(regions, state->valueSize > 0 ? state->valueSize - 1 : 0);
    uint64_t total = 0;
    for (const auto& piece : pieces) total += piece.stored;
    state->bytesScanned = total;
    if (progress) {
        progress->total.store(total);
        progress->done.store(0);
    }

    ScanStatePtr result = query.compare == ScanCompare::Unknown
        ? UnknownFirstScan(session, state, pieces, error, cancelled, progress)
        : ListFirstScan(session, state, pieces, specs, isPattern ? &pattern : nullptr, error,
                        cancelled, progress);
    if (result) state->milliseconds = stopwatch.Milliseconds();
    return result;
}

ScanStatePtr ValueScanner::NextScan(const target::SessionPtr& session, const ScanStatePtr& previous,
                                    const ScanQuery& query, std::string* error,
                                    const std::atomic_bool* cancelled, ScanProgress* progress) {
    if (error) error->clear();
    if (!previous) {
        SetError(error, "no_previous_scan");
        return nullptr;
    }
    if (!CheckSession(session, error)) return nullptr;
    if (!AllowedNext(query.compare, previous->type)) {
        SetError(error, query.compare == ScanCompare::Unknown
                            ? "Unknown initial value is a first scan"
                            : "This comparison does not apply to strings or byte arrays");
        return nullptr;
    }

    const Stopwatch stopwatch;
    auto state = std::make_shared<ScanState>();
    state->type = previous->type;
    state->valueSize = previous->valueSize;
    state->alignment = previous->alignment;
    state->firstQuery = previous->firstQuery;
    state->options = previous->options;
    state->scanNumber = previous->scanNumber + 1;

    ScanQuery effective = query;
    effective.type = previous->type;
    std::vector<NumericSpec> specs;
    PatternSpec pattern;
    const bool isPattern = previous->type == ScanDataType::String || previous->type == ScanDataType::ByteArray;
    if (isPattern) {
        if (query.compare == ScanCompare::Exact) {
            if (!BuildPattern(effective, previous->type, previous->firstQuery.utf16, pattern, error)) return nullptr;
            if (pattern.bytes.size() != previous->valueSize) {
                SetError(error, "The new value must be as long as the first one");
                return nullptr;
            }
        }
    } else if (previous->type == ScanDataType::AllNumeric) {
        std::string firstError;
        for (const auto type : kNumericTypes) {
            NumericSpec spec;
            std::string typeError;
            if (!BuildNumericSpec(type, effective, spec, &typeError)) {
                if (firstError.empty()) firstError = typeError;
                spec = NumericSpec{};
                spec.type = type;
                spec.invert = effective.invert;
                spec.valid = false;
            }
            specs.push_back(spec);
        }
        if (std::none_of(specs.begin(), specs.end(), [](const auto& spec) { return spec.valid; })) {
            SetError(error, firstError.empty() ? "invalid_scan_value" : firstError);
            return nullptr;
        }
    } else {
        NumericSpec spec;
        if (!BuildNumericSpec(previous->type, effective, spec, error)) return nullptr;
        specs.push_back(spec);
    }

    if (progress) {
        uint64_t total = 0;
        if (previous->snapshot) {
            for (const auto& piece : previous->pieces) total += piece.stored;
        } else {
            total = previous->Size();
        }
        progress->total.store(total);
        progress->done.store(0);
    }

    ScanStatePtr result;
    if (previous->snapshot) {
        state->bytesScanned = 0;
        for (const auto& piece : previous->pieces) state->bytesScanned += piece.stored;
        result = SnapshotNextScan(session, *previous, state, specs.front(), error, cancelled, progress);
    } else {
        state->bytesScanned = previous->Size() * previous->valueSize;
        result = ListNextScan(session, *previous, state, specs, isPattern ? &pattern : nullptr,
                              query.compare, query.invert, error, cancelled, progress);
    }
    if (result) state->milliseconds = stopwatch.Milliseconds();
    return result;
}

ScanStatePtr ValueScanner::RemoveResults(const ScanStatePtr& state, std::vector<size_t> indices) {
    if (!state || state->snapshot) return state;
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    auto copy = std::make_shared<ScanState>();
    copy->type = state->type;
    copy->valueSize = state->valueSize;
    copy->alignment = state->alignment;
    copy->firstQuery = state->firstQuery;
    copy->options = state->options;
    copy->scanNumber = state->scanNumber;
    copy->limitReached = state->limitReached;
    copy->bytesScanned = state->bytesScanned;
    copy->milliseconds = state->milliseconds;
    size_t next = 0;
    const size_t stride = state->valueSize;
    for (size_t i = 0; i < state->Size(); ++i) {
        if (next < indices.size() && indices[next] == i) {
            ++next;
            continue;
        }
        copy->addresses.push_back(state->addresses[i]);
        if (state->type == ScanDataType::AllNumeric) copy->hitTypes.push_back(state->hitTypes[i]);
        copy->values.insert(copy->values.end(), state->Value(i), state->Value(i) + stride);
        copy->firstValues.insert(copy->firstValues.end(), state->FirstValue(i), state->FirstValue(i) + stride);
    }
    copy->count = copy->addresses.size();
    return copy;
}

bool ValueScanner::Encode(const std::string& text, ScanDataType type, bool hex, bool utf16,
                          std::vector<uint8_t>& bytes, std::string* error) {
    bytes.clear();
    if (IsInteger(type)) {
        uint64_t value = 0;
        if (!ParseInteger(text, hex, TypeSize(type), value, error)) return false;
        bytes.resize(TypeSize(type));
        std::memcpy(bytes.data(), &value, bytes.size());
        return true;
    }
    if (type == ScanDataType::Float || type == ScanDataType::Double) {
        double value = 0.0;
        int decimals = 0;
        if (!ParseReal(text, value, decimals, error)) return false;
        if (type == ScanDataType::Float) {
            const float narrow = static_cast<float>(value);
            bytes.resize(sizeof(float));
            std::memcpy(bytes.data(), &narrow, sizeof(float));
        } else {
            bytes.resize(sizeof(double));
            std::memcpy(bytes.data(), &value, sizeof(double));
        }
        return true;
    }
    if (type == ScanDataType::String) {
        if (text.empty()) {
            SetError(error, "Enter a non-empty string");
            return false;
        }
        bytes = utf16 ? Utf8ToUtf16(text) : std::vector<uint8_t>(text.begin(), text.end());
        return true;
    }
    if (type == ScanDataType::ByteArray) {
        PatternSpec spec;
        if (!ParseByteArray(text, spec, error)) return false;
        if (std::any_of(spec.mask.begin(), spec.mask.end(), [](uint8_t mask) { return mask != 0xFF; })) {
            SetError(error, "Wildcards cannot be written");
            return false;
        }
        bytes = spec.bytes;
        return true;
    }
    SetError(error, "unsupported_value_type");
    return false;
}

std::string ValueScanner::Format(const uint8_t* data, size_t size, ScanDataType type, bool hex,
                                 bool unsignedValues, bool utf16) {
    if (!data) return "?";
    const size_t typeSize = TypeSize(type);
    if (typeSize && size < typeSize) return "?";
    char buffer[64] = {};
    if (IsInteger(type)) {
        uint64_t bits = 0;
        std::memcpy(&bits, data, typeSize);
        if (hex) {
            std::snprintf(buffer, sizeof(buffer), "%llX", static_cast<unsigned long long>(bits));
            return buffer;
        }
        if (unsignedValues) {
            std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(bits));
            return buffer;
        }
        const unsigned shift = static_cast<unsigned>(64 - typeSize * 8);
        const auto value = shift ? static_cast<int64_t>(bits << shift) >> shift
                                 : static_cast<int64_t>(bits);
        std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
        return buffer;
    }
    if (type == ScanDataType::Float || type == ScanDataType::Double) {
        const double value = type == ScanDataType::Float ? static_cast<double>(Load<float>(data)) : Load<double>(data);
        if (std::isnan(value)) return "NaN";
        if (std::isinf(value)) return value > 0 ? "Inf" : "-Inf";
        // Shortest text that reads back as the same stored value.
        const int from = type == ScanDataType::Float ? 6 : 15;
        const int to = type == ScanDataType::Float ? 9 : 17;
        for (int precision = from; precision <= to; ++precision) {
            std::snprintf(buffer, sizeof(buffer), "%.*g", precision, value);
            const double back = std::strtod(buffer, nullptr);
            if (type == ScanDataType::Float ? static_cast<float>(back) == static_cast<float>(value) : back == value)
                break;
        }
        return buffer;
    }
    if (type == ScanDataType::String) return utf16 ? PrintableUtf16(data, size) : PrintableUtf8(data, size);
    std::string out;
    for (size_t i = 0; i < size; ++i) {
        std::snprintf(buffer, sizeof(buffer), i ? " %02X" : "%02X", data[i]);
        out += buffer;
    }
    return out;
}

} // namespace cortex::services
