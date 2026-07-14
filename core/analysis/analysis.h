#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace analysis {

struct FunctionInfo {
    uintptr_t address;
    size_t size; // distance to the next detected function start (0 for the last one found)
};

// Heuristically finds function start addresses inside `moduleName`'s
// executable regions: linear-decodes every committed executable page,
// treating (a) the first non-padding byte (not 0xCC/0x90) after a ret and
// (b) every direct call's resolved target as a function start. This is the
// same heuristic disassemblers like IDA/Ghidra fall back to on a binary with
// no symbols -- it has false negatives (a function only ever reached
// indirectly, e.g. via a vtable, with no ret+padding before it) and, rarely,
// false positives, but is good enough to turn an unlabeled module into an
// explorable function list. Capped like the other scan features.
std::vector<FunctionInfo> FindFunctions(const std::string& moduleName, bool& outTruncated);

struct CfgBlock {
    uintptr_t start;
    uintptr_t end; // exclusive
};

struct CfgEdge {
    uintptr_t from; // start address of the block the edge originates in
    uintptr_t to;   // 0 for "indirect" edges (target not statically known)
    std::string type; // "jump" | "cond_true" | "cond_false" | "fallthrough" | "indirect"
};

struct CfgResult {
    std::vector<CfgBlock> blocks;
    std::vector<CfgEdge> edges;
    bool truncated = false;
};

// Builds a control-flow graph for the function starting at `address` via
// recursive-descent: each block is decoded until a ret, an unconditional/
// conditional jump, or an undecodable byte is hit. Calls do not end a block
// (they return control to the next instruction). Jump/branch targets and the
// fallthrough after a conditional jump become new block leaders, explored
// breadth-first with a visited set so loops terminate. Indirect jumps
// (register/memory target) produce an edge with `to = 0` and type
// "indirect" since the destination can't be resolved without running the
// code. Sets `ok` to false if `address` isn't readable at all.
CfgResult BuildCFG(uintptr_t address, bool& ok);

struct XRef {
    uintptr_t from;
    std::string type; // "call" | "jump" | "cond_jump" | "read" | "write"
};

// Finds references to `target`: direct call/jump/conditional-jump
// instructions whose resolved destination is `target`, plus instructions
// with a statically-resolvable memory operand at `target` (absolute
// displacement in 32-bit, RIP-relative in 64-bit -- register-relative
// addressing like [ebx+8] can't be resolved without emulating execution, so
// those are missed). Scans `moduleName`'s executable regions if given,
// otherwise every loaded module. Capped like the other scan features.
std::vector<XRef> FindXRefs(uintptr_t target, const std::string& moduleName, bool& outTruncated);

// Finds raw pointer-sized values equal to `target` sitting in *data*, not
// code -- e.g. an entry in a reflection/property table, a vtable slot, a
// global holding the address of a string. FindXRefs only sees references
// encoded as an instruction operand (call/jmp target, RIP-relative/absolute
// memory operand); a pointer baked into a struct array in .data/.rdata is
// invisible to it since nothing "computes" that address at runtime -- it's
// just sitting there as a value. This scans every committed, readable
// (not necessarily executable) page of `moduleName` (or every loaded module
// if empty) for a pointer-width (4 bytes in 32-bit, 8 in 64-bit) value
// matching `target`, at every byte offset (not just aligned ones, since
// packed structs are common). Results use XRef::type = "data_ptr". Capped
// like the other scan features.
std::vector<XRef> FindDataXRefs(uintptr_t target, const std::string& moduleName, bool& outTruncated);

struct VTableEntry {
    uintptr_t slot;    // address of the vtable slot itself (vtableAddress + i*ptrSize)
    uintptr_t address; // function pointer stored at that slot
};

struct VTableDumpResult {
    std::vector<VTableEntry> entries;
    std::string typeName; // best-effort MSVC RTTI class name (demangled), empty if unavailable
    bool truncated = false;
};

// Walks a suspected vtable at `vtableAddress`: reads consecutive pointer-sized
// slots and stops at the first one that doesn't point into any loaded
// module's executable region (compiler-emitted vtables are contiguous
// function-pointer arrays, so the first non-code value marks the end -- the
// same heuristic Cheat Engine/IDA use when there's no size info). Also
// attempts a best-effort MSVC RTTI class-name lookup via the
// CompleteObjectLocator pointer conventionally stored at
// `vtableAddress - ptrSize` (present only if the binary was built with
// `/GR` and the object actually has RTTI info; absent otherwise, in which
// case `typeName` is left empty rather than guessed). `ok` is false only if
// `vtableAddress` itself isn't readable at all.
VTableDumpResult DumpVTable(uintptr_t vtableAddress, int maxEntries, bool& ok);

