#include "analysis.h"
#include "../memory/memory.h"
#include "../process/modules.h"

#include <windows.h>
#include <Zydis/Zydis.h>
#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <sstream>

namespace analysis {

namespace {

constexpr size_t kMaxFunctions = 20000;
constexpr size_t kMaxScanBytesPerRegion = 64 * 1024 * 1024;
constexpr size_t kMaxCfgBlocks = 500;
constexpr size_t kMaxCfgInstructionsPerBlock = 300;
constexpr size_t kMaxXRefs = 2000;

struct ExecRegion {
    uintptr_t start;
    size_t size;
};

ZydisDecoder MakeDecoder() {
    ZydisDecoder decoder;
#ifdef _WIN64
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
#else
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32);
#endif
    return decoder;
}

// Enumerates committed, executable pages within [base, base+size).
std::vector<ExecRegion> ExecutableRegionsIn(uintptr_t base, size_t size) {
    std::vector<ExecRegion> out;
    uintptr_t addr = base;
    uintptr_t end = base + size;
    MEMORY_BASIC_INFORMATION mbi;
    while (addr < end && VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == sizeof(mbi)) {
        uintptr_t regionStart = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        uintptr_t regionEnd = regionStart + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
            uintptr_t clippedEnd = (std::min)(regionEnd, end);
            if (clippedEnd > regionStart) out.push_back({regionStart, clippedEnd - regionStart});
        }
        if (regionEnd <= addr) break; // guard against a zero-size region wedging the loop
        addr = regionEnd;
    }
    return out;
}

std::string Hex(uintptr_t a) {
    std::ostringstream s;
    s << "0x" << std::hex << a;
    return s.str();
}

bool FindModule(const std::string& name, uintptr_t& outBase, size_t& outSize) {
    for (const auto& m : process::ListModules()) {
        if (m.name == name) {
            outBase = m.base;
            outSize = m.size;
            return true;
        }
    }
    return false;
}

std::vector<ExecRegion> AllExecutableRegions() {
    std::vector<ExecRegion> out;
    for (const auto& m : process::ListModules()) {
        auto regions = ExecutableRegionsIn(m.base, m.size);
        out.insert(out.end(), regions.begin(), regions.end());
    }
    return out;
}

// Same as ExecutableRegionsIn but for any committed, non-guard, accessible
// page -- data xref scanning needs .data/.rdata, not just executable code.
std::vector<ExecRegion> ReadableRegionsIn(uintptr_t base, size_t size) {
    std::vector<ExecRegion> out;
    uintptr_t addr = base;
    uintptr_t end = base + size;
    MEMORY_BASIC_INFORMATION mbi;
    while (addr < end && VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == sizeof(mbi)) {
        uintptr_t regionStart = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        uintptr_t regionEnd = regionStart + mbi.RegionSize;
        bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_NOACCESS) && !(mbi.Protect & PAGE_GUARD);
        if (readable) {
            uintptr_t clippedEnd = (std::min)(regionEnd, end);
            if (clippedEnd > regionStart) out.push_back({regionStart, clippedEnd - regionStart});
        }
        if (regionEnd <= addr) break;
        addr = regionEnd;
    }
    return out;
}

std::vector<ExecRegion> AllReadableRegions() {
    std::vector<ExecRegion> out;
    for (const auto& m : process::ListModules()) {
        auto regions = ReadableRegionsIn(m.base, m.size);
        out.insert(out.end(), regions.begin(), regions.end());
    }
    return out;
}

// Best-effort MSVC RTTI class-name lookup. `vtableAddress - ptrSize`
// conventionally holds a pointer to a CompleteObjectLocator; its layout
// differs between 32-bit (absolute pointers, signature == 0) and 64-bit
// (RVAs relative to the module base, signature == 1 / COL_SIG_REV1). Returns
// "" on any layout mismatch or unreadable memory rather than guessing.
std::string DemangleRttiTypeName(const std::string& mangled) {
    // Class-name descriptors look like ".?AVClassName@@" (class) or
    // ".?AUStructName@@" (struct), with namespaces stored innermost-first
    // and separated by '@', e.g. ".?AVCFoo@NBar@@" == NBar::CFoo. This is a
    // narrow best-effort unmangler for that one shape, not a general demangler.
    if (mangled.size() < 5 || mangled[0] != '.' || mangled[1] != '?' || mangled[2] != 'A') return "";
    if (mangled[3] != 'V' && mangled[3] != 'U') return "";
    std::string rest = mangled.substr(4);
    if (rest.size() >= 2 && rest.substr(rest.size() - 2) == "@@") rest.resize(rest.size() - 2);

    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t i = 0; i <= rest.size(); ++i) {
        if (i == rest.size() || rest[i] == '@') {
            if (i > start) parts.push_back(rest.substr(start, i - start));
            start = i + 1;
        }
    }
    if (parts.empty()) return "";
    std::string out = parts.back();
    for (size_t i = parts.size() - 1; i-- > 0;) out += "::" + parts[i];
    return out;
}

