#include "native_code.h"

#include "../analysis/cfg_builder.h"

#include <Zydis/Zydis.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <list>
#include <mutex>

namespace cortex::artifacts::native {
namespace {

constexpr uint64_t kMaxCodeBytes = 64ull << 20;
constexpr size_t kMaxFunctions = 200000;
constexpr size_t kMaxXrefs = 2000000;
constexpr size_t kMaxStrings = 100000;
constexpr uint32_t kExecute = 0x20000000, kCode = 0x20;

std::string Hex(uint64_t value) {
    char text[24];
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
    return text;
}

bool Executable(const PeSection& section) { return section.characteristics & (kExecute | kCode); }

// Bytes of the file at an RVA, at most `size`, never past the section's raw data.
std::vector<uint8_t> ReadRva(const ByteSource& source, const PeFile& pe, uint32_t rva, size_t size) {
    uint64_t offset = 0;
    uint32_t available = 0;
    if (!pe.RvaToOffset(rva, offset, &available)) return {};
    return source.ReadSome(offset, std::min<size_t>(size, available));
}

ZydisDecoder Decoder(bool x64) {
    ZydisDecoder decoder;
    if (x64) ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    else ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32);
    return decoder;
}

uint32_t ParseRva(const json& value) {
    if (!value.is_string()) return 0;
    try {
        return static_cast<uint32_t>(std::stoull(value.get<std::string>(), nullptr, 16));
    } catch (...) {
        return 0;
    }
}

bool Printable(uint8_t c) { return (c >= 0x20 && c < 0x7F) || c == '\t' || c == '\n' || c == '\r'; }

void ScanStrings(const ByteSource& source, const PeSection& section, CodeIndex& index) {
    const uint32_t length = std::min(section.rawSize, section.virtualSize ? section.virtualSize : section.rawSize);
    const auto bytes = source.ReadSome(section.rawOffset, std::min<uint32_t>(length, 64u << 20));
    auto add = [&](size_t at, std::string text, bool wide) {
        if (index.strings.size() >= kMaxStrings) {
            index.truncated = true;
            return;
        }
        if (text.size() > 256) text.resize(256);
        const uint32_t rva = section.virtualAddress + static_cast<uint32_t>(at);
        index.strings[rva] = std::move(text);
        index.stringIsWide[rva] = wide;
    };
    // ASCII, NUL-terminated, at least 5 characters.
    size_t start = 0;
    for (size_t i = 0; i <= bytes.size(); ++i) {
        if (i < bytes.size() && Printable(bytes[i])) continue;
        if (i < bytes.size() && bytes[i] == 0 && i - start >= 5)
            add(start, std::string(bytes.begin() + static_cast<std::ptrdiff_t>(start), bytes.begin() + static_cast<std::ptrdiff_t>(i)), false);
        start = i + 1;
    }
    // UTF-16LE (ASCII range), 2-aligned, at least 5 characters.
    for (size_t parity = 0; parity < 2; ++parity) {
        size_t run = parity;
        std::string text;
        for (size_t i = parity; i + 1 < bytes.size(); i += 2) {
            if (bytes[i + 1] == 0 && Printable(bytes[i])) {
                if (text.empty()) run = i;
                text.push_back(static_cast<char>(bytes[i]));
                continue;
            }
            if (bytes[i] == 0 && bytes[i + 1] == 0 && text.size() >= 5 && !index.strings.count(section.virtualAddress + static_cast<uint32_t>(run)))
                add(run, text, true);
            text.clear();
        }
    }
}

} // namespace

const Function* CodeIndex::At(uint32_t rva) const {
    const auto it = std::lower_bound(functions.begin(), functions.end(), rva,
                                     [](const Function& f, uint32_t value) { return f.rva < value; });
    return it != functions.end() && it->rva == rva ? &*it : nullptr;
}

const Function* CodeIndex::Containing(uint32_t rva) const {
    auto it = std::upper_bound(functions.begin(), functions.end(), rva,
                               [](uint32_t value, const Function& f) { return value < f.rva; });
    if (it == functions.begin()) return nullptr;
    --it;
    if (it->size && rva >= it->rva + it->size) return nullptr;
    return &*it;
}