struct StructuredLine {
    uintptr_t address;
    int depth;         // loop nesting depth, for indentation
    std::string text;  // formatted instruction, or a synthetic structural comment
    bool isAnnotation;  // true for synthetic comments (loop/branch markers), false for real instructions
};

struct StructureResult {
    std::vector<StructuredLine> lines;
    std::vector<uintptr_t> loopHeaders;
    bool truncated = false;
};

// Heuristic "decompiler-lite" pass over BuildCFG's output: detects loops via
// a lightweight back-edge heuristic (a branch whose target address is <= the
// branching block's own start address is treated as jumping back to an
// earlier point in the function -- true for the overwhelming majority of
// compiler-generated while/for loops, without the cost of a full
// dominator-tree analysis), computes a loop-nesting depth per block from the
// resulting [header, backEdgeSource] address ranges, and renders each
// block's instructions indented by that depth with synthetic annotation
// lines marking loop headers and branch destinations. This is NOT real
// decompilation -- there is no data-flow analysis, no variable/type
// recovery, no expression reconstruction; it is disassembly with structural
// hints layered on top to make loops and branches visually obvious. Blocks
// are emitted in address order. Sets `ok` to false if `address` isn't
// readable (same as BuildCFG).
StructureResult StructureCFG(uintptr_t address, bool& ok);

struct PeSectionInfo {
    std::string name;
    uintptr_t virtualAddress; // absolute (module base + section RVA)
    uint32_t virtualSize;
    uint32_t rawSize;
    uint32_t characteristics; // IMAGE_SCN_* flags
};

struct PeImportEntry {
    std::string moduleName;
    std::string functionName; // empty if imported by ordinal
    uint16_t ordinal;         // valid only when functionName is empty
    uintptr_t iatSlot;        // absolute address of the IAT slot itself
};

struct PeExportEntry {
    std::string name; // empty if exported by ordinal only
    uint16_t ordinal;
    uintptr_t address; // absolute
};

struct PeHeaderInfo {
    uintptr_t base;
    uint32_t sizeOfImage;
    uintptr_t entryPoint; // absolute (base + AddressOfEntryPoint)
    uint16_t machine;
    uint16_t subsystem;
    uint32_t timestamp;
    uint32_t characteristics;
    bool isDll;
    std::vector<PeSectionInfo> sections;
    std::vector<PeImportEntry> imports;
    std::vector<PeExportEntry> exports;
    bool importsTruncated = false;
    bool exportsTruncated = false;
};

// Reads and parses the PE headers of `moduleName` as currently mapped in this
// process (not the on-disk file -- see ScanPatches for that): DOS header
// (checks e_magic), NT headers (checks Signature), file/optional header
// fields, section table, import table (module + function/ordinal + IAT slot
// address, walked via the Import Directory's INT/IAT thunk arrays), and
// export table (name/ordinal + resolved address, walked via the Export
// Directory's name/ordinal/address arrays). Since the module is already
// mapped in-process, RVAs are resolved by simple base+rva reads rather than
// needing section-alignment math for file-offset conversion. Sets `ok` to
// false if the module isn't found or its DOS/NT signatures don't check out.
PeHeaderInfo DissectPeHeaders(const std::string& moduleName, bool& ok);

// Diffs the in-memory bytes of `moduleName`'s executable sections against the
// bytes on disk (read from the module's own file, resolved via
// GetModuleFileNameA on its base address) to reveal runtime patches -- a
// trainer/anticheat/mod that hooked or patched code after load, independent
// of anything this tool itself applied (which are tracked separately by
// patch::List()). Only PE sections whose characteristics include
// IMAGE_SCN_MEM_EXECUTE are compared (data sections routinely legitimately
// differ due to relocations/globals); a differing byte range shorter than
// `minRunLength` apart from the previous one is merged into it, to avoid
// reporting every isolated relocated pointer as its own patch. Capped at
// `kMaxPatchRuns` results. Sets `ok` to false if the module isn't found, its
// on-disk file can't be opened, or its PE signatures don't check out.
struct PatchRun {
    uintptr_t address; // absolute, in-memory
    std::vector<uint8_t> diskBytes;
    std::vector<uint8_t> memoryBytes;
};

struct ScanPatchesResult {
    std::vector<PatchRun> runs;
    bool truncated = false;
};

ScanPatchesResult ScanPatches(const std::string& moduleName, size_t minRunLength, bool& ok);

} // namespace analysis
