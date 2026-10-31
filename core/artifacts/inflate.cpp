#include "inflate.h"

#include <array>

namespace cortex::artifacts {
namespace {

constexpr int kMaxBits = 15;
constexpr int kMaxLengthCodes = 286;
constexpr int kMaxDistanceCodes = 30;
constexpr int kFixedLengthCodes = 288;

// Canonical Huffman table: how many codes of each length, then the symbols
// in code order.
struct Huffman {
    std::array<uint16_t, kMaxBits + 1> count{};
    std::array<uint16_t, kFixedLengthCodes> symbol{};
};

class Decoder {
public:
    Decoder(const uint8_t* input, size_t size, std::vector<uint8_t>& output, size_t maxOutput)
        : in_(input), size_(size), out_(output), max_(maxOutput) {}

    bool Run(std::string& error) {
        bool last = false;
        while (!last) {
            int value = 0;
            if (!Bits(1, value)) return Fail(error);
            last = value != 0;
            int type = 0;
            if (!Bits(2, type)) return Fail(error);
            bool ok = false;
            if (type == 0) ok = Stored();
            else if (type == 1) ok = Fixed();
            else if (type == 2) ok = Dynamic();
            else reason_ = "inflate_invalid_block_type";
            if (!ok) return Fail(error);
        }
        return true;
    }

private:
    bool Fail(std::string& error) {
        error = reason_.empty() ? "inflate_truncated" : reason_;
        return false;
    }

    bool Bits(int need, int& value) {
        long bits = bitBuffer_;
        while (bitCount_ < need) {
            if (pos_ >= size_) {
                reason_ = "inflate_truncated";
                return false;
            }
            bits |= static_cast<long>(in_[pos_++]) << bitCount_;
            bitCount_ += 8;
        }
        bitBuffer_ = static_cast<int>(bits >> need);
        bitCount_ -= need;
        value = static_cast<int>(bits & ((1L << need) - 1));
        return true;
    }

    bool Emit(uint8_t byte) {
        if (out_.size() >= max_) {
            reason_ = "inflate_output_limit";
            return false;
        }
        out_.push_back(byte);
        return true;
    }

    bool Stored() {
        bitBuffer_ = 0;  // discard the rest of the current byte
        bitCount_ = 0;
        if (pos_ + 4 > size_) {
            reason_ = "inflate_truncated";
            return false;
        }
        const unsigned length = in_[pos_] | (in_[pos_ + 1] << 8);
        const unsigned complement = in_[pos_ + 2] | (in_[pos_ + 3] << 8);
        pos_ += 4;
        if (length != (~complement & 0xFFFFu)) {
            reason_ = "inflate_stored_length_mismatch";
            return false;
        }
        if (pos_ + length > size_) {
            reason_ = "inflate_truncated";
            return false;
        }
        for (unsigned i = 0; i < length; ++i)
            if (!Emit(in_[pos_++])) return false;
        return true;
    }