std::string TryReadRttiName(uintptr_t vtableAddress) {
    constexpr size_t kPtrSize = sizeof(uintptr_t);

    std::vector<uint8_t> ptrBuf;
    if (!memory::ReadBytes(vtableAddress - kPtrSize, kPtrSize, ptrBuf)) return "";
    uintptr_t colPtr;
    std::memcpy(&colPtr, ptrBuf.data(), kPtrSize);
    if (colPtr == 0) return "";

    uintptr_t typeDescAddr = 0;
#ifdef _WIN64
    uintptr_t modBase = 0, modSize = 0;
    for (const auto& m : process::ListModules()) {
        if (vtableAddress >= m.base && vtableAddress < m.base + m.size) {
            modBase = m.base;
            modSize = m.size;
            break;
        }
    }
    if (modBase == 0) return "";

    std::vector<uint8_t> colBuf;
    if (!memory::ReadBytes(colPtr, 16, colBuf)) return "";
    uint32_t signature, typeDescRva;
    std::memcpy(&signature, colBuf.data(), 4);
    std::memcpy(&typeDescRva, colBuf.data() + 12, 4);
    if (signature != 1) return "";
    typeDescAddr = modBase + typeDescRva;
#else
    std::vector<uint8_t> colBuf;
    if (!memory::ReadBytes(colPtr, 16, colBuf)) return "";
    uint32_t signature, typeDescPtr;
    std::memcpy(&signature, colBuf.data(), 4);
    std::memcpy(&typeDescPtr, colBuf.data() + 12, 4);
    if (signature != 0) return "";
    typeDescAddr = static_cast<uintptr_t>(typeDescPtr);
#endif

    auto mangled = memory::ReadString(typeDescAddr + kPtrSize * 2, 256);
    if (!mangled.has_value() || mangled->empty()) return "";
    return DemangleRttiTypeName(*mangled);
}

// Resolves operand[0] of a call/jmp/jcc to an absolute address, if it's a
// direct (immediate) branch rather than an indirect (register/memory) one.
bool ResolveBranchTarget(const ZydisDecodedInstruction& instr, const ZydisDecodedOperand* operands,
                          uintptr_t curAddr, uintptr_t& outTarget) {
    if (instr.operand_count_visible < 1 || operands[0].type != ZYDIS_OPERAND_TYPE_IMMEDIATE) return false;
    ZyanU64 target = 0;
    if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&instr, &operands[0], curAddr, &target))) return false;
    outTarget = static_cast<uintptr_t>(target);
    return true;
}

} // namespace