bool BuildIndex(const ByteSource& source, const PeFile& pe, CodeIndex& index, std::string& error) {
    index = CodeIndex();
    if (pe.machine == 0x8664) index.x64 = true;
    else if (pe.machine != 0x14C) {
        error = "unsupported_machine:" + Hex(pe.machine);
        return false;
    }
    index.imageBase = pe.imageBase;
    const uint64_t imageEnd = pe.imageBase + pe.sizeOfImage;
    std::map<uint32_t, Function> starts;
    auto addStart = [&](uint32_t rva, const char* source_, const std::string& name = {}) {
        const PeSection* section = pe.SectionFor(rva);
        if (!section || !Executable(*section)) return;
        if (starts.size() >= kMaxFunctions && !starts.count(rva)) {
            index.truncated = true;
            return;
        }
        Function& function = starts[rva];
        function.rva = rva;
        function.sources.insert(source_);
        if (!name.empty() && function.exportName.empty()) function.exportName = name;
    };

    if (pe.entryRva) addStart(pe.entryRva, "entry");
    InspectOptions all;
    all.maxEntries = kMaxFunctions;
    bool truncated = false;
    if (pe.HasDirectory(kPeExport)) {
        const json exports = PeExports(source, pe, all, truncated);
        for (const auto& row : exports.value("functions", json::array()))
            if (!row.contains("forwarded_to")) addStart(ParseRva(row.value("rva", json())), "export", row.value("name", std::string()));
    }
    if (pe.HasDirectory(kPeTls)) {
        std::string ignored;
        const json data = PeProvider().Inspect(source, all, ignored);
        if (data.is_object() && data.contains("tls"))
            for (const auto& callback : data["tls"].value("callbacks", json::array()))
                addStart(ParseRva(callback.value("rva", json())), "tls");
    }
    // x64 exception directory: RUNTIME_FUNCTION {begin, end, unwind info}.
    std::map<uint32_t, uint32_t> pdataSize;
    if (index.x64 && pe.HasDirectory(kPeException)) {
        const auto& directory = pe.directories[kPeException];
        const auto table = ReadRva(source, pe, directory.rva, std::min<uint32_t>(directory.size, 64u << 20));
        for (size_t at = 0; at + 12 <= table.size(); at += 12) {
            uint32_t begin = 0, end = 0, unwind = 0;
            std::memcpy(&begin, &table[at], 4);
            std::memcpy(&end, &table[at + 4], 4);
            std::memcpy(&unwind, &table[at + 8], 4);
            if (!begin || end <= begin) continue;
            ++index.pdataEntries;
            const auto info = ReadRva(source, pe, unwind & ~1u, 1);
            if (!info.empty() && ((info[0] >> 3) & 0x4)) {  // UNW_FLAG_CHAININFO: a fragment of another function
                ++index.pdataChained;
                continue;
            }
            addStart(begin, "pdata");
            pdataSize[begin] = end - begin;
        }
    }

    // Linear sweep of the executable sections: direct calls name function
    // starts; every resolvable operand becomes a cross-reference.
    const ZydisDecoder decoder = Decoder(index.x64);
    for (const auto& section : pe.sections) {
        if (!Executable(section)) continue;
        const uint32_t length = std::min(section.rawSize, section.virtualSize ? section.virtualSize : section.rawSize);
        if (index.decodedBytes + length > kMaxCodeBytes) {
            index.truncated = true;
            index.warnings.push_back("code beyond " + std::to_string(kMaxCodeBytes >> 20) + " MiB not swept");
            break;
        }
        const auto bytes = source.ReadSome(section.rawOffset, length);
        index.decodedBytes += bytes.size();
        size_t offset = 0;
        bool afterRet = false;
        while (offset < bytes.size()) {
            const uint32_t rva = section.virtualAddress + static_cast<uint32_t>(offset);
            const uint64_t va = pe.imageBase + rva;
            if (afterRet && bytes[offset] != 0xCC && bytes[offset] != 0x90 && bytes[offset] != 0x00) {
                addStart(rva, "after_ret");
                afterRet = false;
            }
            ZydisDecodedInstruction instr;
            ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
            if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, bytes.data() + offset, bytes.size() - offset, &instr, operands))) {
                afterRet = false;
                ++offset;
                continue;
            }
            ++index.instructions;
            const auto category = instr.meta.category;
            auto addXref = [&](uint64_t targetVa, const char* type) {
                if (targetVa < pe.imageBase || targetVa >= imageEnd) return;
                if (index.xrefs.size() >= kMaxXrefs) {
                    index.truncated = true;
                    return;
                }
                index.xrefs.push_back({rva, static_cast<uint32_t>(targetVa - pe.imageBase), type});
            };
            const bool branch = category == ZYDIS_CATEGORY_CALL || category == ZYDIS_CATEGORY_UNCOND_BR ||
                                category == ZYDIS_CATEGORY_COND_BR;
            uint64_t target = 0;
            if (branch && analysis::ResolveDirectBranch(instr, operands, va, target)) {
                if (category == ZYDIS_CATEGORY_CALL) {
                    addXref(target, "call");
                    if (target >= pe.imageBase && target < imageEnd) addStart(static_cast<uint32_t>(target - pe.imageBase), "call_target");
                } else {
                    addXref(target, category == ZYDIS_CATEGORY_COND_BR ? "cond_jump" : "jump");
                }
            }
            for (uint8_t i = 0; i < instr.operand_count_visible; ++i) {
                const auto& operand = operands[i];
                if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY) {
                    uint64_t address = 0;
                    if (operand.mem.base == ZYDIS_REGISTER_RIP || operand.mem.base == ZYDIS_REGISTER_EIP) {
                        if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&instr, &operand, va, &address))) continue;
                    } else if (operand.mem.base == ZYDIS_REGISTER_NONE && operand.mem.disp.has_displacement && !index.x64) {
                        address = static_cast<uint32_t>(operand.mem.disp.value);  // absolute (or a table base with an index)
                    } else {
                        continue;
                    }
                    const char* type = operand.mem.type == ZYDIS_MEMOP_TYPE_AGEN || operand.mem.index != ZYDIS_REGISTER_NONE
                        ? "address"
                        : (operand.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) ? "write" : "read";
                    addXref(address, type);
                } else if (operand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && !branch) {
                    const uint64_t value = index.x64 ? operand.imm.value.u : static_cast<uint32_t>(operand.imm.value.u);
                    if (value >= pe.imageBase + pe.sizeOfHeaders && value < imageEnd) addXref(value, "address");
                }
            }
            afterRet = category == ZYDIS_CATEGORY_RET;
            offset += instr.length;
        }
    }
    if (starts.empty()) {
        error = "no_executable_code";
        return false;
    }

    // Sizes: pdata when known, else up to the next start within the section.
    index.functions.reserve(starts.size());
    for (auto& [rva, function] : starts) index.functions.push_back(std::move(function));
    for (size_t i = 0; i < index.functions.size(); ++i) {
        Function& function = index.functions[i];
        const auto pdata = pdataSize.find(function.rva);
        if (pdata != pdataSize.end()) {
            function.size = pdata->second;
            function.sizeFromPdata = true;
            continue;
        }
        const PeSection* section = pe.SectionFor(function.rva);
        uint32_t end = section ? section->virtualAddress + std::max(section->virtualSize, section->rawSize) : function.rva;
        if (i + 1 < index.functions.size() && index.functions[i + 1].rva < end) end = index.functions[i + 1].rva;
        function.size = end > function.rva ? end - function.rva : 0;
    }
    std::sort(index.xrefs.begin(), index.xrefs.end(), [](const Xref& a, const Xref& b) {
        return a.to != b.to ? a.to < b.to : a.from < b.from;
    });
    for (const auto& section : pe.sections)
        if (!Executable(section) && section.rawSize) ScanStrings(source, section, index);
    return true;
}

