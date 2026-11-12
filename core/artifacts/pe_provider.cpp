#include "native_more_provider.h"
#include "pe_provider.h"
#include "pe_extras.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <set>

namespace cortex::artifacts {
namespace {

constexpr size_t kMaxSections = 96;
constexpr size_t kMaxImportModules = 1024;
constexpr size_t kMaxName = 512;

const char* MachineName(uint16_t machine) {
    switch (machine) {
        case 0x014c: return "x86";
        case 0x8664: return "x64";
        case 0xaa64: return "arm64";
        case 0x01c4: return "arm";
        case 0x0200: return "ia64";
        default: return "unknown";
    }
}

const char* SubsystemName(uint16_t subsystem) {
    switch (subsystem) {
        case 1: return "native";
        case 2: return "windows_gui";
        case 3: return "windows_cui";
        case 9: return "windows_ce_gui";
        case 10: return "efi_application";
        case 11: return "efi_boot_service_driver";
        case 12: return "efi_runtime_driver";
        case 16: return "windows_boot_application";
        default: return "unknown";
    }
}

const char* DebugTypeName(uint32_t type) {
    switch (type) {
        case 1: return "coff";
        case 2: return "codeview";
        case 4: return "misc";
        case 9: return "borland";
        case 12: return "vc_feature";
        case 13: return "pogo";
        case 14: return "iltcg";
        case 16: return "repro";
        case 17: return "embedded_portable_pdb";
        case 19: return "pdb_checksum";
        case 20: return "ex_dllcharacteristics";
        default: return "other";
    }
}

std::string SectionName(const uint8_t (&raw)[8]) {
    std::string name;
    for (uint8_t c : raw) {
        if (c == 0) break;
        name.push_back(c >= 0x20 && c < 0x7f ? static_cast<char>(c) : '?');
    }
    return name;
}

json SectionFlags(uint32_t characteristics) {
    json flags = json::array();
    if (characteristics & 0x00000020) flags.push_back("code");
    if (characteristics & 0x00000040) flags.push_back("initialized_data");
    if (characteristics & 0x00000080) flags.push_back("uninitialized_data");
    if (characteristics & 0x02000000) flags.push_back("discardable");
    if (characteristics & 0x10000000) flags.push_back("shared");
    if (characteristics & 0x20000000) flags.push_back("execute");
    if (characteristics & 0x40000000) flags.push_back("read");
    if (characteristics & 0x80000000) flags.push_back("write");
    return flags;
}

json DllCharacteristics(uint16_t value) {
    json flags = json::array();
    if (value & 0x0020) flags.push_back("high_entropy_va");
    if (value & 0x0040) flags.push_back("dynamic_base");
    if (value & 0x0080) flags.push_back("force_integrity");
    if (value & 0x0100) flags.push_back("nx_compat");
    if (value & 0x0200) flags.push_back("no_isolation");
    if (value & 0x0400) flags.push_back("no_seh");
    if (value & 0x0800) flags.push_back("no_bind");
    if (value & 0x1000) flags.push_back("appcontainer");
    if (value & 0x2000) flags.push_back("wdm_driver");
    if (value & 0x4000) flags.push_back("guard_cf");
    if (value & 0x8000) flags.push_back("terminal_server_aware");
    return flags;
}

// Reads a NUL-terminated string at an RVA, or "" with a warning.
std::string StringAt(const ByteSource& source, const PeFile& pe, uint32_t rva, bool* ok = nullptr) {
    uint64_t offset = 0;
    std::string text;
    const bool found = pe.RvaToOffset(rva, offset) && source.ReadCString(offset, kMaxName, text);
    if (ok) *ok = found;
    return found ? text : std::string();
}

json Relocations(const ByteSource& source, const PeFile& pe) {
    const auto& dir = pe.directories[kPeBaseReloc];
    uint64_t offset = 0;
    uint32_t available = 0;
    json result = {{"blocks", 0}, {"entries", 0}, {"by_type", json::object()}};
    if (!pe.RvaToOffset(dir.rva, offset, &available)) {
        result["error"] = "directory_outside_file";
        return result;
    }
    const uint64_t end = offset + std::min<uint64_t>(dir.size, available);
    std::map<int, uint64_t> byType;
    uint64_t blocks = 0, entries = 0;
    for (uint64_t at = offset; at + 8 <= end && blocks < 1000000;) {
        uint32_t page = 0, blockSize = 0;
        if (!ReadU32(source, at, page) || !ReadU32(source, at + 4, blockSize)) break;
        if (blockSize < 8 || at + blockSize > end) {
            result["error"] = "malformed_block";
            break;
        }
        for (uint64_t e = at + 8; e + 2 <= at + blockSize; e += 2) {
            uint16_t entry = 0;
            if (!ReadU16(source, e, entry)) break;
            ++byType[entry >> 12];
            ++entries;
        }
        ++blocks;
        at += blockSize;
    }
    static const char* names[] = {"absolute", "high", "low", "highlow", "highadj", "arm_mov32", "reserved",
                                  "thumb_mov32", "riscv_low12s", "riscv_mips_jmpaddr16", "dir64"};
    for (const auto& [type, count] : byType)
        result["by_type"][type <= 10 ? names[type] : std::to_string(type)] = count;
    result["blocks"] = blocks;
    result["entries"] = entries;
    return result;
}

json DebugEntries(const ByteSource& source, const PeFile& pe) {
    const auto& dir = pe.directories[kPeDebug];
    uint64_t offset = 0;
    json rows = json::array();
    if (!pe.RvaToOffset(dir.rva, offset)) return rows;
    const uint32_t count = std::min<uint32_t>(dir.size / 28, 64);
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t at = offset + i * 28ull;
        uint32_t type = 0, size = 0, rawPointer = 0, stamp = 0;
        if (!ReadU32(source, at + 4, stamp) || !ReadU32(source, at + 12, type) ||
            !ReadU32(source, at + 16, size) || !ReadU32(source, at + 24, rawPointer))
            break;
        json row = {{"type", DebugTypeName(type)}, {"type_id", type}, {"size", size}, {"timestamp", stamp}};
        if (type == 2 && size >= 24) {
            uint32_t signature = 0;
            if (ReadU32(source, rawPointer, signature) && signature == 0x53445352) {  // "RSDS"
                const auto guid = source.ReadSome(rawPointer + 4, 16);
                uint32_t age = 0;
                std::string path;
                if (guid.size() == 16 && ReadU32(source, rawPointer + 20, age)) {
                    char text[40];
                    std::snprintf(text, sizeof(text), "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                                  guid[3], guid[2], guid[1], guid[0], guid[5], guid[4], guid[7], guid[6], guid[8],
                                  guid[9], guid[10], guid[11], guid[12], guid[13], guid[14], guid[15]);
                    source.ReadCString(rawPointer + 24, std::min<uint32_t>(size - 24, kMaxName), path);
                    row["codeview"] = {{"format", "RSDS"}, {"guid", text}, {"age", age}, {"pdb", path}};
                }
            }
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

json TlsCallbacks(const ByteSource& source, const PeFile& pe) {
    json result = {{"callbacks", json::array()}};
    uint64_t offset = 0;
    if (!pe.RvaToOffset(pe.directories[kPeTls].rva, offset)) {
        result["error"] = "directory_outside_file";
        return result;
    }
    uint64_t callbacksVa = 0;
    if (pe.pe32Plus) {
        if (!ReadU64(source, offset + 24, callbacksVa)) return result;
    } else {
        uint32_t va = 0;
        if (!ReadU32(source, offset + 12, va)) return result;
        callbacksVa = va;
    }
    if (callbacksVa == 0 || callbacksVa < pe.imageBase) return result;
    uint64_t array = 0;
    if (!pe.RvaToOffset(static_cast<uint32_t>(callbacksVa - pe.imageBase), array)) return result;
    for (int i = 0; i < 64; ++i) {
        uint64_t va = 0;
        if (pe.pe32Plus) {
            if (!ReadU64(source, array + i * 8ull, va)) break;
        } else {
            uint32_t value = 0;
            if (!ReadU32(source, array + i * 4ull, value)) break;
            va = value;
        }
        if (va == 0) break;
        result["callbacks"].push_back({{"va", HexU64(va)}, {"rva", HexU64(va - pe.imageBase)}});
    }
    return result;
}

json ClrHeader(const ByteSource& source, const PeFile& pe) {
    json result;
    uint64_t offset = 0;
    if (!pe.RvaToOffset(pe.directories[kPeClr].rva, offset)) return {{"error", "directory_outside_file"}};
    uint32_t cb = 0, metadataRva = 0, metadataSize = 0, flags = 0, entryToken = 0;
    uint16_t major = 0, minor = 0;
    if (!ReadU32(source, offset, cb) || !ReadU16(source, offset + 4, major) || !ReadU16(source, offset + 6, minor) ||
        !ReadU32(source, offset + 8, metadataRva) || !ReadU32(source, offset + 12, metadataSize) ||
        !ReadU32(source, offset + 16, flags) || !ReadU32(source, offset + 20, entryToken))
        return {{"error", "truncated_clr_header"}};
    json flagNames = json::array();
    if (flags & 0x1) flagNames.push_back("il_only");
    if (flags & 0x2) flagNames.push_back("32bit_required");
    if (flags & 0x8) flagNames.push_back("strong_name_signed");
    if (flags & 0x10) flagNames.push_back("native_entrypoint");
    if (flags & 0x20000) flagNames.push_back("32bit_preferred");
    result = {{"header_size", cb},
              {"runtime_version", std::to_string(major) + "." + std::to_string(minor)},
              {"flags", flagNames},
              {"metadata_rva", HexU64(metadataRva)},
              {"metadata_size", metadataSize},
              {(flags & 0x10) ? "entry_point_rva" : "entry_point_token", HexU64(entryToken)}};
    uint64_t metadata = 0;
    uint32_t signature = 0, versionLength = 0;
    if (pe.RvaToOffset(metadataRva, metadata) && ReadU32(source, metadata, signature) && signature == 0x424A5342 &&
        ReadU32(source, metadata + 12, versionLength) && versionLength <= 255) {
        std::string version;
        source.ReadCString(metadata + 16, versionLength, version);
        result["metadata_version"] = version;  // e.g. v4.0.30319
    } else {
        result["metadata_error"] = "no_BSJB_metadata_root";
    }
    return result;
}


std::string LowerAscii(std::string text) {
    for (char& ch : text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return text;
}

// A driver is a native-subsystem PE, not just a file ending in .sys.
// The name and imports are evidence of *likely* role, never proof that a
// loader would accept it or that its entry point is named DriverEntry.
json DriverDetails(const ByteSource& source, const PeFile& pe, const json& imports, bool importsTruncated) {
    if (pe.subsystem != 1 || pe.HasDirectory(kPeClr)) return nullptr;
    const std::string name = source.Name();
    const size_t separator = name.find_last_of("/\\!");
    const std::string leaf = LowerAscii(separator == std::string::npos ? name : name.substr(separator + 1));
    const bool sysExtension = leaf.size() >= 4 && leaf.compare(leaf.size() - 4, 4, ".sys") == 0;
    const bool wdmFlag = (pe.dllCharacteristics & 0x2000) != 0;
    static const std::set<std::string> kernelModules = {
        "ntoskrnl.exe", "ntkrnlmp.exe", "ntkrnlpa.exe", "hal.dll",
        "wdfldr.sys", "wdf01000.sys", "fltmgr.sys", "ndis.sys",
        "storport.sys", "scsiport.sys"
    };
    std::set<std::string> observed;
    if (imports.is_array()) {
        for (const auto& row : imports) {
            if (!row.is_object() || !row.contains("name") || !row["name"].is_string()) continue;
            const std::string module = LowerAscii(row["name"].get<std::string>());
            if (kernelModules.count(module)) observed.insert(module);
        }
    }
    if (!sysExtension && !wdmFlag && observed.empty()) return nullptr;

    json indicators = json::array();
    if (sysExtension) indicators.push_back("sys_extension");
    if (wdmFlag) indicators.push_back("wdm_driver_flag");
    if (!observed.empty()) indicators.push_back("kernel_imports");

    json modules = json::array();
    for (const auto& module : observed) modules.push_back(module);
    json result = {{"entry_rva", HexU64(pe.entryRva)},
                   {"indicators", std::move(indicators)},
                   {"kernel_imports", std::move(modules)},
                   {"classification", "candidate"},
                   {"note", "PE Native subsystem and driver indicators; the entry RVA is not a proven DriverEntry symbol. File is only read, never loaded."}};
    if (pe.entryRva == 0) result["warning"] = "no_entry_point";
    if (importsTruncated) result["kernel_imports_may_be_incomplete"] = true;
    return result;
}

} // namespace

const PeSection* PeFile::SectionFor(uint32_t rva) const {
    for (const auto& section : sections) {
        const uint32_t span = std::max(section.virtualSize, section.rawSize);
        if (rva >= section.virtualAddress && rva - section.virtualAddress < span) return &section;
    }
    return nullptr;
}

bool PeFile::RvaToOffset(uint32_t rva, uint64_t& offset, uint32_t* available) const {
    if (rva == 0) return false;
    if (rva < sizeOfHeaders && (sections.empty() || rva < sections.front().virtualAddress)) {
        offset = rva;
        if (available) *available = sizeOfHeaders - rva;
        return true;
    }
    const PeSection* section = SectionFor(rva);
    if (!section) return false;
    const uint32_t delta = rva - section->virtualAddress;
    if (delta >= section->rawSize) return false;  // zero-filled at load, nothing in the file
    offset = static_cast<uint64_t>(section->rawOffset) + delta;
    if (available) *available = section->rawSize - delta;
    return true;
}

bool ParsePe(const ByteSource& source, PeFile& pe, std::string& error) {
    pe = PeFile{};
    uint16_t mz = 0;
    uint32_t lfanew = 0, signature = 0;
    if (!ReadU16(source, 0, mz) || mz != 0x5A4D) { error = "not_mz"; return false; }
    if (!ReadU32(source, 0x3C, lfanew) || lfanew < 0x40 || lfanew > 0x10000000) { error = "bad_e_lfanew"; return false; }
    if (!ReadU32(source, lfanew, signature) || signature != 0x00004550) { error = "not_pe"; return false; }

    const uint64_t fileHeader = lfanew + 4ull;
    uint16_t sectionCount = 0, optionalSize = 0;
    uint32_t symbolTable = 0, symbolCount = 0;
    if (!ReadU16(source, fileHeader, pe.machine) || !ReadU16(source, fileHeader + 2, sectionCount) ||
        !ReadU32(source, fileHeader + 4, pe.timestamp) || !ReadU32(source, fileHeader + 8, symbolTable) ||
        !ReadU32(source, fileHeader + 12, symbolCount) || !ReadU16(source, fileHeader + 16, optionalSize) ||
        !ReadU16(source, fileHeader + 18, pe.characteristics)) {
        error = "truncated_file_header";
        return false;
    }
    const uint64_t optional = fileHeader + 20;
    uint16_t magic = 0;
    if (!ReadU16(source, optional, magic) || (magic != 0x10b && magic != 0x20b)) { error = "bad_optional_magic"; return false; }
    pe.pe32Plus = magic == 0x20b;
    uint32_t directoryCount = 0;
    const uint64_t directoryTable = optional + (pe.pe32Plus ? 112 : 96);
    bool ok = ReadU32(source, optional + 16, pe.entryRva) && ReadU32(source, optional + 56, pe.sizeOfImage) &&
              ReadU32(source, optional + 60, pe.sizeOfHeaders) && ReadU16(source, optional + 68, pe.subsystem) &&
              ReadU16(source, optional + 70, pe.dllCharacteristics) &&
              ReadU32(source, optional + (pe.pe32Plus ? 108 : 92), directoryCount);
    if (pe.pe32Plus) {
        ok = ok && ReadU64(source, optional + 24, pe.imageBase);
    } else {
        uint32_t base = 0;
        ok = ok && ReadU32(source, optional + 28, base);
        pe.imageBase = base;
    }
    if (!ok) { error = "truncated_optional_header"; return false; }

    // The directory table must fit inside the declared optional header.
    const uint64_t optionalEnd = optional + optionalSize;
    const uint64_t fit = directoryTable > optionalEnd ? 0 : (optionalEnd - directoryTable) / 8;
    pe.directoryCount = static_cast<uint32_t>(std::min<uint64_t>({directoryCount, 16, fit}));
    if (directoryCount > 16) pe.warnings.push_back("directory_count_clamped");
    for (uint32_t i = 0; i < pe.directoryCount; ++i) {
        if (!ReadU32(source, directoryTable + i * 8ull, pe.directories[i].rva) ||
            !ReadU32(source, directoryTable + i * 8ull + 4, pe.directories[i].size)) {
            pe.warnings.push_back("truncated_directory_table");
            pe.directoryCount = i;
            break;
        }
    }

    if (sectionCount > kMaxSections) {
        pe.warnings.push_back("section_count_clamped");
        sectionCount = kMaxSections;
    }
    const uint64_t sectionTable = optionalEnd;
    for (uint16_t i = 0; i < sectionCount; ++i) {
        const uint64_t at = sectionTable + i * 40ull;
        uint8_t raw[8] = {};
        PeSection section;
        if (!source.Read(at, raw, 8) || !ReadU32(source, at + 8, section.virtualSize) ||
            !ReadU32(source, at + 12, section.virtualAddress) || !ReadU32(source, at + 16, section.rawSize) ||
            !ReadU32(source, at + 20, section.rawOffset) || !ReadU32(source, at + 36, section.characteristics)) {
            pe.warnings.push_back("truncated_section_table");
            break;
        }
        section.name = SectionName(raw);
        // "/N": a name longer than 8 bytes, stored at offset N of the COFF
        // string table that follows the symbol table (MinGW debug sections).
        if (section.name.size() > 1 && section.name[0] == '/' && symbolTable &&
            section.name.find_first_not_of("0123456789", 1) == std::string::npos) {
            const uint64_t strings = uint64_t(symbolTable) + uint64_t(symbolCount) * 18;
            const uint64_t index = std::stoull(section.name.substr(1));
            std::string longName;
            if (index >= 4 && source.ReadCString(strings + index, 256, longName) && !longName.empty())
                section.name = longName;
        }
        if (section.rawOffset > source.Size()) {
            pe.warnings.push_back("section_outside_file:" + section.name);
            section.rawSize = 0;
        } else if (section.rawSize > source.Size() - section.rawOffset) {
            pe.warnings.push_back("section_truncated:" + section.name);
            section.rawSize = static_cast<uint32_t>(source.Size() - section.rawOffset);
        }
        pe.sections.push_back(std::move(section));
    }
    return true;
}

json PeImports(const ByteSource& source, const PeFile& pe, const InspectOptions& options, bool& truncated) {
    truncated = false;
    json modules = json::array();
    uint64_t offset = 0;
    if (!pe.RvaToOffset(pe.directories[kPeImport].rva, offset)) return modules;
    size_t total = 0;
    const unsigned thunkSize = pe.pe32Plus ? 8 : 4;
    for (size_t index = 0; index < kMaxImportModules; ++index) {
        const uint64_t at = offset + index * 20ull;
        uint32_t lookup = 0, nameRva = 0, iat = 0;
        if (!ReadU32(source, at, lookup) || !ReadU32(source, at + 12, nameRva) || !ReadU32(source, at + 16, iat))
            break;
        if (lookup == 0 && nameRva == 0 && iat == 0) break;
        bool nameOk = false;
        json module = {{"name", StringAt(source, pe, nameRva, &nameOk)}, {"functions", json::array()}};
        if (!nameOk) module["warning"] = "name_unreadable";
        uint64_t thunks = 0;
        if (pe.RvaToOffset(lookup ? lookup : iat, thunks)) {
            for (uint32_t t = 0; t < 65536; ++t) {
                uint64_t value = 0;
                if (pe.pe32Plus) {
                    if (!ReadU64(source, thunks + t * 8ull, value)) break;
                } else {
                    uint32_t small = 0;
                    if (!ReadU32(source, thunks + t * 4ull, small)) break;
                    value = small;
                }
                if (value == 0) break;
                if (total >= options.maxEntries) { truncated = true; break; }
                const bool byOrdinal = pe.pe32Plus ? (value >> 63) != 0 : (value >> 31) != 0;
                const uint32_t slot = iat + t * thunkSize;
                if (byOrdinal) {
                    module["functions"].push_back({{"ordinal", value & 0xFFFF}, {"iat_rva", HexU64(slot)}});
                } else {
                    const uint32_t hintRva = static_cast<uint32_t>(value & 0x7FFFFFFF);
                    uint64_t hintOffset = 0;
                    uint16_t hint = 0;
                    std::string name;
                    if (pe.RvaToOffset(hintRva, hintOffset) && ReadU16(source, hintOffset, hint))
                        source.ReadCString(hintOffset + 2, kMaxName, name);
                    module["functions"].push_back({{"name", name}, {"hint", hint}, {"iat_rva", HexU64(slot)}});
                }
                ++total;
            }
        }
        modules.push_back(std::move(module));
        if (truncated) break;
    }
    return modules;
}

json PeExports(const ByteSource& source, const PeFile& pe, const InspectOptions& options, bool& truncated) {
    truncated = false;
    json result = {{"functions", json::array()}};
    const auto& dir = pe.directories[kPeExport];
    uint64_t offset = 0;
    if (!pe.RvaToOffset(dir.rva, offset)) return result;
    uint32_t nameRva = 0, base = 0, functionCount = 0, nameCount = 0, functions = 0, names = 0, ordinals = 0;
    if (!ReadU32(source, offset + 12, nameRva) || !ReadU32(source, offset + 16, base) ||
        !ReadU32(source, offset + 20, functionCount) || !ReadU32(source, offset + 24, nameCount) ||
        !ReadU32(source, offset + 28, functions) || !ReadU32(source, offset + 32, names) ||
        !ReadU32(source, offset + 36, ordinals)) {
        result["error"] = "truncated_export_directory";
        return result;
    }
    result["dll_name"] = StringAt(source, pe, nameRva);
    result["ordinal_base"] = base;
    std::map<uint32_t, std::string> namesByIndex;
    uint64_t namesOffset = 0, ordinalsOffset = 0;
    if (pe.RvaToOffset(names, namesOffset) && pe.RvaToOffset(ordinals, ordinalsOffset)) {
        for (uint32_t i = 0; i < std::min<uint32_t>(nameCount, 1u << 20); ++i) {
            uint32_t entryName = 0;
            uint16_t index = 0;
            if (!ReadU32(source, namesOffset + i * 4ull, entryName) || !ReadU16(source, ordinalsOffset + i * 2ull, index))
                break;
            namesByIndex[index] = StringAt(source, pe, entryName);
        }
    }
    uint64_t functionsOffset = 0;
    if (!pe.RvaToOffset(functions, functionsOffset)) return result;
    for (uint32_t i = 0; i < std::min<uint32_t>(functionCount, 1u << 20); ++i) {
        uint32_t rva = 0;
        if (!ReadU32(source, functionsOffset + i * 4ull, rva)) break;
        if (rva == 0) continue;
        if (result["functions"].size() >= options.maxEntries) { truncated = true; break; }
        json row = {{"ordinal", base + i}, {"rva", HexU64(rva)}};
        const auto named = namesByIndex.find(i);
        if (named != namesByIndex.end()) row["name"] = named->second;
        if (rva >= dir.rva && rva - dir.rva < dir.size) row["forwarded_to"] = StringAt(source, pe, rva);
        result["functions"].push_back(std::move(row));
    }
    return result;
}

std::vector<Capability> PeProvider::Capabilities() const {
    return {{"static", CapabilityStatus::Validated, "cortex_artifacts_tests (PE fixtures, malformed inputs)",
             "headers, sections with entropy, imports with imphash, delay imports, exports, relocations, debug/CodeView, "
             "TLS, exception table, CLR header, overlay, Rich header, resources (version, manifest), load config, "
             "hardening (ASLR, DEP, CFG, SafeSEH, CET, /GS), packer detection, Authenticode (signer, certificates, "
             "digest match; chain not verified), native-subsystem .sys driver indicators and kernel imports"},
            {"decompile", CapabilityStatus::Unavailable, "",
             "no built-in decompiler. External: the Ghidra analysis provider (cortex_analysis_start provider=ghidra, "
             "`cortex provider run ghidra`) when the operator configures Ghidra"},
            {"live", CapabilityStatus::Validated, "native engine",
             "the native memory/debugger engine attaches to running PE processes"}};
}

int PeProvider::Detect(const ByteSource& source) const {
    PeFile pe;
    std::string error;
    if (!ParsePe(source, pe, error)) return 0;
    return pe.HasDirectory(kPeClr) ? 55 : 50;
}

json PeProvider::Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const {
    PeFile pe;
    if (!ParsePe(source, pe, error)) return nullptr;

    json sections = json::array();
    uint64_t lastRawEnd = pe.sizeOfHeaders;
    for (const auto& section : pe.sections) {
        sections.push_back({{"name", section.name},
                            {"rva", HexU64(section.virtualAddress)},
                            {"virtual_size", section.virtualSize},
                            {"raw_offset", HexU64(section.rawOffset)},
                            {"raw_size", section.rawSize},
                            {"entropy", Entropy(source, section.rawOffset, section.rawSize)},
                            {"flags", SectionFlags(section.characteristics)}});
        if (section.rawSize) lastRawEnd = std::max<uint64_t>(lastRawEnd, uint64_t(section.rawOffset) + section.rawSize);
    }

    static const char* directoryNames[16] = {"export", "import", "resource", "exception", "security", "basereloc",
                                             "debug", "architecture", "globalptr", "tls", "load_config",
                                             "bound_import", "iat", "delay_import", "clr", "reserved"};
    json directories = json::object();
    for (uint32_t i = 0; i < pe.directoryCount; ++i)
        if (pe.directories[i].rva || pe.directories[i].size)
            directories[directoryNames[i]] = {{"rva", HexU64(pe.directories[i].rva)}, {"size", pe.directories[i].size}};

    const bool dotnet = pe.HasDirectory(kPeClr);
    const bool dll = (pe.characteristics & 0x2000) != 0;
    json data = {{"format", dotnet ? "pe-dotnet" : "pe"},
                 {"kind", dll ? "dll" : ((pe.characteristics & 0x0002) ? "executable" : "object")},
                 {"pe32_plus", pe.pe32Plus},
                 {"machine", MachineName(pe.machine)},
                 {"machine_id", HexU64(pe.machine)},
                 {"timestamp", pe.timestamp},
                 {"entry_rva", HexU64(pe.entryRva)},
                 {"image_base", HexU64(pe.imageBase)},
                 {"size_of_image", pe.sizeOfImage},
                 {"subsystem", SubsystemName(pe.subsystem)},
                 {"dll_characteristics", DllCharacteristics(pe.dllCharacteristics)},
                 {"sections", std::move(sections)},
                 {"directories", std::move(directories)}};

    bool importsTruncated = false;
    if (pe.HasDirectory(kPeImport)) {
        data["imports"] = PeImports(source, pe, options, importsTruncated);
        if (importsTruncated) data["imports_truncated"] = true;
    }
    if (pe.HasDirectory(kPeExport)) {
        bool exportsTruncated = false;
        data["exports"] = PeExports(source, pe, options, exportsTruncated);
        if (exportsTruncated) data["exports_truncated"] = true;
    }
    if (pe.HasDirectory(kPeBaseReloc)) data["relocations"] = Relocations(source, pe);
    if (pe.HasDirectory(kPeDebug)) data["debug"] = DebugEntries(source, pe);
    if (pe.HasDirectory(kPeTls)) data["tls"] = TlsCallbacks(source, pe);
    if (pe.HasDirectory(kPeException)) {
        const uint32_t entrySize = pe.machine == 0x8664 || pe.machine == 0xaa64 ? (pe.machine == 0x8664 ? 12 : 8) : 0;
        if (entrySize) data["exception"] = {{"runtime_functions", pe.directories[kPeException].size / entrySize}};
    }
    if (dotnet) data["clr"] = ClrHeader(source, pe);
    const json driver = DriverDetails(source, pe, data.value("imports", json::array()), importsTruncated);
    if (!driver.is_null()) {
        data["kind"] = "driver";
        data["driver"] = driver;
    }

    // What the file says about its build, signature and protections.
    if (data.contains("imports")) {
        const std::string imphash = ImportHash(data["imports"]);
        if (!imphash.empty()) data["imphash"] = imphash;
    }
    if (pe.HasDirectory(kPeDelayImport)) data["delay_imports"] = DelayImports(source, pe, options);
    const json rich = RichHeader(source);
    if (!rich.is_null()) data["rich_header"] = rich;
    if (pe.HasDirectory(kPeResource)) data["resources"] = Resources(source, pe, options);
    const json loadConfig = LoadConfig(source, pe);
    if (!loadConfig.is_null()) data["load_config"] = loadConfig;
    data["hardening"] = Hardening(source, pe, loadConfig);
    data["packers"] = Packers(source, pe, data.value("imports", json::array()), data["sections"]);
    const json signature = Authenticode(source, pe);
    if (!signature.is_null()) data["authenticode"] = signature;

    // Bytes after the last section that the loader never maps: installers,
    // PyInstaller archives, appended data. The Authenticode certificate
    // (security directory: a file offset, not an RVA) is reported apart.
    uint64_t overlayStart = lastRawEnd;
    if (pe.directoryCount > kPeSecurity && pe.directories[kPeSecurity].rva) {
        const auto& certificate = pe.directories[kPeSecurity];
        data["certificate"] = {{"offset", HexU64(certificate.rva)}, {"size", certificate.size}};
    }
    if (overlayStart < source.Size()) {
        uint64_t overlaySize = source.Size() - overlayStart;
        if (pe.directoryCount > kPeSecurity && pe.directories[kPeSecurity].rva >= overlayStart)
            overlaySize = pe.directories[kPeSecurity].rva - overlayStart;
        if (overlaySize) data["overlay"] = {{"offset", HexU64(overlayStart)}, {"size", overlaySize}};
    }
    const json go = GoBinaryInfo(source);
    if (!go.is_null()) data["go"] = go;
    if (!pe.warnings.empty()) data["warnings"] = pe.warnings;
    return data;
}

} // namespace cortex::artifacts