std::vector<FunctionInfo> FindFunctions(const std::string& moduleName, bool& outTruncated) {
    outTruncated = false;
    std::vector<FunctionInfo> result;
    if (moduleName.empty()) return result;

    uintptr_t modBase = 0;
    size_t modSize = 0;
    if (!FindModule(moduleName, modBase, modSize)) return result;

    ZydisDecoder decoder = MakeDecoder();
    std::set<uintptr_t> starts;

    for (const auto& region : ExecutableRegionsIn(modBase, modSize)) {
        std::vector<uint8_t> buf;
        size_t readSize = (std::min)(region.size, kMaxScanBytesPerRegion);
        if (!memory::ReadBytes(region.start, readSize, buf)) continue;

        size_t offset = 0;
        uintptr_t curAddr = region.start;
        bool afterRet = false;
        while (offset < buf.size()) {
            uint8_t b = buf[offset];
            if (afterRet) {
                if (b == 0xCC || b == 0x90) {
                    // still inside the padding run, keep waiting for real code
                } else {
                    starts.insert(curAddr);
                    afterRet = false;
                }
            }

            ZydisDecodedInstruction instr;
            ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
            ZyanStatus status = ZydisDecoderDecodeFull(&decoder, buf.data() + offset, buf.size() - offset,
                                                        &instr, operands);
            if (!ZYAN_SUCCESS(status)) {
                afterRet = false;
                offset += 1;
                curAddr += 1;
                continue;
            }

            if (instr.meta.category == ZYDIS_CATEGORY_CALL) {
                uintptr_t target = 0;
                if (ResolveBranchTarget(instr, operands, curAddr, target) &&
                    target >= modBase && target < modBase + modSize) {
                    starts.insert(target);
                }
            }
            afterRet = (instr.meta.category == ZYDIS_CATEGORY_RET);

            offset += instr.length;
            curAddr += instr.length;

            if (starts.size() >= kMaxFunctions) {
                outTruncated = true;
                break;
            }
        }
        if (outTruncated) break;
    }

    result.reserve(starts.size());
    std::vector<uintptr_t> sorted(starts.begin(), starts.end());
    for (size_t i = 0; i < sorted.size(); ++i) {
        FunctionInfo fi;
        fi.address = sorted[i];
        fi.size = (i + 1 < sorted.size()) ? (sorted[i + 1] - sorted[i]) : 0;
        result.push_back(fi);
    }
    return result;
}

CfgResult BuildCFG(uintptr_t address, bool& ok) {
    ok = false;
    CfgResult result;
    if (address == 0) return result;

    MEMORY_BASIC_INFORMATION probe;
    if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &probe, sizeof(probe)) != sizeof(probe)) return result;
    if (probe.State != MEM_COMMIT || (probe.Protect & PAGE_NOACCESS) || (probe.Protect & PAGE_GUARD)) return result;

    ZydisDecoder decoder = MakeDecoder();
    std::set<uintptr_t> leaders{address};
    std::set<uintptr_t> visited;
    std::vector<uintptr_t> worklist{address};

    while (!worklist.empty()) {
        if (result.blocks.size() >= kMaxCfgBlocks) {
            result.truncated = true;
            break;
        }
        uintptr_t blockStart = worklist.back();
        worklist.pop_back();
        if (visited.count(blockStart)) continue;
        visited.insert(blockStart);

        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(reinterpret_cast<LPCVOID>(blockStart), &mbi, sizeof(mbi)) != sizeof(mbi)) continue;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_NOACCESS) || (mbi.Protect & PAGE_GUARD)) continue;
        uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (blockStart >= regionEnd) continue;

        size_t wantSize = (std::min)(static_cast<size_t>(kMaxCfgInstructionsPerBlock) * 16,
                                      static_cast<size_t>(regionEnd - blockStart));
        std::vector<uint8_t> buf;
        if (!memory::ReadBytes(blockStart, wantSize, buf)) continue;

        size_t offset = 0;
        uintptr_t curAddr = blockStart;
        uintptr_t blockEnd = blockStart;
        size_t instrCount = 0;

        while (offset < buf.size() && instrCount < kMaxCfgInstructionsPerBlock) {
            if (curAddr != blockStart && leaders.count(curAddr)) {
                result.edges.push_back({blockStart, curAddr, "fallthrough"});
                if (!visited.count(curAddr)) worklist.push_back(curAddr);
                break;
            }

            ZydisDecodedInstruction instr;
            ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
            ZyanStatus status = ZydisDecoderDecodeFull(&decoder, buf.data() + offset, buf.size() - offset,
                                                        &instr, operands);
            if (!ZYAN_SUCCESS(status)) break; // undecodable -- end block here, no successors

            blockEnd = curAddr + instr.length;
            instrCount++;
            auto category = instr.meta.category;

            if (category == ZYDIS_CATEGORY_RET) {
                break;
            }
            if (category == ZYDIS_CATEGORY_COND_BR) {
                uintptr_t fallAddr = curAddr + instr.length;
                leaders.insert(fallAddr);
                if (!visited.count(fallAddr)) worklist.push_back(fallAddr);
                result.edges.push_back({blockStart, fallAddr, "cond_false"});

                uintptr_t target = 0;
                if (ResolveBranchTarget(instr, operands, curAddr, target)) {
                    leaders.insert(target);
                    if (!visited.count(target)) worklist.push_back(target);
                    result.edges.push_back({blockStart, target, "cond_true"});
                }
                break;
            }
            if (category == ZYDIS_CATEGORY_UNCOND_BR) {
                uintptr_t target = 0;
                if (ResolveBranchTarget(instr, operands, curAddr, target)) {
                    leaders.insert(target);
                    if (!visited.count(target)) worklist.push_back(target);
                    result.edges.push_back({blockStart, target, "jump"});
                } else {
                    result.edges.push_back({blockStart, 0, "indirect"});
                }
                break;
            }

            offset += instr.length;
            curAddr += instr.length;
        }

        result.blocks.push_back({blockStart, blockEnd});
    }

    ok = true;
    return result;
}