std::shared_ptr<const CodeIndex> IndexFor(const ByteSource& source, const PeFile& pe, const std::string& sha256,
                                          std::string& error) {
    static std::mutex mutex;
    static std::list<std::pair<std::string, std::shared_ptr<const CodeIndex>>> cache;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto it = cache.begin(); it != cache.end(); ++it)
            if (!sha256.empty() && it->first == sha256) {
                cache.splice(cache.begin(), cache, it);
                return cache.front().second;
            }
    }
    auto index = std::make_shared<CodeIndex>();
    if (!BuildIndex(source, pe, *index, error)) return {};
    std::lock_guard<std::mutex> lock(mutex);
    if (!sha256.empty()) {
        cache.emplace_front(sha256, index);
        if (cache.size() > 4) cache.pop_back();
    }
    return index;
}

bool ResolveLocation(const CodeIndex& index, const std::string& query, const std::map<std::string, uint32_t>& names,
                     uint32_t& rva) {
    auto hex = [](const std::string& text, uint64_t& value) {
        if (text.empty() || text.size() > 18) return false;
        size_t used = 0;
        try {
            value = std::stoull(text, &used, 16);
        } catch (...) {
            return false;
        }
        return used == text.size();
    };
    uint64_t value = 0;
    if (query.rfind("va:", 0) == 0 && hex(query.substr(3), value) && value >= index.imageBase) {
        rva = static_cast<uint32_t>(value - index.imageBase);
        return true;
    }
    if (query.rfind("rva:", 0) == 0 && hex(query.substr(4), value)) {
        rva = static_cast<uint32_t>(value);
        return true;
    }
    if (query.rfind("sub_", 0) == 0 && hex(query.substr(4), value)) {
        rva = static_cast<uint32_t>(value);
        return true;
    }
    if (query.rfind("0x", 0) == 0 && hex(query.substr(2), value)) {
        rva = static_cast<uint32_t>(value);
        return true;
    }
    const auto named = names.find(query);
    if (named != names.end()) {
        rva = named->second;
        return true;
    }
    for (const auto& function : index.functions)
        if (function.exportName == query) {
            rva = function.rva;
            return true;
        }
    return false;
}

