#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cortex::services {

// Builds an array-of-bytes signature that finds an instruction again after
// the program is rebuilt or relocated: instruction by instruction, with the
// bytes that change between builds (relative targets, addresses and 32-bit
// displacements or immediates) wildcarded, until the pattern is unique in
// the searched code.

struct SignatureOptions {
    bool x64 = true;
    bool wildcardDisplacements = true;  // 32-bit memory displacements
    bool wildcardImmediates = true;     // 32/64-bit immediates (addresses, large constants)
    size_t maxLength = 96;
};

struct Signature {
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> mask;  // 0xFF = fixed byte, 0 = wildcard
    size_t instructions = 0;
    size_t matches = 0;         // occurrences in the searched code (1 = unique)
    std::string Text() const;   // "48 8B 05 ?? ?? ?? ?? 48 85 C0"
};

// code: the bytes at the instruction to sign; haystack: the code to search
// (the module's executable sections, say) for uniqueness.
bool GenerateSignature(const uint8_t* code, size_t codeSize, const uint8_t* haystack, size_t haystackSize,
                       const SignatureOptions& options, Signature& signature, std::string* error = nullptr);

} // namespace cortex::services