std::vector<XRef> FindXRefs(uintptr_t target, const std::string& moduleName, bool& outTruncated) {
    outTruncated = false;
    std::vector<XRef> result;
    if (target == 0) return result;

    std::vector<ExecRegion> regions;
    if (!moduleName.empty()) {
        uintptr_t modBase = 0;
        size_t modSize = 0;
        if (!FindModule(moduleName, modBase, modSize)) return result;
        regions = ExecutableRegionsIn(modBase, modSize);
    } else {
        regions = AllExecutableRegions();
    }

    ZydisDecoder decoder = MakeDecoder();

    for (const auto& region : regions) {
        std::vector<uint8_t> buf;
        size_t readSize = (std::min)(region.size, kMaxScanBytesPerRegion);
        if (!memory::ReadBytes(region.start, readSize, buf)) continue;

        size_t offset = 0;
        uintptr_t curAddr = region.start;
        while (offset < buf.size()) {
            ZydisDecodedInstruction instr;
            ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
            ZyanStatus status = ZydisDecoderDecodeFull(&decoder, buf.data() + offset, buf.size() - offset,
                                                        &instr, operands);
            if (!ZYAN_SUCCESS(status)) {
                offset += 1;
                curAddr += 1;
                continue;
            }

            const char* codeType = nullptr;
            switch (instr.meta.category) {
                case ZYDIS_CATEGORY_CALL: codeType = "call"; break;
                case ZYDIS_CATEGORY_UNCOND_BR: codeType = "jump"; break;
                case ZYDIS_CATEGORY_COND_BR: codeType = "cond_jump"; break;
                default: break;
            }
            if (codeType) {
                uintptr_t branchTarget = 0;
                if (ResolveBranchTarget(instr, operands, curAddr, branchTarget) && branchTarget == target) {
                    result.push_back({curAddr, codeType});
                }
            }

            for (int i = 0; i < instr.operand_count_visible; ++i) {
                if (operands[i].type != ZYDIS_OPERAND_TYPE_MEMORY) continue;
#ifdef _WIN64
                bool resolvable = operands[i].mem.base == ZYDIS_REGISTER_RIP;
#else
                bool resolvable =
                    operands[i].mem.base == ZYDIS_REGISTER_NONE && operands[i].mem.index == ZYDIS_REGISTER_NONE;
#endif
                if (!resolvable) continue;
                ZyanU64 memTarget = 0;
                if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&instr, &operands[i], curAddr, &memTarget))) continue;
                if (static_cast<uintptr_t>(memTarget) != target) continue;
                bool isWrite = (operands[i].actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) != 0;
                result.push_back({curAddr, isWrite ? "write" : "read"});
            }

            if (result.size() >= kMaxXRefs) {
                outTruncated = true;
                break;
            }

            offset += instr.length;
            curAddr += instr.length;
        }
        if (outTruncated) break;
    }

    return result;
}

std::vector<XRef> FindDataXRefs(uintptr_t target, const std::string& moduleName, bool& outTruncated) {
    outTruncated = false;
    std::vector<XRef> result;
    if (target == 0) return result;

    std::vector<ExecRegion> regions;
    if (!moduleName.empty()) {
        uintptr_t modBase = 0;
        size_t modSize = 0;
        if (!FindModule(moduleName, modBase, modSize)) return result;
        regions = ReadableRegionsIn(modBase, modSize);
    } else {
        regions = AllReadableRegions();
    }

    constexpr size_t kPtrSize = sizeof(uintptr_t);

    for (const auto& region : regions) {
        std::vector<uint8_t> buf;
        size_t readSize = (std::min)(region.size, kMaxScanBytesPerRegion);
        if (!memory::ReadBytes(region.start, readSize, buf)) continue;
        if (buf.size() < kPtrSize) continue;

        for (size_t offset = 0; offset + kPtrSize <= buf.size(); ++offset) {
            uintptr_t value;
            std::memcpy(&value, buf.data() + offset, kPtrSize);
            if (value == target) {
                result.push_back({region.start + offset, "data_ptr"});
                if (result.size() >= kMaxXRefs) {
                    outTruncated = true;
                    break;
                }
            }
        }
        if (outTruncated) break;
    }

    return result;
}