std::string FunctionName(const Function& function, const std::map<uint32_t, std::string>& annotations) {
    const auto annotated = annotations.find(function.rva);
    if (annotated != annotations.end()) return annotated->second;
    if (!function.exportName.empty()) return function.exportName;
    char text[24];
    std::snprintf(text, sizeof(text), "sub_%X", function.rva);
    return text;
}

json FunctionsJson(const CodeIndex& index, const std::string& filter, size_t maxEntries,
                   const std::map<uint32_t, std::string>& annotations) {
    json rows = json::array();
    std::map<std::string, size_t> bySource;
    size_t matched = 0;
    for (const auto& function : index.functions) {
        for (const auto& source : function.sources) bySource[source]++;
        const std::string name = FunctionName(function, annotations);
        if (!filter.empty() && name.find(filter) == std::string::npos) continue;
        ++matched;
        if (rows.size() >= maxEntries) continue;
        rows.push_back({{"rva", Hex(function.rva)},
                        {"name", name},
                        {"sources", std::vector<std::string>(function.sources.begin(), function.sources.end())},
                        {"size", function.size},
                        {"size_source", function.sizeFromPdata ? "pdata" : "next_start"},
                        {"annotated", annotations.count(function.rva) > 0}});
    }
    return {{"ok", true},
            {"architecture", index.x64 ? "x64" : "x86"},
            {"image_base", Hex(index.imageBase)},
            {"functions", rows},
            {"matched", matched},
            {"total", index.functions.size()},
            {"by_source", bySource},
            {"pdata_entries", index.pdataEntries},
            {"pdata_chained", index.pdataChained},
            {"instructions", index.instructions},
            {"xrefs", index.xrefs.size()},
            {"strings", index.strings.size()},
            {"truncated", index.truncated || matched > rows.size()},
            {"note", "starts by provenance; a linear sweep can decode data as code, so after_ret-only entries are heuristic"}};
}

json XrefsJson(const CodeIndex& index, uint32_t target, size_t maxEntries,
               const std::map<uint32_t, std::string>& annotations) {
    const auto range = std::equal_range(index.xrefs.begin(), index.xrefs.end(), Xref{0, target, {}},
                                        [](const Xref& a, const Xref& b) { return a.to < b.to; });
    json rows = json::array();
    size_t total = 0;
    for (auto it = range.first; it != range.second; ++it, ++total) {
        if (rows.size() >= maxEntries) continue;
        json row = {{"from", Hex(it->from)}, {"type", it->type}};
        if (const Function* function = index.Containing(it->from))
            row["function"] = {{"rva", Hex(function->rva)}, {"name", FunctionName(*function, annotations)}};
        rows.push_back(std::move(row));
    }
    json result = {{"ok", true}, {"target", Hex(target)}, {"references", rows}, {"total", total},
                   {"truncated", total > rows.size()},
                   {"note", "direct branches and statically resolvable operands from a linear sweep; computed and "
                            "register-relative references are not seen"}};
    if (const Function* function = index.At(target)) result["target_function"] = FunctionName(*function, annotations);
    const auto string = index.strings.find(target);
    if (string != index.strings.end()) result["target_string"] = string->second;
    return result;
}

