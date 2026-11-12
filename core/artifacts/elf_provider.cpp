#include "elf_provider.h"
#include "native_more_provider.h"
#include "bounded_reader.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <set>

// System V ELF gABI: https://gabi.xinuos.com/elf/
// GNU tags are interpreted as file metadata, never as a guarantee that all
// functions or the runtime loader enforce a particular mitigation.
namespace cortex::artifacts {
namespace {
using bounded::Error;
using bounded::View;
constexpr uint64_t kByteBudget = 64ull << 20;
constexpr uint32_t kTableLimit = 65536;
constexpr uint32_t kRecordLimit = 1000000;
struct Section {
    uint32_t nameIndex = 0, type = 0, link = 0, info = 0;
    uint64_t flags = 0, address = 0, offset = 0, size = 0, align = 0, entry = 0;
    std::string name;
};
struct Segment {
    uint32_t type = 0, flags = 0;
    uint64_t offset = 0, address = 0, fileSize = 0, memorySize = 0, align = 0;
};
const char* Machine(uint16_t value) {
    switch (value) {
        case 3: return "x86"; case 62: return "x64"; case 40: return "arm";
        case 183: return "arm64"; case 8: return "mips"; case 20: return "ppc";
        case 21: return "ppc64"; case 243: return "riscv"; case 247: return "bpf";
        default: return "unknown";
    }
}
const char* SectionType(uint32_t type) {
    switch (type) {
        case 0: return "null"; case 1: return "progbits"; case 2: return "symtab";
        case 3: return "strtab"; case 4: return "rela"; case 6: return "dynamic";
        case 7: return "note"; case 8: return "nobits"; case 9: return "rel";
        case 11: return "dynsym"; case 14: return "init_array"; case 15: return "fini_array";
        case 18: return "symtab_shndx"; case 19: return "relr"; default: return "other";
    }
}
const char* SegmentType(uint32_t type) {
    switch (type) {
        case 0: return "null"; case 1: return "load"; case 2: return "dynamic";
        case 3: return "interp"; case 4: return "note"; case 6: return "phdr";
        case 7: return "tls"; case 0x6474E551: return "gnu_stack";
        case 0x6474E552: return "gnu_relro"; case 0x6474E553: return "gnu_property";
        default: return "other";
    }
}

class Elf {
public:
    Elf(const ByteSource& source, const InspectOptions& options) : source_(source), limit_(std::min<size_t>(options.maxEntries, 65536)) {
        const auto initial = Load(0, std::min<uint64_t>(64, source.Size()));
        const View h(initial);
        if (!h.Has(0, 16) || std::memcmp(h.Data(0, 4), "\177ELF", 4)) throw Error("not_elf");
        if (h.U8(4) != 1 && h.U8(4) != 2) throw Error("elf_class_unsupported");
        if (h.U8(5) != 1 && h.U8(5) != 2) throw Error("elf_byte_order_unsupported");
        wide_ = h.U8(4) == 2; little_ = h.U8(5) == 1;
        if (h.U8(6) != 1 || h.U32(20, little_) != 1) throw Error("elf_version_unsupported");
        type_ = h.U16(16, little_); machine_ = h.U16(18, little_);
        entry_ = wide_ ? h.U64(24, little_) : h.U32(24, little_);
        phoff_ = wide_ ? h.U64(32, little_) : h.U32(28, little_);
        shoff_ = wide_ ? h.U64(40, little_) : h.U32(32, little_);
        const uint32_t flags = h.U32(wide_ ? 48 : 36, little_);
        const uint16_t ehsize = h.U16(wide_ ? 52 : 40, little_);
        phstride_ = h.U16(wide_ ? 54 : 42, little_);
        uint64_t phcount = h.U16(wide_ ? 56 : 44, little_);
        shstride_ = h.U16(wide_ ? 58 : 46, little_);
        uint64_t shcount = h.U16(wide_ ? 60 : 48, little_);
        uint64_t shstrings = h.U16(wide_ ? 62 : 50, little_);
        if (ehsize < (wide_ ? 64 : 52) || ehsize > source.Size()) throw Error("elf_header_size_invalid");
        if (shoff_) {
            if (shstride_ < (wide_ ? 64 : 40) || shstride_ > 4096) throw Error("elf_section_stride_invalid");
            const Section zero = ReadSection(0);
            if (!shcount) shcount = zero.size;
            if (shstrings == 0xFFFF) shstrings = zero.link;
            if (phcount == 0xFFFF) phcount = zero.info;
        } else if (shcount || shstrings || phcount == 0xFFFF) throw Error("elf_missing_extended_section_header");
        if (shcount > kTableLimit || phcount > kTableLimit) throw Error("elf_table_count_limit");
        if (shcount && !bounded::Range(source.Size(), shoff_, shcount * shstride_)) throw Error("elf_section_table_outside_file");
        if (phcount && (!phoff_ || phstride_ < (wide_ ? 56 : 32) || phstride_ > 4096 ||
                       !bounded::Range(source.Size(), phoff_, phcount * phstride_))) throw Error("elf_program_table_invalid");
        for (uint32_t i = 0; i < shcount; ++i) {
            auto s = ReadSection(i);
            if (i && s.type != 0 && s.type != 8 && !bounded::Range(source.Size(), s.offset, s.size))
                throw Error("elf_section_outside_file:" + std::to_string(i));
            sections_.push_back(std::move(s));
        }
        if (shstrings) {
            if (shstrings >= sections_.size() || sections_[shstrings].type != 3) throw Error("elf_section_names_invalid");
            const View names = SectionBytes(static_cast<uint32_t>(shstrings));
            for (auto& section : sections_) {
                try { section.name = names.CString(section.nameIndex); }
                catch (const Error&) { Warn("section_name_invalid"); }
            }
        }
        for (uint32_t i = 0; i < phcount; ++i) segments_.push_back(ReadSegment(i));
        const char* kind = type_ == 1 ? "relocatable" : type_ == 2 ? "executable" : type_ == 3 ? "shared_object" : type_ == 4 ? "core" : "other";
        result_ = {{"format", "elf"}, {"kind", kind}, {"class", wide_ ? 64 : 32},
                   {"byte_order", little_ ? "little" : "big"}, {"machine", Machine(machine_)},
                   {"machine_id", machine_}, {"os_abi", h.U8(7)}, {"abi_version", h.U8(8)},
                   {"type_id", type_}, {"flags", HexU64(flags)}, {"entry", HexU64(entry_)},
                   {"section_count", shcount}, {"segment_count", phcount},
                   {"sections", json::array()}, {"segments", json::array()},
                   {"symbols", json::array()}, {"relocations", json::array()}, {"notes", json::array()}};
    }

