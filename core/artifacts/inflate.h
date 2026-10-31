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

// CRC-32 (IEEE 802.3), as ZIP stores it. Chain calls with the previous value.
uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc = 0);

} // namespace cortex::artifacts