VTableDumpResult DumpVTable(uintptr_t vtableAddress, int maxEntries, bool& ok) {
    ok = false;
    VTableDumpResult result;
    if (vtableAddress == 0) return result;

    constexpr size_t kPtrSize = sizeof(uintptr_t);
    int cap = maxEntries > 0 ? maxEntries : 256;

    auto execRegions = AllExecutableRegions();
    auto pointsIntoCode = [&](uintptr_t addr) {
        for (const auto& r : execRegions) {
            if (addr >= r.start && addr < r.start + r.size) return true;
        }
        return false;
    };

    for (int i = 0; i < cap; ++i) {
        uintptr_t slot = vtableAddress + static_cast<uintptr_t>(i) * kPtrSize;
        std::vector<uint8_t> buf;
        if (!memory::ReadBytes(slot, kPtrSize, buf)) break;
        ok = true; // the vtable's own memory was readable at least this far
        uintptr_t fn;
        std::memcpy(&fn, buf.data(), kPtrSize);
        if (!pointsIntoCode(fn)) break;
        result.entries.push_back({slot, fn});
    }

    if (!ok) {
        std::vector<uint8_t> probe;
        ok = memory::ReadBytes(vtableAddress, kPtrSize, probe);
    }

    if (static_cast<int>(result.entries.size()) >= cap) result.truncated = true;
    result.typeName = TryReadRttiName(vtableAddress);
    return result;
}

StructureResult StructureCFG(uintptr_t address, bool& ok) {
    StructureResult result;
    CfgResult cfg = BuildCFG(address, ok);
    if (!ok) return result;
    result.truncated = cfg.truncated;

    struct LoopRange {
        uintptr_t header;
        uintptr_t maxSource;
    };
    std::vector<LoopRange> loopRanges;
    std::set<uintptr_t> loopHeaderSet;
    for (const auto& e : cfg.edges) {
        if (e.to == 0 || e.to > e.from) continue;
        loopHeaderSet.insert(e.to);
        bool merged = false;
        for (auto& lr : loopRanges) {
            if (lr.header == e.to) {
                lr.maxSource = (std::max)(lr.maxSource, e.from);
                merged = true;
                break;
            }
        }
        if (!merged) loopRanges.push_back({e.to, e.from});
    }
    result.loopHeaders.assign(loopHeaderSet.begin(), loopHeaderSet.end());

    std::vector<CfgBlock> blocks = cfg.blocks;
    std::sort(blocks.begin(), blocks.end(), [](const CfgBlock& a, const CfgBlock& b) { return a.start < b.start; });

    std::map<uintptr_t, std::vector<CfgEdge>> edgesByBlock;
    for (const auto& e : cfg.edges) edgesByBlock[e.from].push_back(e);

    ZydisDecoder decoder = MakeDecoder();
    ZydisFormatter formatter;
    ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);

    for (const auto& block : blocks) {
        int depth = 0;
        for (const auto& lr : loopRanges) {
            if (block.start >= lr.header && block.start <= lr.maxSource) depth++;
        }

        if (loopHeaderSet.count(block.start)) {
            result.lines.push_back({block.start, depth, "// loop header", true});
        }

        size_t size = block.end > block.start ? block.end - block.start : 0;
        std::vector<uint8_t> buf;
        if (size > 0 && memory::ReadBytes(block.start, size, buf)) {
            size_t offset = 0;
            uintptr_t curAddr = block.start;
            while (offset < buf.size()) {
                ZydisDecodedInstruction instr;
                ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
                ZyanStatus status = ZydisDecoderDecodeFull(&decoder, buf.data() + offset, buf.size() - offset,
                                                            &instr, operands);
                if (!ZYAN_SUCCESS(status)) break;

                char textBuf[256];
                ZydisFormatterFormatInstruction(&formatter, &instr, operands, instr.operand_count_visible, textBuf,
                                                 sizeof(textBuf), curAddr, ZYAN_NULL);
                result.lines.push_back({curAddr, depth, textBuf, false});

                offset += instr.length;
                curAddr += instr.length;
            }
        }

        auto it = edgesByBlock.find(block.start);
        if (it != edgesByBlock.end()) {
            for (const auto& e : it->second) {
                if (e.type == "cond_true") {
                    result.lines.push_back({block.end, depth, "// if (branch taken) goto " + Hex(e.to), true});
                } else if (e.type == "cond_false") {
                    result.lines.push_back({block.end, depth, "// else goto " + Hex(e.to), true});
                } else if (e.type == "jump") {
                    result.lines.push_back({block.end, depth, "// goto " + Hex(e.to), true});
                } else if (e.type == "indirect") {
                    result.lines.push_back({block.end, depth, "// goto <indirect>", true});
                }
            }
        }
    }

    return result;
}