    // Next symbol, reading the code one bit at a time (codes are stored
    // bit-reversed in the stream).
    bool Decode(const Huffman& h, int& symbol) {
        int code = 0, first = 0, index = 0;
        for (int length = 1; length <= kMaxBits; ++length) {
            int bit = 0;
            if (!Bits(1, bit)) return false;
            code |= bit;
            const int count = h.count[length];
            if (code - count < first) {
                symbol = h.symbol[index + (code - first)];
                return true;
            }
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        reason_ = "inflate_invalid_code";
        return false;
    }

    // Builds a table from code lengths. Returns <0 for an over-subscribed
    // set, 0 for complete, >0 for incomplete.
    static int Build(Huffman& h, const uint16_t* lengths, int n) {
        h.count.fill(0);
        for (int s = 0; s < n; ++s) h.count[lengths[s]]++;
        if (h.count[0] == n) return 0;  // no codes: complete, decodes nothing
        int left = 1;
        for (int length = 1; length <= kMaxBits; ++length) {
            left <<= 1;
            left -= h.count[length];
            if (left < 0) return left;
        }
        std::array<uint16_t, kMaxBits + 1> offsets{};
        for (int length = 1; length < kMaxBits; ++length) offsets[length + 1] = offsets[length] + h.count[length];
        for (int s = 0; s < n; ++s)
            if (lengths[s] != 0) h.symbol[offsets[lengths[s]]++] = static_cast<uint16_t>(s);
        return left;
    }

    bool Codes(const Huffman& lengthCodes, const Huffman& distanceCodes) {
        static const uint16_t base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                          31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static const uint8_t extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                          2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static const uint16_t distanceBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,
                                                  33,  49,  65,  97,  129, 193,  257,  385,  513,  769,
                                                  1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static const uint8_t distanceExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                  6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        while (true) {
            int symbol = 0;
            if (!Decode(lengthCodes, symbol)) return false;
            if (symbol < 256) {
                if (!Emit(static_cast<uint8_t>(symbol))) return false;
                continue;
            }
            if (symbol == 256) return true;
            symbol -= 257;
            if (symbol >= 29) {
                reason_ = "inflate_invalid_length_symbol";
                return false;
            }
            int add = 0;
            if (!Bits(extra[symbol], add)) return false;
            const int length = base[symbol] + add;
            int distanceSymbol = 0;
            if (!Decode(distanceCodes, distanceSymbol)) return false;
            if (distanceSymbol >= 30) {
                reason_ = "inflate_invalid_distance_symbol";
                return false;
            }
            if (!Bits(distanceExtra[distanceSymbol], add)) return false;
            const size_t distance = static_cast<size_t>(distanceBase[distanceSymbol] + add);
            if (distance > out_.size()) {
                reason_ = "inflate_distance_too_far";
                return false;
            }
            for (int i = 0; i < length; ++i)
                if (!Emit(out_[out_.size() - distance])) return false;
        }
    }

    bool Fixed() {
        static Huffman lengthCodes, distanceCodes;
        static const bool built = [] {
            uint16_t lengths[kFixedLengthCodes];
            int s = 0;
            for (; s < 144; ++s) lengths[s] = 8;
            for (; s < 256; ++s) lengths[s] = 9;
            for (; s < 280; ++s) lengths[s] = 7;
            for (; s < kFixedLengthCodes; ++s) lengths[s] = 8;
            Build(lengthCodes, lengths, kFixedLengthCodes);
            for (s = 0; s < kMaxDistanceCodes; ++s) lengths[s] = 5;
            Build(distanceCodes, lengths, kMaxDistanceCodes);
            return true;
        }();
        (void)built;
        return Codes(lengthCodes, distanceCodes);
    }

    bool Dynamic() {
        static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        int literalCount = 0, distanceCount = 0, codeCount = 0;
        if (!Bits(5, literalCount) || !Bits(5, distanceCount) || !Bits(4, codeCount)) return false;
        literalCount += 257;
        distanceCount += 1;
        codeCount += 4;
        if (literalCount > kMaxLengthCodes || distanceCount > kMaxDistanceCodes) {
            reason_ = "inflate_bad_counts";
            return false;
        }
        uint16_t lengths[kMaxLengthCodes + kMaxDistanceCodes] = {};
        for (int i = 0; i < codeCount; ++i) {
            int value = 0;
            if (!Bits(3, value)) return false;
            lengths[order[i]] = static_cast<uint16_t>(value);
        }
        Huffman lengthCodes, distanceCodes;
        if (Build(lengthCodes, lengths, 19) != 0) {
            reason_ = "inflate_incomplete_code_lengths";
            return false;
        }
        int index = 0;
        while (index < literalCount + distanceCount) {
            int symbol = 0;
            if (!Decode(lengthCodes, symbol)) return false;
            if (symbol < 16) {
                lengths[index++] = static_cast<uint16_t>(symbol);
                continue;
            }
            uint16_t repeated = 0;
            int repeat = 0;
            if (symbol == 16) {
                if (index == 0) {
                    reason_ = "inflate_repeat_without_previous";
                    return false;
                }
                repeated = lengths[index - 1];
                if (!Bits(2, repeat)) return false;
                repeat += 3;
            } else if (symbol == 17) {
                if (!Bits(3, repeat)) return false;
                repeat += 3;
            } else {
                if (!Bits(7, repeat)) return false;
                repeat += 11;
            }
            if (index + repeat > literalCount + distanceCount) {
                reason_ = "inflate_too_many_lengths";
                return false;
            }
            while (repeat--) lengths[index++] = repeated;
        }
        if (lengths[256] == 0) {
            reason_ = "inflate_no_end_of_block";
            return false;
        }
        // Incomplete codes are allowed only for a single length code.
        const int literalResult = Build(lengthCodes, lengths, literalCount);
        if (literalResult < 0 || (literalResult > 0 && literalCount - lengthCodes.count[0] != 1)) {
            reason_ = "inflate_bad_literal_lengths";
            return false;
        }
        const int distanceResult = Build(distanceCodes, lengths + literalCount, distanceCount);
        if (distanceResult < 0 || (distanceResult > 0 && distanceCount - distanceCodes.count[0] != 1)) {
            reason_ = "inflate_bad_distance_lengths";
            return false;
        }
        return Codes(lengthCodes, distanceCodes);
    }

    const uint8_t* in_;
    size_t size_;
    size_t pos_ = 0;
    int bitBuffer_ = 0;
    int bitCount_ = 0;
    std::vector<uint8_t>& out_;
    size_t max_;
    std::string reason_;
};

} // namespace

bool Inflate(const uint8_t* input, size_t size, std::vector<uint8_t>& output, size_t maxOutput, std::string& error) {
    output.clear();
    error.clear();
    return Decoder(input, size, output, maxOutput).Run(error);
}

uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc) {
    static const auto table = [] {
        std::array<uint32_t, 256> values{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            values[i] = c;
        }
        return values;
    }();
    crc = ~crc;
    for (size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

} // namespace cortex::artifacts