    json Inspect() {
        bool relro = false, interpreter = false;
        json nxStack = nullptr;
        for (size_t i = 0; i < segments_.size(); ++i) {
            const auto& p = segments_[i];
            Add(result_["segments"], {{"index", i}, {"type", SegmentType(p.type)}, {"type_id", p.type},
                {"offset", HexU64(p.offset)}, {"virtual_address", HexU64(p.address)}, {"file_size", p.fileSize},
                {"memory_size", p.memorySize}, {"flags", p.flags}, {"read", (p.flags & 4) != 0},
                {"write", (p.flags & 2) != 0}, {"execute", (p.flags & 1) != 0}, {"alignment", p.align}});
            if (p.type == 0x6474E551) nxStack = (p.flags & 1) == 0;
            if (p.type == 0x6474E552) relro = true;
            if (p.type == 3) {
                const auto bytes = Load(p.offset, p.fileSize);
                result_["interpreter"] = View(bytes).CString(0); interpreter = true;
            }
        }
        bool sawDynamic = false;
        for (const auto& p : segments_) if (p.type == 2) {
            if (sawDynamic) { Warn("multiple_dynamic_segments"); break; }
            Dynamic(p.offset, p.fileSize, nullptr); sawDynamic = true;
        }
        for (uint32_t i = 0; i < sections_.size(); ++i) {
            const auto& s = sections_[i];
            Add(result_["sections"], {{"index", i}, {"name", s.name}, {"type", SectionType(s.type)},
                {"type_id", s.type}, {"flags", HexU64(s.flags)}, {"address", HexU64(s.address)},
                {"offset", HexU64(s.offset)}, {"size", s.size}, {"link", s.link}, {"info", s.info},
                {"alignment", s.align}, {"entry_size", s.entry}, {"compressed", (s.flags & 0x800) != 0}});
            if (s.flags & 0x800) { Warn("compressed_section_not_decoded:" + s.name); continue; }
            try {
                if (s.type == 2 || s.type == 11) Symbols(i);
                if (s.type == 4 || s.type == 9) Relocations(i);
                if (s.type == 19) Warn("packed_RELR_entries_not_decoded");
                if (s.type == 6 && !sawDynamic) { Dynamic(s.offset, s.size, &s); sawDynamic = true; }
                if (s.type == 7) Notes(s.offset, s.size);
            } catch (const Error& ex) { Warn("section_" + std::to_string(i) + ":" + ex.what()); }
        }
        for (const auto& p : segments_) if (p.type == 4 || p.type == 0x6474E553) {
            try { Notes(p.offset, p.fileSize); } catch (const Error& ex) { Warn(ex.what()); }
        }
        result_["hardening"] = {{"nx_stack", nxStack}, {"gnu_relro_segment", relro},
            {"bind_now", bindNow_}, {"relro", relro ? (bindNow_ && dynamicComplete_ ? "full_metadata" : "partial_metadata") : "not_observed"},
            {"pie", type_ == 3 && (pieFlag_ || interpreter) ? "indicated" : type_ == 3 ? "shared_object_or_static_pie" : "not_indicated"},
            {"stack_canary_reference", canaryReference_},
            {"note", "file metadata and symbol evidence only; absent references do not prove absence of protection"}};
        if (sections_.empty()) result_["symbols_note"] = "no_section_table; dynamic-symbol reconstruction from hash tables is not implemented";
        result_["truncated"] = truncated_;
        result_["limits"] = {{"max_entries_per_list", limit_}, {"max_read_bytes", kByteBudget}, {"max_records", kRecordLimit}};
        if (!warnings_.empty()) result_["warnings"] = warnings_;
        if (warningsTruncated_) result_["warnings_truncated"] = true;
        return result_;
    }
private:
    std::vector<uint8_t> Load(uint64_t at, uint64_t size) {
        if (size > remaining_) throw Error("elf_read_budget_exceeded");
        remaining_ -= size;
        return bounded::Read(source_, at, size);
    }
    void Work() { if (++work_ > kRecordLimit) throw Error("elf_record_budget_exceeded"); }
    void Warn(const std::string& text) {
        if (warnings_.size() < 128) warnings_.push_back(text); else warningsTruncated_ = true;
    }
    bool Add(json& array, json value) {
        if (array.size() >= limit_) { truncated_ = true; return false; }
        array.push_back(std::move(value)); return true;
    }
    Section ReadSection(uint32_t index) {
        const auto bytes = Load(shoff_ + uint64_t(index) * shstride_, wide_ ? 64 : 40);
        const View v(bytes); Section s;
        s.nameIndex = v.U32(0, little_); s.type = v.U32(4, little_);
        s.flags = wide_ ? v.U64(8, little_) : v.U32(8, little_);
        s.address = wide_ ? v.U64(16, little_) : v.U32(12, little_);
        s.offset = wide_ ? v.U64(24, little_) : v.U32(16, little_);
        s.size = wide_ ? v.U64(32, little_) : v.U32(20, little_);
        s.link = v.U32(wide_ ? 40 : 24, little_); s.info = v.U32(wide_ ? 44 : 28, little_);
        s.align = wide_ ? v.U64(48, little_) : v.U32(32, little_);
        s.entry = wide_ ? v.U64(56, little_) : v.U32(36, little_);
        return s;
    }
    Segment ReadSegment(uint32_t index) {
        const auto bytes = Load(phoff_ + uint64_t(index) * phstride_, wide_ ? 56 : 32);
        const View v(bytes); Segment p;
        p.type = v.U32(0, little_); p.flags = v.U32(wide_ ? 4 : 24, little_);
        p.offset = wide_ ? v.U64(8, little_) : v.U32(4, little_);
        p.address = wide_ ? v.U64(16, little_) : v.U32(8, little_);
        p.fileSize = wide_ ? v.U64(32, little_) : v.U32(16, little_);
        p.memorySize = wide_ ? v.U64(40, little_) : v.U32(20, little_);
        p.align = wide_ ? v.U64(48, little_) : v.U32(28, little_);
        if (p.type && !bounded::Range(source_.Size(), p.offset, p.fileSize)) throw Error("elf_segment_outside_file");
        if (p.type == 1 && p.fileSize > p.memorySize) throw Error("elf_load_filesz_exceeds_memsz");
        if (p.memorySize > std::numeric_limits<uint64_t>::max() - p.address) throw Error("elf_virtual_range_overflow");
        if (p.align > 1 && ((p.align & (p.align - 1)) || (p.type == 1 && p.offset % p.align != p.address % p.align)))
            Warn("segment_alignment_invalid");
        return p;
    }
    View SectionBytes(uint32_t index) {
        if (index >= sections_.size()) throw Error("elf_section_index_invalid");
        const auto found = cache_.find(index);
        if (found != cache_.end()) return View(found->second);
        const auto& s = sections_[index];
        if (s.type == 8 || (s.flags & 0x800)) throw Error("elf_section_has_no_readable_payload");
        auto bytes = Load(s.offset, s.size);
        return View(cache_.emplace(index, std::move(bytes)).first->second);
    }
    std::string SymbolName(uint32_t table, uint64_t index) {
        if (table >= sections_.size()) throw Error("elf_symbol_table_index_invalid");
        const auto& s = sections_[table];
        if ((s.type != 2 && s.type != 11) || s.entry < (wide_ ? 24 : 16) || index >= s.size / s.entry)
            throw Error("elf_symbol_index_invalid");
        const View syms = SectionBytes(table);
        if (s.link >= sections_.size() || sections_[s.link].type != 3) throw Error("elf_symbol_string_table_invalid");
        return SectionBytes(s.link).CString(syms.U32(index * s.entry, little_));
    }
    void Symbols(uint32_t table) {
        const auto& s = sections_[table];
        if (s.entry < (wide_ ? 24 : 16) || s.entry > 4096 || s.size % s.entry) throw Error("elf_symbol_stride_invalid");
        if (s.link >= sections_.size() || sections_[s.link].type != 3) throw Error("elf_symbol_string_table_invalid");
        const View symbols = SectionBytes(table), strings = SectionBytes(s.link);
        View ext;
        for (uint32_t n = 0; n < sections_.size(); ++n)
            if (sections_[n].type == 18 && sections_[n].link == table) { ext = SectionBytes(n); break; }
        for (uint64_t i = 0; i < s.size / s.entry; ++i) {
            Work();
            if (result_["symbols"].size() >= limit_) { truncated_ = true; break; }
            const View v = symbols.Slice(i * s.entry, s.entry);
            const uint32_t name = v.U32(0, little_);
            const uint8_t info = v.U8(wide_ ? 4 : 12), other = v.U8(wide_ ? 5 : 13);
            uint32_t section = v.U16(wide_ ? 6 : 14, little_);
            if (section == 0xFFFF) {
                if (!ext.Has(i * 4, 4)) throw Error("elf_extended_symbol_index_missing");
                section = ext.U32(i * 4, little_);
            }
            const uint64_t value = wide_ ? v.U64(8, little_) : v.U32(4, little_);
            const uint64_t size = wide_ ? v.U64(16, little_) : v.U32(8, little_);
            const std::string text = strings.CString(name);
            if (text == "__stack_chk_fail" || text == "__stack_chk_guard") canaryReference_ = true;
            Add(result_["symbols"], {{"table", table}, {"index", i}, {"name", text},
                {"value", HexU64(value)}, {"size", size}, {"binding", info >> 4}, {"type", info & 15},
                {"visibility", other & 3}, {"section", section}, {"undefined", section == 0},
                {"value_kind", type_ == 1 ? "section_relative" : "virtual_address"}});
        }
    }
    void Relocations(uint32_t table) {
        const auto& s = sections_[table];
        const bool addend = s.type == 4;
        const uint64_t minimum = wide_ ? (addend ? 24 : 16) : (addend ? 12 : 8);
        if (s.entry < minimum || s.entry > 4096 || s.size % s.entry) throw Error("elf_relocation_stride_invalid");
        const View data = SectionBytes(table);
        for (uint64_t i = 0; i < s.size / s.entry; ++i) {
            Work();
            if (result_["relocations"].size() >= limit_) { truncated_ = true; break; }
            const View v = data.Slice(i * s.entry, s.entry);
            const uint64_t offset = wide_ ? v.U64(0, little_) : v.U32(0, little_);
            const uint64_t info = wide_ ? v.U64(8, little_) : v.U32(4, little_);
            const uint64_t symbol = wide_ ? info >> 32 : info >> 8;
            json row = {{"table", table}, {"offset", HexU64(offset)}, {"symbol_index", symbol},
                        {"type", wide_ ? info & 0xFFFFFFFFull : info & 255}, {"target_section", s.info}};
            if (addend) row["addend"] = wide_ ? static_cast<int64_t>(v.U64(16, little_)) : static_cast<int32_t>(v.U32(8, little_));
            if (symbol) {
                try { row["symbol"] = SymbolName(s.link, symbol); }
                catch (const Error& ex) { row["symbol_error"] = ex.what(); }
            }
            Add(result_["relocations"], std::move(row));
        }
    }
    uint64_t VirtualToFile(uint64_t address, uint64_t size) const {
        bool found = false; uint64_t offset = 0;
        for (const auto& p : segments_) if (p.type == 1 && address >= p.address &&
                bounded::Range(p.fileSize, address - p.address, size)) {
            const uint64_t current = p.offset + (address - p.address);
            if (found && offset != current) throw Error("elf_virtual_mapping_ambiguous");
            offset = current; found = true;
        }
        if (!found) throw Error("elf_virtual_range_not_file_backed");
        return offset;
    }
    void Dynamic(uint64_t offset, uint64_t size, const Section* section) {
        const auto bytes = Load(offset, size); const View v(bytes);
        const uint64_t stride = wide_ ? 16 : 8;
        if (size % stride) throw Error("elf_dynamic_size_invalid");
        std::vector<std::pair<uint64_t, uint64_t>> values;
        uint64_t strAddress = 0, strSize = 0;
        for (uint64_t at = 0; at < size; at += stride) {
            Work();
            const uint64_t tag = wide_ ? v.U64(at, little_) : v.U32(at, little_);
            const uint64_t value = wide_ ? v.U64(at + 8, little_) : v.U32(at + 4, little_);
            if (!tag) { dynamicComplete_ = true; break; }
            if (values.size() >= kTableLimit) throw Error("elf_dynamic_entry_limit");
            values.emplace_back(tag, value);
            if (tag == 5) strAddress = value;
            if (tag == 10) strSize = value;
            if (tag == 24 || (tag == 30 && (value & 8)) || (tag == 0x6FFFFFFB && (value & 1))) bindNow_ = true;
            if (tag == 0x6FFFFFFB && (value & 0x08000000)) pieFlag_ = true;
        }
        if (!dynamicComplete_) Warn("dynamic_table_without_DT_NULL");
        std::vector<uint8_t> strings;
        View names;
        if (section && section->link < sections_.size() && sections_[section->link].type == 3)
            names = SectionBytes(section->link);
        else if (strAddress && strSize) { strings = Load(VirtualToFile(strAddress, strSize), strSize); names = View(strings); }
        result_["dynamic"] = json::array(); result_["needed"] = json::array();
        for (const auto& [tag, value] : values) {
            json row = {{"tag", HexU64(tag)}, {"value", HexU64(value)}};
            if (tag == 1 || tag == 14 || tag == 15 || tag == 29) {
                try {
                    const std::string name = names.CString(value); row["string"] = name;
                    if (tag == 1) Add(result_["needed"], name);
                    else result_[tag == 14 ? "soname" : tag == 15 ? "rpath" : "runpath"] = name;
                } catch (const Error& ex) { row["error"] = ex.what(); }
            }
            Add(result_["dynamic"], std::move(row));
        }
    }
    void Notes(uint64_t offset, uint64_t size) {
        if (!seenNotes_.insert({offset, size}).second || !size) return;
        const auto data = Load(offset, size); const View v(data);
        for (uint64_t at = 0; at < size;) {
            Work();
            if (result_["notes"].size() >= limit_) { truncated_ = true; break; }
            const uint32_t namesz = v.U32(at, little_), descsz = v.U32(at + 4, little_), type = v.U32(at + 8, little_);
            if (namesz > 4096) throw Error("elf_note_name_limit");
            const uint64_t nameAt = at + 12, descAt = nameAt + bounded::Align4(namesz);
            const View desc = v.Slice(descAt, descsz);
            const uint8_t* nameBytes = v.Data(nameAt, namesz);
            size_t nameLength = namesz;
            while (nameLength && nameBytes[nameLength - 1] == 0) --nameLength;
            const std::string name = bounded::Utf8(nameBytes, nameLength);
            json row = {{"name", name}, {"type", type}, {"descriptor_bytes", descsz}};
            static const char* digits = "0123456789abcdef";
            std::string hex;
            for (uint64_t i = 0; i < std::min<uint64_t>(descsz, 256); ++i) {
                const uint8_t b = desc.U8(i); hex += digits[b >> 4]; hex += digits[b & 15];
            }
            row["descriptor_hex"] = hex; row["descriptor_truncated"] = descsz > 256;
            if (name == "GNU" && type == 3 && descsz <= 256) row["build_id"] = hex;
            if (name == "GNU" && type == 1 && descsz >= 16)
                row["abi"] = {{"os", desc.U32(0, little_)}, {"major", desc.U32(4, little_)},
                              {"minor", desc.U32(8, little_)}, {"patch", desc.U32(12, little_)}};
            Add(result_["notes"], std::move(row));
            at = descAt + bounded::Align4(descsz);
            if (at > size) throw Error("elf_note_padding_invalid");
        }
    }
    const ByteSource& source_;
    size_t limit_;
    bool wide_ = false, little_ = true, truncated_ = false, warningsTruncated_ = false;
    bool bindNow_ = false, pieFlag_ = false, canaryReference_ = false, dynamicComplete_ = false;
    uint16_t type_ = 0, machine_ = 0, phstride_ = 0, shstride_ = 0;
    uint64_t entry_ = 0, phoff_ = 0, shoff_ = 0, remaining_ = kByteBudget;
    uint32_t work_ = 0;
    std::vector<Section> sections_;
    std::vector<Segment> segments_;
    std::map<uint32_t, std::vector<uint8_t>> cache_;
    std::set<std::pair<uint64_t, uint64_t>> seenNotes_;
    std::vector<std::string> warnings_;
    json result_;
};
} // namespace

std::vector<Capability> ElfProvider::Capabilities() const {
    return {{"static", CapabilityStatus::Experimental, "tests/fixtures/elf-native; cortex_artifacts_tests",
             "ELF32/64 LE/BE, extended counts, sections/segments, symbol tables, REL/RELA, notes/build IDs, dynamic dependencies and hardening metadata"},
            {"decompile", CapabilityStatus::Unavailable, "", "no built-in decompiler; optional Ghidra provider supports ELF"},
            {"live", CapabilityStatus::Unavailable, "", "static file reader; Linux process backend is separate"}};
}
int ElfProvider::Detect(const ByteSource& source) const {
    uint8_t magic[4]; return source.Read(0, magic, 4) && !std::memcmp(magic, "\177ELF", 4) ? 80 : 0;
}
json ElfProvider::Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const {
    try {
        json result = Elf(source, options).Inspect();
        const json go = GoBinaryInfo(source);
        if (!go.is_null()) result["go"] = go;
        return result;
    }
    catch (const Error& ex) { error = ex.what(); return nullptr; }
}
} // namespace cortex::artifacts