json StringsJson(const CodeIndex& index, const std::string& filter, size_t maxEntries) {
    json rows = json::array();
    size_t matched = 0;
    for (const auto& [rva, text] : index.strings) {
        if (!filter.empty() && text.find(filter) == std::string::npos) continue;
        ++matched;
        if (rows.size() >= maxEntries) continue;
        const auto range = std::equal_range(index.xrefs.begin(), index.xrefs.end(), Xref{0, rva, {}},
                                            [](const Xref& a, const Xref& b) { return a.to < b.to; });
        json refs = json::array();
        for (auto it = range.first; it != range.second && refs.size() < 32; ++it) {
            json ref = {{"from", Hex(it->from)}};
            if (const Function* function = index.Containing(it->from)) ref["function"] = Hex(function->rva);
            refs.push_back(std::move(ref));
        }
        rows.push_back({{"rva", Hex(rva)}, {"text", text}, {"wide", index.stringIsWide.at(rva)}, {"references", refs}});
    }
    return {{"ok", true}, {"strings", rows}, {"matched", matched}, {"truncated", matched > rows.size()}};
}

json CfgJson(const ByteSource& source, const PeFile& pe, const CodeIndex& index, uint32_t rva,
             const std::set<uint32_t>& covered, size_t maxBlocks, const std::map<uint32_t, std::string>& annotations) {
    const PeSection* section = pe.SectionFor(rva);
    if (!section || !Executable(*section)) return {{"ok", false}, {"error", "not_in_executable_section"}};
    const ZydisDecoder decoder = Decoder(index.x64);
    // Same algorithm as the runtime's analysis_cfg, over the file's bytes;
    // addresses are RVAs.
    auto fetch = [&](uintptr_t address, size_t maxBytes) {
        const PeSection* at = pe.SectionFor(static_cast<uint32_t>(address));
        if (!at || !Executable(*at)) return std::vector<uint8_t>();
        return ReadRva(source, pe, static_cast<uint32_t>(address), maxBytes);
    };
    const auto cfg = analysis::BuildCfgFrom(rva, decoder, fetch, maxBlocks, 300);

    ZydisFormatter formatter;
    ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);
    ZydisFormatterSetProperty(&formatter, ZYDIS_FORMATTER_PROP_ADDR_PADDING_ABSOLUTE, ZYDIS_PADDING_DISABLED);
    json blocks = json::array();
    size_t coveredBlocks = 0;
    for (const auto& block : cfg.blocks) {
        const auto bytes = ReadRva(source, pe, static_cast<uint32_t>(block.start), block.end - block.start);
        json instructions = json::array();
        size_t offset = 0;
        bool blockCovered = false;
        while (offset < bytes.size()) {
            ZydisDecodedInstruction instr;
            ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
            if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, bytes.data() + offset, bytes.size() - offset, &instr, operands)))
                break;
            const uint32_t at = static_cast<uint32_t>(block.start + offset);
            char text[256];
            ZydisFormatterFormatInstruction(&formatter, &instr, operands, instr.operand_count_visible, text, sizeof(text), at,
                                            nullptr);
            json row = {{"rva", Hex(at)}, {"text", text}};
            uint64_t target = 0;
            if (instr.meta.category == ZYDIS_CATEGORY_CALL && analysis::ResolveDirectBranch(instr, operands, at, target))
                if (const Function* function = index.At(static_cast<uint32_t>(target)))
                    row["call"] = FunctionName(*function, annotations);
            if (covered.count(at)) {
                row["covered"] = true;
                blockCovered = true;
            }
            instructions.push_back(std::move(row));
            offset += instr.length;
        }
        json row = {{"start", Hex(block.start)}, {"end", Hex(block.end)}, {"instructions", instructions}};
        if (!covered.empty()) {
            row["covered"] = blockCovered;
            if (blockCovered) ++coveredBlocks;
        }
        blocks.push_back(std::move(row));
    }
    json edges = json::array();
    for (const auto& edge : cfg.edges)
        edges.push_back({{"from", Hex(edge.from)}, {"to", edge.to ? json(Hex(edge.to)) : json(nullptr)}, {"type", edge.type}});
    json result = {{"ok", true}, {"function", Hex(rva)}, {"blocks", blocks}, {"edges", edges}, {"truncated", cfg.truncated},
                   {"note", "addresses are RVAs; recursive descent from the start, indirect jumps are not followed"}};
    if (const Function* function = index.At(rva)) result["name"] = FunctionName(*function, annotations);
    if (!covered.empty()) result["coverage"] = {{"blocks_covered", coveredBlocks}, {"blocks", cfg.blocks.size()},
                                                {"source", "trace addresses supplied by the caller"}};
    return result;
}

} // namespace cortex::artifacts::native