namespace {

constexpr size_t kMaxImports = 5000;
constexpr size_t kMaxExports = 5000;
constexpr size_t kMaxPatchRuns = 500;
constexpr size_t kMaxPatchCompareBytes = 64 * 1024 * 1024;

template <typename T>
bool ReadStruct(uintptr_t address, T& out) {
    std::vector<uint8_t> buf;
    if (!memory::ReadBytes(address, sizeof(T), buf) || buf.size() != sizeof(T)) return false;
    memcpy(&out, buf.data(), sizeof(T));
    return true;
}

std::string ReadCString(uintptr_t address, size_t maxLen = 256) {
    auto s = memory::ReadString(address, maxLen);
    return s.has_value() ? *s : std::string();
}

} // namespace

PeHeaderInfo DissectPeHeaders(const std::string& moduleName, bool& ok) {
    ok = false;
    PeHeaderInfo info{};

    uintptr_t base = 0;
    size_t modSize = 0;
    if (!FindModule(moduleName, base, modSize)) return info;

    IMAGE_DOS_HEADER dos;
    if (!ReadStruct(base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE) return info;

    uintptr_t ntAddress = base + dos.e_lfanew;
    IMAGE_NT_HEADERS nt;
    if (!ReadStruct(ntAddress, nt) || nt.Signature != IMAGE_NT_SIGNATURE) return info;

    info.base = base;
    info.sizeOfImage = nt.OptionalHeader.SizeOfImage;
    info.entryPoint = base + nt.OptionalHeader.AddressOfEntryPoint;
    info.machine = nt.FileHeader.Machine;
    info.subsystem = nt.OptionalHeader.Subsystem;
    info.timestamp = nt.FileHeader.TimeDateStamp;
    info.characteristics = nt.FileHeader.Characteristics;
    info.isDll = (nt.FileHeader.Characteristics & IMAGE_FILE_DLL) != 0;

    uintptr_t sectionTableAddr =
        ntAddress + offsetof(IMAGE_NT_HEADERS, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
    for (WORD i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER sh;
        if (!ReadStruct(sectionTableAddr + i * sizeof(IMAGE_SECTION_HEADER), sh)) break;
        char nameBuf[9] = {0};
        memcpy(nameBuf, sh.Name, 8);
        info.sections.push_back({nameBuf, base + sh.VirtualAddress, sh.Misc.VirtualSize, sh.SizeOfRawData,
                                  sh.Characteristics});
    }

    if (nt.OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_IMPORT) {
        const auto& dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (dir.VirtualAddress != 0) {
            uintptr_t descAddr = base + dir.VirtualAddress;
            for (;;) {
                IMAGE_IMPORT_DESCRIPTOR desc;
                if (!ReadStruct(descAddr, desc)) break;
                if (desc.Name == 0 && desc.FirstThunk == 0 && desc.OriginalFirstThunk == 0) break;
                std::string modName = ReadCString(base + desc.Name);

                uintptr_t thunkRva = desc.OriginalFirstThunk != 0 ? desc.OriginalFirstThunk : desc.FirstThunk;
                uintptr_t intAddr = base + thunkRva;
                uintptr_t iatAddr = base + desc.FirstThunk;
                for (size_t i = 0;; ++i) {
                    IMAGE_THUNK_DATA thunk;
                    if (!ReadStruct(intAddr + i * sizeof(IMAGE_THUNK_DATA), thunk)) break;
                    if (thunk.u1.AddressOfData == 0) break;
                    if (info.imports.size() >= kMaxImports) { info.importsTruncated = true; break; }

                    PeImportEntry entry;
                    entry.moduleName = modName;
                    entry.iatSlot = iatAddr + i * sizeof(IMAGE_THUNK_DATA);
                    if (IMAGE_SNAP_BY_ORDINAL(thunk.u1.Ordinal)) {
                        entry.ordinal = static_cast<uint16_t>(IMAGE_ORDINAL(thunk.u1.Ordinal));
                    } else {
                        uintptr_t ibnAddr = base + static_cast<uintptr_t>(thunk.u1.AddressOfData);
                        std::vector<uint8_t> hintBuf;
                        uint16_t hint = 0;
                        if (memory::ReadBytes(ibnAddr, 2, hintBuf) && hintBuf.size() == 2) memcpy(&hint, hintBuf.data(), 2);
                        entry.ordinal = hint;
                        entry.functionName = ReadCString(ibnAddr + 2);
                    }
                    info.imports.push_back(entry);
                }
                if (info.importsTruncated) break;
                descAddr += sizeof(IMAGE_IMPORT_DESCRIPTOR);
            }
        }
    }

    if (nt.OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_EXPORT) {
        const auto& dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (dir.VirtualAddress != 0) {
            IMAGE_EXPORT_DIRECTORY exp;
            if (ReadStruct(base + dir.VirtualAddress, exp)) {
                std::vector<uint32_t> functions(exp.NumberOfFunctions);
                std::vector<uint8_t> funcBuf;
                if (exp.NumberOfFunctions > 0 &&
                    memory::ReadBytes(base + exp.AddressOfFunctions, exp.NumberOfFunctions * sizeof(uint32_t), funcBuf) &&
                    funcBuf.size() == exp.NumberOfFunctions * sizeof(uint32_t)) {
                    memcpy(functions.data(), funcBuf.data(), funcBuf.size());
                }

                std::vector<uint32_t> nameRvas(exp.NumberOfNames);
                std::vector<uint8_t> nameBuf;
                if (exp.NumberOfNames > 0 &&
                    memory::ReadBytes(base + exp.AddressOfNames, exp.NumberOfNames * sizeof(uint32_t), nameBuf) &&
                    nameBuf.size() == exp.NumberOfNames * sizeof(uint32_t)) {
                    memcpy(nameRvas.data(), nameBuf.data(), nameBuf.size());
                }

                std::vector<uint16_t> nameOrdinals(exp.NumberOfNames);
                std::vector<uint8_t> ordBuf;
                if (exp.NumberOfNames > 0 &&
                    memory::ReadBytes(base + exp.AddressOfNameOrdinals, exp.NumberOfNames * sizeof(uint16_t), ordBuf) &&
                    ordBuf.size() == exp.NumberOfNames * sizeof(uint16_t)) {
                    memcpy(nameOrdinals.data(), ordBuf.data(), ordBuf.size());
                }

                std::set<uint32_t> namedIndices;
                for (size_t i = 0; i < nameRvas.size() && i < nameOrdinals.size(); ++i) {
                    if (info.exports.size() >= kMaxExports) { info.exportsTruncated = true; break; }
                    uint32_t ordIndex = nameOrdinals[i];
                    namedIndices.insert(ordIndex);
                    if (ordIndex >= functions.size() || functions[ordIndex] == 0) continue;
                    PeExportEntry entry;
                    entry.name = ReadCString(base + nameRvas[i]);
                    entry.ordinal = static_cast<uint16_t>(ordIndex + exp.Base);
                    entry.address = base + functions[ordIndex];
                    info.exports.push_back(entry);
                }
                if (!info.exportsTruncated) {
                    for (uint32_t i = 0; i < functions.size(); ++i) {
                        if (info.exports.size() >= kMaxExports) { info.exportsTruncated = true; break; }
                        if (namedIndices.count(i) || functions[i] == 0) continue;
                        info.exports.push_back({"", static_cast<uint16_t>(i + exp.Base), base + functions[i]});
                    }
                }
            }
        }
    }

    ok = true;
    return info;
}

ScanPatchesResult ScanPatches(const std::string& moduleName, size_t minRunLength, bool& ok) {
    ok = false;
    ScanPatchesResult result;
    if (minRunLength == 0) minRunLength = 1;

    uintptr_t base = 0;
    size_t modSize = 0;
    if (!FindModule(moduleName, base, modSize)) return result;

    char pathBuf[MAX_PATH] = {0};
    if (GetModuleFileNameA(reinterpret_cast<HMODULE>(base), pathBuf, MAX_PATH) == 0) return result;

    HANDLE hFile = CreateFileA(pathBuf, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return result;

    LARGE_INTEGER fileSizeLi{};
    if (!GetFileSizeEx(hFile, &fileSizeLi) || fileSizeLi.QuadPart <= 0) {
        CloseHandle(hFile);
        return result;
    }
    size_t fileSize = static_cast<size_t>(fileSizeLi.QuadPart);

    HANDLE hMap = CreateFileMappingA(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!hMap) {
        CloseHandle(hFile);
        return result;
    }
    LPVOID mapView = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if (!mapView) {
        CloseHandle(hMap);
        CloseHandle(hFile);
        return result;
    }

    const uint8_t* diskBase = reinterpret_cast<const uint8_t*>(mapView);
    bool parsedOk = false;
    if (fileSize >= sizeof(IMAGE_DOS_HEADER)) {
        IMAGE_DOS_HEADER dos;
        memcpy(&dos, diskBase, sizeof(dos));
        if (dos.e_magic == IMAGE_DOS_SIGNATURE &&
            static_cast<size_t>(dos.e_lfanew) + sizeof(IMAGE_NT_HEADERS) <= fileSize) {
            IMAGE_NT_HEADERS nt;
            memcpy(&nt, diskBase + dos.e_lfanew, sizeof(nt));
            if (nt.Signature == IMAGE_NT_SIGNATURE) {
                parsedOk = true;
                size_t sectionTableOffset =
                    dos.e_lfanew + offsetof(IMAGE_NT_HEADERS, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;

                for (WORD s = 0; s < nt.FileHeader.NumberOfSections && result.runs.size() < kMaxPatchRuns; ++s) {
                    size_t shOffset = sectionTableOffset + s * sizeof(IMAGE_SECTION_HEADER);
                    if (shOffset + sizeof(IMAGE_SECTION_HEADER) > fileSize) break;
                    IMAGE_SECTION_HEADER sh;
                    memcpy(&sh, diskBase + shOffset, sizeof(sh));
                    if (!(sh.Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;

                    size_t compareSize = (std::min)({static_cast<size_t>(sh.Misc.VirtualSize),
                                                       static_cast<size_t>(sh.SizeOfRawData), kMaxPatchCompareBytes});
                    if (compareSize == 0) continue;
                    if (static_cast<size_t>(sh.PointerToRawData) >= fileSize) continue;
                    compareSize = (std::min)(compareSize, fileSize - static_cast<size_t>(sh.PointerToRawData));

                    std::vector<uint8_t> memBytes;
                    if (!memory::ReadBytes(base + sh.VirtualAddress, compareSize, memBytes) ||
                        memBytes.size() != compareSize) {
                        continue;
                    }
                    const uint8_t* diskSection = diskBase + sh.PointerToRawData;

                    std::vector<size_t> diffPositions;
                    for (size_t i = 0; i < compareSize; ++i) {
                        if (diskSection[i] != memBytes[i]) diffPositions.push_back(i);
                    }

                    size_t idx = 0;
                    while (idx < diffPositions.size() && result.runs.size() < kMaxPatchRuns) {
                        size_t runStartIdx = diffPositions[idx];
                        size_t runEndIdx = runStartIdx;
                        size_t j = idx + 1;
                        while (j < diffPositions.size() && diffPositions[j] - runEndIdx <= minRunLength) {
                            runEndIdx = diffPositions[j];
                            ++j;
                        }
                        size_t len = runEndIdx - runStartIdx + 1;
                        PatchRun run;
                        run.address = base + sh.VirtualAddress + runStartIdx;
                        run.diskBytes.assign(diskSection + runStartIdx, diskSection + runStartIdx + len);
                        run.memoryBytes.assign(memBytes.begin() + runStartIdx, memBytes.begin() + runStartIdx + len);
                        result.runs.push_back(std::move(run));
                        idx = j;
                    }
                    if (idx < diffPositions.size()) result.truncated = true;
                }
            }
        }
    }

    UnmapViewOfFile(mapView);
    CloseHandle(hMap);
    CloseHandle(hFile);

    ok = parsedOk;
    return result;
}

} // namespace analysis
