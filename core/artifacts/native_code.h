#pragma once

// Offline static analysis of native code in a PE file (step 4d): a
// function catalog where every entry says where it comes from, a
// cross-reference index (code, data and address constants, strings), and
// control-flow graphs with optional coverage from a trace. Nothing is
// loaded or executed; addresses are RVAs. x86 and x64 images are both
// decoded (Zydis), whatever the host's bitness.
//
// Provenance of a function start, strongest first:
//   entry       the PE entry point
//   export      an exported symbol (not a forwarder)
//   pdata       an x64 exception-directory RUNTIME_FUNCTION (compiler
//               metadata; chained fragments are not starts)
//   tls         a TLS callback
//   call_target the target of a direct call found by a linear sweep
//   after_ret   the first non-padding byte after a ret (heuristic)
// A linear sweep can decode data as code; entries found only by
// after_ret are the weakest and are labelled as such.

#include "artifact.h"
#include "pe_provider.h"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace cortex::artifacts::native {

struct Function {
    uint32_t rva = 0;
    uint32_t size = 0;            // from pdata when known, else up to the next start
    bool sizeFromPdata = false;
    std::set<std::string> sources;
    std::string exportName;
};

struct Xref {
    uint32_t from = 0;            // instruction RVA
    uint32_t to = 0;              // referenced RVA
    std::string type;             // call | jump | cond_jump | read | write | address
};

struct CodeIndex {
    bool x64 = false;
    uint64_t imageBase = 0;
    std::vector<Function> functions;           // sorted by RVA
    std::vector<Xref> xrefs;                   // sorted by target
    std::map<uint32_t, std::string> strings;   // RVA -> text (ASCII or UTF-16LE, cut to 256)
    std::map<uint32_t, bool> stringIsWide;
    uint64_t decodedBytes = 0;
    uint64_t instructions = 0;
    uint32_t pdataEntries = 0, pdataChained = 0;
    bool truncated = false;
    std::vector<std::string> warnings;

    // The function containing `rva` (by pdata range or next start), or null.
    const Function* Containing(uint32_t rva) const;
    const Function* At(uint32_t rva) const;
};

// Builds the index; false with `error` if the PE has no decodable code.
bool BuildIndex(const ByteSource& source, const PeFile& pe, CodeIndex& index, std::string& error);

// Index of a file, cached by SHA-256 (a few recent files) so repeated
// questions about one binary are not re-analysed.
std::shared_ptr<const CodeIndex> IndexFor(const ByteSource& source, const PeFile& pe, const std::string& sha256,
                                          std::string& error);

// An RVA from "0x1234", "rva:0x1234", "va:0x140001234", "sub_1234", an
// export name or a name in `names` (annotations). False if none matches.
bool ResolveLocation(const CodeIndex& index, const std::string& query, const std::map<std::string, uint32_t>& names,
                     uint32_t& rva);

// Name shown for a function: annotation, export, or sub_<RVA>.
std::string FunctionName(const Function& function, const std::map<uint32_t, std::string>& annotations);

// A position-independent fingerprint of the code at `rva`: SHA-256 of the
// first whole instructions (up to 64 bytes, or the function's size) with
// relative branch targets, RIP-relative displacements and in-image absolute
// addresses zeroed, so the same function moved by a rebuild still matches.
// {"sha256", "length", "function_size"} or null if `rva` is not code.
json Fingerprint(const ByteSource& source, const PeFile& pe, const CodeIndex& index, uint32_t rva);
// Function starts whose fingerprint equals `fingerprint`.
std::vector<uint32_t> FindFingerprint(const ByteSource& source, const PeFile& pe, const CodeIndex& index,
                                      const json& fingerprint);

// JSON views. `annotations` maps RVA -> name given by a person or agent.
json FunctionsJson(const CodeIndex& index, const std::string& filter, size_t maxEntries,
                   const std::map<uint32_t, std::string>& annotations);
json XrefsJson(const CodeIndex& index, uint32_t target, size_t maxEntries,
               const std::map<uint32_t, std::string>& annotations);
json StringsJson(const CodeIndex& index, const std::string& filter, size_t maxEntries);
// The CFG of the function at `rva` with each block's instructions and,
// when `covered` is not empty, whether a traced instruction fell in it.
json CfgJson(const ByteSource& source, const PeFile& pe, const CodeIndex& index, uint32_t rva,
             const std::set<uint32_t>& covered, size_t maxBlocks, const std::map<uint32_t, std::string>& annotations);

} // namespace cortex::artifacts::native
