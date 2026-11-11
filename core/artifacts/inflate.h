#pragma once

// Raw DEFLATE (RFC 1951) decoding and CRC-32, for ZIP containers (JAR, APK,
// NuGet, wheels). The input is untrusted: every read is bounds-checked, the
// output is capped, and invalid streams fail with a reason instead of
// producing partial data. Written after zlib's reference decoder "puff".

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cortex::artifacts {

// Decodes a raw DEFLATE stream (no zlib or gzip header). Fails with
// `error` set if the stream is invalid, truncated, or would expand past
// `maxOutput` bytes ("inflate_output_limit").
bool Inflate(const uint8_t* input, size_t size, std::vector<uint8_t>& output, size_t maxOutput, std::string& error);

// Decode one raw stream and report its byte boundary (including the last
// partially-used byte). Used for concatenated gzip members. Clears output
// and consumed on failure; trailing bytes are left to the caller.
bool InflatePrefix(const uint8_t* input, size_t size, std::vector<uint8_t>& output,
                   size_t maxOutput, size_t& consumed, std::string& error);

// Raw DEFLATE with a read-only preset history (last 32 KiB only), used by
// successive CAB/MSZIP blocks. maxOutput excludes the dictionary. Returns
// only newly expanded bytes and clears output on failure.
bool InflateWithDictionary(const uint8_t* input, size_t size, const uint8_t* dictionary,
                           size_t dictionarySize, std::vector<uint8_t>& output,
                           size_t maxOutput, std::string& error);

// CRC-32 (IEEE 802.3), as ZIP stores it. Chain calls with the previous value.
uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc = 0);

} // namespace cortex::artifacts
