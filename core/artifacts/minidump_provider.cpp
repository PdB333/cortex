#include "minidump_provider.h"

#include <algorithm>
#include <cstdio>

namespace cortex::artifacts {
namespace {

constexpr uint32_t kSignature = 0x504D444Du;  // "MDMP"

const char* StreamName(uint32_t type) {
    switch (type) {
        case 0: return "unused";
        case 3: return "thread_list";
        case 4: return "module_list";
        case 5: return "memory_list";
        case 6: return "exception";
        case 7: return "system_info";
        case 8: return "thread_ex_list";
        case 9: return "memory64_list";
        case 10: return "comment_a";
        case 11: return "comment_w";
        case 12: return "handle_data";
        case 13: return "function_table";
        case 14: return "unloaded_module_list";
        case 15: return "misc_info";
        case 16: return "memory_info_list";
        case 17: return "thread_info_list";
        case 18: return "handle_operation_list";
        case 19: return "token";
        case 21: return "system_memory_info";
        case 22: return "process_vm_counters";
        case 23: return "ipt_trace";
        case 24: return "thread_names";
        case 0xFFF0: return "wine_specific";
        default: return nullptr;
    }
}

const char* ExceptionName(uint32_t code) {
    switch (code) {
        case 0xC0000005: return "ACCESS_VIOLATION";
        case 0xC0000006: return "IN_PAGE_ERROR";
        case 0xC000001D: return "ILLEGAL_INSTRUCTION";
        case 0xC0000025: return "NONCONTINUABLE_EXCEPTION";
        case 0xC0000026: return "INVALID_DISPOSITION";
        case 0xC000008C: return "ARRAY_BOUNDS_EXCEEDED";
        case 0xC000008D: return "FLOAT_DENORMAL_OPERAND";
        case 0xC000008E: return "FLOAT_DIVIDE_BY_ZERO";
        case 0xC0000094: return "INTEGER_DIVIDE_BY_ZERO";
        case 0xC0000095: return "INTEGER_OVERFLOW";
        case 0xC0000096: return "PRIVILEGED_INSTRUCTION";
        case 0xC00000FD: return "STACK_OVERFLOW";
        case 0xC0000374: return "HEAP_CORRUPTION";
        case 0xC0000409: return "STACK_BUFFER_OVERRUN";
        case 0xC0000417: return "INVALID_CRUNTIME_PARAMETER";
        case 0x80000003: return "BREAKPOINT";
        case 0x80000004: return "SINGLE_STEP";
        case 0x80000001: return "GUARD_PAGE";
        case 0xE06D7363: return "C++ exception (MSVC)";
        case 0xE0434352: return "CLR exception (.NET)";
        case 0x40010005: return "CONTROL_C";
        case 0xCFFFFFFF: return "application hang (WER)";
        default: return nullptr;
    }
}

const char* ArchitectureName(uint16_t value) {
    switch (value) {
        case 0: return "x86";
        case 5: return "arm";
        case 6: return "ia64";
        case 9: return "x64";
        case 12: return "arm64";
        default: return "unknown";
    }
}

std::string Utf16(const ByteSource& source, uint32_t rva) {
    uint32_t bytes = 0;
    if (!rva || !ReadU32(source, rva, bytes) || bytes > 4096) return {};
    const auto raw = source.ReadSome(rva + 4, bytes);
    std::string out;
    for (size_t i = 0; i + 1 < raw.size(); i += 2) {
        uint32_t c = raw[i] | (raw[i + 1] << 8);
        if (c >= 0xD800 && c <= 0xDBFF && i + 3 < raw.size()) {
            const uint32_t low = raw[i + 2] | (raw[i + 3] << 8);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
                i += 2;
            }
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD;
        }
        if (c < 0x80) out += static_cast<char>(c);
        else if (c < 0x800) { out += char(0xC0 | (c >> 6)); out += char(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { out += char(0xE0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
        else { out += char(0xF0 | (c >> 18)); out += char(0x80 | ((c >> 12) & 0x3F)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
    }
    return out;
}

struct Module {
    uint64_t base = 0;
    uint32_t size = 0;
    std::string name;
};

std::string BaseName(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// "module+0xoffset" for an address inside a listed module.
std::string Locate(const std::vector<Module>& modules, uint64_t address) {
    for (const auto& module : modules)
        if (address >= module.base && address - module.base < module.size)
            return BaseName(module.name) + "+" + HexU64(address - module.base);
    return {};
}

// Instruction, stack and frame pointers from a thread context.
json ContextRegisters(const ByteSource& source, uint32_t size, uint32_t rva, uint16_t architecture,
                      const std::vector<Module>& modules) {
    json out = json::object();
    if (!rva) return out;
    if (architecture == 9 && size >= 0x100) {
        uint64_t rip = 0, rsp = 0, rbp = 0;
        if (ReadU64(source, rva + 0xF8, rip) && ReadU64(source, rva + 0x98, rsp) && ReadU64(source, rva + 0xA0, rbp)) {
            out = {{"ip", HexU64(rip)}, {"sp", HexU64(rsp)}, {"bp", HexU64(rbp)}};
            const std::string where = Locate(modules, rip);
            if (!where.empty()) out["ip_module"] = where;
        }
    } else if (architecture == 0 && size >= 0xCC) {
        uint32_t eip = 0, esp = 0, ebp = 0;
        if (ReadU32(source, rva + 0xB8, eip) && ReadU32(source, rva + 0xC4, esp) && ReadU32(source, rva + 0xB4, ebp)) {
            out = {{"ip", HexU64(eip)}, {"sp", HexU64(esp)}, {"bp", HexU64(ebp)}};
            const std::string where = Locate(modules, eip);
            if (!where.empty()) out["ip_module"] = where;
        }
    } else if (architecture == 12 && size >= 0x110) {
        uint64_t pc = 0, sp = 0, fp = 0;
        if (ReadU64(source, rva + 0x108, pc) && ReadU64(source, rva + 0x100, sp) && ReadU64(source, rva + 0xF0, fp)) {
            out = {{"ip", HexU64(pc)}, {"sp", HexU64(sp)}, {"bp", HexU64(fp)}};
            const std::string where = Locate(modules, pc);
            if (!where.empty()) out["ip_module"] = where;
        }
    }
    return out;
}

}  // namespace

std::vector<Capability> MinidumpProvider::Capabilities() const {
    return {{"static", CapabilityStatus::Validated, "cortex_artifacts_tests (a Wine minidump checked against the minidump package)",
             "system, process, exception and faulting module+offset, threads with registers, modules with versions "
             "and PDB identities, memory ranges, unknown streams listed"},
            {"decompile", CapabilityStatus::Unavailable, "", "a dump holds memory, not a program to decompile"},
            {"live", CapabilityStatus::Unavailable, "", "a dump is a snapshot; attach to a running process instead"}};
}

int MinidumpProvider::Detect(const ByteSource& source) const {
    uint32_t signature = 0, version = 0;
    if (!ReadU32(source, 0, signature) || signature != kSignature || !ReadU32(source, 4, version)) return 0;
    return (version & 0xFFFF) == 0xA793 ? 90 : 60;
}

json MinidumpProvider::Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const {
    uint32_t signature = 0, version = 0, streams = 0, directory = 0, stamp = 0;
    uint64_t flags = 0;
    if (!ReadU32(source, 0, signature) || signature != kSignature || !ReadU32(source, 4, version) ||
        !ReadU32(source, 8, streams) || !ReadU32(source, 12, directory) || !ReadU32(source, 20, stamp) ||
        !ReadU64(source, 24, flags)) {
        error = "not a minidump";
        return nullptr;
    }
    json data = {{"format", "minidump"}, {"version", HexU64(version & 0xFFFF)}, {"timestamp", stamp},
                 {"flags", HexU64(flags)}};
    struct Stream {
        uint32_t type, size, rva;
    };
    std::vector<Stream> list;
    json streamRows = json::array();
    for (uint32_t i = 0; i < std::min<uint32_t>(streams, 256); ++i) {
        Stream stream{};
        if (!ReadU32(source, directory + i * 12ull, stream.type) || !ReadU32(source, directory + i * 12ull + 4, stream.size) ||
            !ReadU32(source, directory + i * 12ull + 8, stream.rva))
            break;
        list.push_back(stream);
        const char* name = StreamName(stream.type);
        streamRows.push_back({{"type", name ? name : HexU64(stream.type)}, {"size", stream.size}});
    }
    data["streams"] = streamRows;
    auto find = [&list](uint32_t type) -> const Stream* {
        for (const auto& stream : list)
            if (stream.type == type) return &stream;
        return nullptr;
    };

    uint16_t architecture = 0xFFFF;
    if (const Stream* info = find(7); info && info->size >= 32) {
        uint16_t level = 0, revision = 0;
        uint8_t processors = 0, product = 0;
        uint32_t major = 0, minor = 0, build = 0, csd = 0;
        ReadU16(source, info->rva, architecture);
        ReadU16(source, info->rva + 2, level);
        ReadU16(source, info->rva + 4, revision);
        const auto small = source.ReadSome(info->rva + 6, 2);
        if (small.size() == 2) {
            processors = small[0];
            product = small[1];
        }
        ReadU32(source, info->rva + 8, major);
        ReadU32(source, info->rva + 12, minor);
        ReadU32(source, info->rva + 16, build);
        ReadU32(source, info->rva + 24, csd);
        data["system"] = {{"architecture", ArchitectureName(architecture)},
                          {"processors", processors},
                          {"os_version", std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(build)},
                          {"product_type", product == 1 ? "workstation" : product == 2 ? "domain_controller" : product == 3 ? "server" : "unknown"}};
        const std::string servicePack = Utf16(source, csd);
        if (!servicePack.empty()) data["system"]["service_pack"] = servicePack;
    }
    if (const Stream* misc = find(15); misc && misc->size >= 24) {
        uint32_t miscFlags = 0, pid = 0, created = 0;
        ReadU32(source, misc->rva + 4, miscFlags);
        ReadU32(source, misc->rva + 8, pid);
        ReadU32(source, misc->rva + 12, created);
        json process = json::object();
        if (miscFlags & 1) process["pid"] = pid;
        if (miscFlags & 2) process["created"] = created;
        if (!process.empty()) data["process"] = process;
    }

    std::vector<Module> modules;
    if (const Stream* stream = find(4)) {
        uint32_t count = 0;
        ReadU32(source, stream->rva, count);
        json rows = json::array();
        for (uint32_t i = 0; i < std::min<uint32_t>(count, 4096); ++i) {
            const uint64_t at = stream->rva + 4 + i * 108ull;
            Module module;
            uint32_t checksum = 0, moduleStamp = 0, nameRva = 0, cvSize = 0, cvRva = 0;
            if (!ReadU64(source, at, module.base) || !ReadU32(source, at + 8, module.size) ||
                !ReadU32(source, at + 12, checksum) || !ReadU32(source, at + 16, moduleStamp) ||
                !ReadU32(source, at + 20, nameRva))
                break;
            module.name = Utf16(source, nameRva);
            modules.push_back(module);
            if (rows.size() >= options.maxEntries) continue;
            json row = {{"name", module.name}, {"base", HexU64(module.base)}, {"size", module.size},
                        {"timestamp", moduleStamp}};
            uint32_t fixed = 0, fileMs = 0, fileLs = 0;
            if (ReadU32(source, at + 24, fixed) && fixed == 0xFEEF04BDu && ReadU32(source, at + 32, fileMs) &&
                ReadU32(source, at + 36, fileLs))
                row["file_version"] = std::to_string(fileMs >> 16) + "." + std::to_string(fileMs & 0xFFFF) + "." +
                                      std::to_string(fileLs >> 16) + "." + std::to_string(fileLs & 0xFFFF);
            uint32_t cvSignature = 0;
            if (ReadU32(source, at + 76, cvSize) && ReadU32(source, at + 80, cvRva) && cvSize >= 24 &&
                ReadU32(source, cvRva, cvSignature) && cvSignature == 0x53445352u) {
                const auto guid = source.ReadSome(cvRva + 4, 16);
                uint32_t age = 0;
                std::string pdb;
                if (guid.size() == 16 && ReadU32(source, cvRva + 20, age)) {
                    char text[40];
                    std::snprintf(text, sizeof(text), "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                                  guid[3], guid[2], guid[1], guid[0], guid[5], guid[4], guid[7], guid[6], guid[8], guid[9],
                                  guid[10], guid[11], guid[12], guid[13], guid[14], guid[15]);
                    source.ReadCString(cvRva + 24, std::min<uint32_t>(cvSize - 24, 512), pdb);
                    row["pdb"] = {{"guid", text}, {"age", age}, {"path", pdb}};
                }
            }
            rows.push_back(row);
        }
        data["modules"] = rows;
        if (count > rows.size()) data["modules_truncated"] = true;
    }

    if (const Stream* stream = find(6); stream && stream->size >= 168) {
        uint32_t thread = 0, code = 0, exceptionFlags = 0, parameters = 0, contextSize = 0, contextRva = 0;
        uint64_t address = 0;
        ReadU32(source, stream->rva, thread);
        ReadU32(source, stream->rva + 8, code);
        ReadU32(source, stream->rva + 12, exceptionFlags);
        ReadU64(source, stream->rva + 24, address);
        ReadU32(source, stream->rva + 32, parameters);
        const char* name = ExceptionName(code);
        json exception = {{"thread", thread}, {"code", HexU64(code)}, {"name", name ? name : "unknown"},
                          {"address", HexU64(address)}, {"continuable", (exceptionFlags & 1) == 0}};
        const std::string where = Locate(modules, address);
        if (!where.empty()) exception["module"] = where;
        json information = json::array();
        for (uint32_t i = 0; i < std::min<uint32_t>(parameters, 15); ++i) {
            uint64_t value = 0;
            if (ReadU64(source, stream->rva + 40 + i * 8ull, value)) information.push_back(HexU64(value));
        }
        exception["parameters"] = information;
        if ((code == 0xC0000005 || code == 0xC0000006) && information.size() >= 2) {
            const std::string kind = information[0] == "0x0" ? "read" : information[0] == "0x1" ? "write" : information[0] == "0x8" ? "execute" : "unknown";
            exception["access"] = {{"kind", kind}, {"target", information[1]}};
        }
        if (ReadU32(source, stream->rva + 160, contextSize) && ReadU32(source, stream->rva + 164, contextRva))
            exception["context"] = ContextRegisters(source, contextSize, contextRva, architecture, modules);
        data["exception"] = exception;
    }

    if (const Stream* stream = find(3)) {
        uint32_t count = 0;
        ReadU32(source, stream->rva, count);
        json rows = json::array();
        for (uint32_t i = 0; i < std::min<uint32_t>(count, 65536) && rows.size() < options.maxEntries; ++i) {
            const uint64_t at = stream->rva + 4 + i * 48ull;
            uint32_t id = 0, suspend = 0, contextSize = 0, contextRva = 0, stackSize = 0;
            uint64_t teb = 0, stackStart = 0;
            if (!ReadU32(source, at, id) || !ReadU32(source, at + 4, suspend) || !ReadU64(source, at + 16, teb) ||
                !ReadU64(source, at + 24, stackStart) || !ReadU32(source, at + 32, stackSize) ||
                !ReadU32(source, at + 40, contextSize) || !ReadU32(source, at + 44, contextRva))
                break;
            rows.push_back({{"id", id}, {"suspend_count", suspend}, {"teb", HexU64(teb)},
                            {"stack", {{"start", HexU64(stackStart)}, {"size", stackSize}}},
                            {"context", ContextRegisters(source, contextSize, contextRva, architecture, modules)}});
        }
        data["threads"] = rows;
    }

    uint64_t ranges = 0, bytes = 0;
    if (const Stream* stream = find(5)) {
        uint32_t count = 0;
        ReadU32(source, stream->rva, count);
        for (uint32_t i = 0; i < std::min<uint32_t>(count, 1u << 20); ++i) {
            uint32_t size = 0;
            if (!ReadU32(source, stream->rva + 4 + i * 16ull + 8, size)) break;
            ++ranges;
            bytes += size;
        }
    }
    if (const Stream* stream = find(9)) {
        uint64_t count = 0;
        ReadU64(source, stream->rva, count);
        for (uint64_t i = 0; i < std::min<uint64_t>(count, 1u << 20); ++i) {
            uint64_t size = 0;
            if (!ReadU64(source, stream->rva + 16 + i * 16ull + 8, size)) break;
            ++ranges;
            bytes += size;
        }
    }
    data["memory"] = {{"ranges", ranges}, {"bytes", bytes},
                      {"kind", find(9) ? "full (Memory64List)" : find(5) ? "partial (MemoryList)" : "none"}};
    for (uint32_t type : {10u, 11u}) {
        if (const Stream* stream = find(type)) {
            if (type == 11) {
                const auto raw = source.ReadSome(stream->rva, std::min<uint32_t>(stream->size, 2048));
                std::string text;
                for (size_t i = 0; i + 1 < raw.size(); i += 2)
                    if (raw[i] && raw[i] < 0x80) text += static_cast<char>(raw[i]);
                data["comment"] = text;
            } else {
                std::string text;
                source.ReadCString(stream->rva, std::min<uint32_t>(stream->size, 2048), text);
                data["comment"] = text;
            }
        }
    }
    return data;
}

}  // namespace cortex::artifacts
