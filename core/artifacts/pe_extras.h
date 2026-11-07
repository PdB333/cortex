#pragma once

// What a PE says about itself beyond its code layout: who signed it, how it
// was built and protected, what it embeds. All of it is read from the file;
// nothing is executed, unpacked or trusted.
//
// - Authenticode: the PKCS#7 signature's signer and certificates, and
//   whether the file's current digest matches the signed one. The chain is
//   not checked against trusted roots.
// - Import hash (imphash, as pefile computes it), delay-load imports.
// - Rich header: the build tools' product ids, builds and counts.
// - Resources: the tree, version information, manifest.
// - Load configuration and a hardening summary (ASLR, DEP, CFG, SafeSEH,
//   CET, /GS).
// - Section entropy and packer detection by evidence. Detection only:
//   Cortex never unpacks.

#include "pe_provider.h"

#include <string>
#include <vector>

namespace cortex::artifacts {

// MD5 (RFC 1321), hex, for the import hash.
std::string Md5Hex(const std::string& data);

// pefile's get_imphash() over the "imports" list PeImports returns. Imports
// by ordinal are written "ord<N>" (pefile names a few well-known ordinals of
// ws2_32, wsock32 and oleaut32; those differ).
std::string ImportHash(const json& imports);

// Rich header between the DOS stub and the PE header, or null.
json RichHeader(const ByteSource& source);
// The same on raw bytes (the start of a file), for tests.
json RichHeaderFromBytes(const std::vector<uint8_t>& head);

json DelayImports(const ByteSource& source, const PeFile& pe, const InspectOptions& options);
json Resources(const ByteSource& source, const PeFile& pe, const InspectOptions& options);
json LoadConfig(const ByteSource& source, const PeFile& pe);
json Hardening(const ByteSource& source, const PeFile& pe, const json& loadConfig);
double Entropy(const ByteSource& source, uint64_t offset, uint64_t size);
json Packers(const ByteSource& source, const PeFile& pe, const json& imports, const json& sections);
json Authenticode(const ByteSource& source, const PeFile& pe);

}  // namespace cortex::artifacts
