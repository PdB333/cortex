#include "data_provider.h"
#include "bounded_reader.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <set>

namespace cortex::artifacts {
namespace {
using bounded::Error;
using bounded::View;

uint32_t Swap32(uint32_t x) { return (x >> 24) | ((x >> 8) & 0xFF00) | ((x << 8) & 0xFF0000) | (x << 24); }

json Finite(double d) { return std::isfinite(d) ? json(d) : json(std::isnan(d) ? "nan" : d > 0 ? "inf" : "-inf"); }

std::string HexBytes(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < n; ++i) { out += d[p[i] >> 4]; out += d[p[i] & 15]; }
    return out;
}

// ------------------------------------------------------------------ SQLite ---

struct Sqlite {
    std::vector<uint8_t> bytes;
    View v;
    uint32_t pageSize = 0, usable = 0, pageCount = 0, headerPages = 0, encoding = 1;
    size_t visited = 0;

    explicit Sqlite(const ByteSource& source) : bytes(bounded::Read(source, 0, source.Size(), 1024u << 20)), v(bytes) {
        if (std::memcmp(v.Data(0, 16), "SQLite format 3\0", 16) != 0) throw Error("sqlite_magic");
        const uint32_t raw = v.U16(16, false);
        pageSize = raw == 1 ? 65536 : raw;
        if (pageSize < 512 || (pageSize & (pageSize - 1))) throw Error("sqlite_page_size_invalid");
        usable = pageSize - v.U8(20);
        if (usable < 480) throw Error("sqlite_usable_size_invalid");
        headerPages = pageCount = v.U32(28, false);
        const uint64_t actual = v.Size() / pageSize;
        if (!pageCount || pageCount > actual) pageCount = static_cast<uint32_t>(actual);  // header count is advisory
        encoding = v.U32(56, false);
    }

    View Page(uint32_t number) const {
        if (!number || number > pageCount) throw Error("sqlite_page_number_invalid");
        return v.Slice(uint64_t(number - 1) * pageSize, pageSize);
    }

    static uint64_t Varint(const View& p, uint64_t& at) {
        uint64_t x = 0;
        for (int i = 0; i < 8; ++i) {
            const uint8_t b = p.U8(at++);
            x = (x << 7) | (b & 0x7F);
            if (!(b & 0x80)) return x;
        }
        return (x << 8) | p.U8(at++);
    }

    // The full payload of a table-leaf cell, following overflow pages.
    std::vector<uint8_t> Payload(const View& page, uint64_t at, uint64_t size) {
        if (size > (64u << 20)) throw Error("sqlite_payload_limit");
        const uint64_t x = usable - 35;
        uint64_t local = size;
        if (size > x) {
            const uint64_t m = ((uint64_t(usable) - 12) * 32 / 255) - 23;
            const uint64_t k = m + ((size - m) % (usable - 4));
            local = k <= x ? k : m;
        }
        std::vector<uint8_t> out(page.Data(at, local), page.Data(at, local) + local);
        if (local < size) {
            uint32_t next = page.U32(at + local, false);
            std::set<uint32_t> seen;
            while (out.size() < size) {
                if (!next || !seen.insert(next).second || ++visited > 2000000) throw Error("sqlite_overflow_chain_invalid");
                const View o = Page(next);
                const uint64_t take = std::min<uint64_t>(usable - 4, size - out.size());
                const uint8_t* p = o.Data(4, take);
                out.insert(out.end(), p, p + take);
                next = o.U32(0, false);
            }
        }
        return out;
    }

    json Value(const View& record, uint64_t serial, uint64_t& body) {
        switch (serial) {
            case 0: return nullptr;
            case 8: return 0;
            case 9: return 1;
            case 1: case 2: case 3: case 4: case 5: case 6: {
                static const unsigned widths[] = {0, 1, 2, 3, 4, 6, 8};
                const unsigned n = widths[serial];
                uint64_t x = 0;
                for (unsigned i = 0; i < n; ++i) x = (x << 8) | record.U8(body + i);
                if (n < 8 && (x >> (n * 8 - 1)) & 1) x |= ~uint64_t(0) << (n * 8);
                body += n;
                return static_cast<int64_t>(x);
            }
            case 7: { const uint64_t bits = record.U64(body, false); body += 8; double d; std::memcpy(&d, &bits, 8); return Finite(d); }
            default: {
                if (serial < 12) throw Error("sqlite_serial_type_reserved");
                const uint64_t n = (serial - (serial & 1 ? 13 : 12)) / 2;
                const View field = record.Slice(body, n);
                body += n;
                if (!(serial & 1)) return {{"blob_bytes", n}};
                const size_t shown = std::min<uint64_t>(n, 8192);
                if (encoding == 2 || encoding == 3) return bounded::Utf16(field.Slice(0, shown & ~size_t(1)), encoding == 2);
                return field.Text(0, shown);
            }
        }
    }

    json Record(const std::vector<uint8_t>& payload) {
        const View r(payload);
        uint64_t at = 0;
        const uint64_t headerSize = Varint(r, at);
        if (headerSize > r.Size()) throw Error("sqlite_record_header_invalid");
        std::vector<uint64_t> serials;
        while (at < headerSize) { serials.push_back(Varint(r, at)); if (serials.size() > 4096) throw Error("sqlite_column_limit"); }
        uint64_t body = headerSize;
        json row = json::array();
        for (uint64_t s : serials) row.push_back(Value(r, s, body));
        return row;
    }

    // Walks a b-tree; calls leaf(cell payload) for table leaves, counts cells.
    void Walk(uint32_t root, const std::function<void(const std::vector<uint8_t>&)>& leaf, uint64_t& cells, std::set<uint32_t>& seen, unsigned depth = 0) {
        if (depth > 64 || !seen.insert(root).second || ++visited > 2000000) throw Error("sqlite_btree_loop_or_limit");
        const View page = Page(root);
        const uint64_t hdr = root == 1 ? 100 : 0;
        const uint8_t type = page.U8(hdr);
        const uint16_t n = page.U16(hdr + 3, false);
        const bool interior = type == 2 || type == 5;
        if (type != 2 && type != 5 && type != 10 && type != 13) throw Error("sqlite_btree_page_type_invalid");
        const uint64_t pointers = hdr + (interior ? 12 : 8);
        for (uint16_t i = 0; i < n; ++i) {
            uint64_t at = page.U16(pointers + uint64_t(i) * 2, false);
            if (type == 5) { Walk(page.U32(at, false), leaf, cells, seen, depth + 1); continue; }
            if (type == 2) { Walk(page.U32(at, false), leaf, cells, seen, depth + 1); ++cells; continue; }  // index interior cells are records too
            ++cells;
            if (type == 13 && leaf) {
                const uint64_t size = Varint(page, at);
                (void)Varint(page, at);  // rowid
                leaf(Payload(page, at, size));
            }
        }
        if (interior) Walk(page.U32(hdr + 8, false), leaf, cells, seen, depth + 1);
    }

    json Inspect(const InspectOptions& options) {
        json schema = json::array(), tables = json::array();
        uint64_t schemaCells = 0;
        std::set<uint32_t> seen;
        std::vector<std::pair<std::string, uint32_t>> roots;
        Walk(1, [&](const std::vector<uint8_t>& payload) {
            const json row = Record(payload);
            if (row.size() < 5) return;
            json entry = {{"type", row[0]}, {"name", row[1]}, {"table", row[2]}, {"root_page", row[3]}, {"sql", row[4]}};
            if (row[0] == "table" && row[3].is_number_integer() && row[3].get<int64_t>() > 0)
                roots.emplace_back(row[1].is_string() ? row[1].get<std::string>() : "", static_cast<uint32_t>(row[3].get<int64_t>()));
            if (schema.size() < options.maxEntries) schema.push_back(std::move(entry));
        }, schemaCells, seen);
        for (const auto& [name, root] : roots) {
            if (tables.size() >= options.maxEntries) break;
            json row = {{"name", name}, {"root_page", root}};
            try {
                uint64_t cells = 0; std::set<uint32_t> s;
                Walk(root, nullptr, cells, s);
                row["rows"] = cells;
            } catch (const Error& e) { row["error"] = e.what(); }
            tables.push_back(std::move(row));
        }
        const uint32_t version = v.U32(96, false);
        json warnings = json::array();
        if (headerPages > pageCount) warnings.push_back("file_shorter_than_header: " + std::to_string(headerPages) + " pages declared, " + std::to_string(pageCount) + " present");
        if (v.Size() % pageSize) warnings.push_back("partial_last_page");
        for (const auto& table : tables) if (table.contains("error")) { warnings.push_back("table_unreadable: " + table.value("name", std::string())); }
        return {{"format", "sqlite"}, {"warnings", warnings}, {"page_size", pageSize}, {"page_count", pageCount},
                {"text_encoding", encoding == 1 ? "utf-8" : encoding == 2 ? "utf-16le" : encoding == 3 ? "utf-16be" : "unknown"},
                {"journal_mode", v.U8(18) == 2 ? "wal" : "rollback"},
                {"user_version", v.U32(60, false)}, {"application_id", v.U32(68, false)},
                {"sqlite_version", std::to_string(version / 1000000) + "." + std::to_string(version / 1000 % 1000) + "." + std::to_string(version % 1000)},
                {"freelist_pages", v.U32(36, false)}, {"schema", schema}, {"tables", tables}};
    }
};

json SqliteWal(const ByteSource& source) {
    const auto head = bounded::Read(source, 0, 32);
    View h(head);
    const uint32_t pageSize = h.U32(8, false);
    if (pageSize < 512 || pageSize > 65536 || (pageSize & (pageSize - 1))) throw Error("wal_page_size_invalid");
    const uint64_t frameSize = 24 + uint64_t(pageSize);
    const uint64_t frames = (source.Size() - 32) / frameSize;
    uint64_t commits = 0;
    std::set<uint32_t> pages;
    for (uint64_t i = 0; i < frames && i < 1000000; ++i) {
        uint32_t page = 0, dbSize = 0;
        if (!ReadU32(source, 32 + i * frameSize, page) || !ReadU32(source, 36 + i * frameSize, dbSize)) break;
        page = Swap32(page); dbSize = Swap32(dbSize);
        pages.insert(page);
        if (dbSize) ++commits;
    }
    return {{"format", "sqlite-wal"}, {"version", h.U32(4, false)}, {"page_size", pageSize},
            {"checkpoint_sequence", h.U32(12, false)}, {"big_endian_checksums", h.U32(0, false) == 0x377F0683},
            {"frames", frames}, {"commit_frames", commits}, {"distinct_pages", pages.size()}};
}

// ------------------------------------------------------------------ bplist ---

struct BPlist {
    View v;
    unsigned offsetSize = 0, refSize = 0;
    uint64_t count = 0, top = 0, table = 0;
    size_t budget = 200000;

    uint64_t Uint(uint64_t at, unsigned n) const {
        uint64_t x = 0;
        for (unsigned i = 0; i < n; ++i) x = (x << 8) | v.U8(at + i);
        return x;
    }
    uint64_t Offset(uint64_t index) const {
        if (index >= count) throw Error("bplist_object_index_invalid");
        const uint64_t off = Uint(table + index * offsetSize, offsetSize);
        if (off >= table) throw Error("bplist_object_offset_invalid");
        return off;
    }
    uint64_t Length(uint64_t& at, uint8_t low) const {
        if (low != 0xF) return low;
        const uint8_t marker = v.U8(at++);
        if ((marker & 0xF0) != 0x10) throw Error("bplist_length_marker_invalid");
        const unsigned n = 1u << (marker & 0xF);
        if (n > 8) throw Error("bplist_length_too_wide");
        const uint64_t len = Uint(at, n);
        at += n;
        return len;
    }
    json Object(uint64_t index, std::vector<uint64_t>& path) {
        if (!budget--) throw Error("bplist_object_budget");
        if (path.size() > 256 || std::find(path.begin(), path.end(), index) != path.end()) throw Error("bplist_cycle_or_depth");
        uint64_t at = Offset(index);
        const uint8_t marker = v.U8(at++);
        const uint8_t hi = marker >> 4, low = marker & 0xF;
        switch (hi) {
            case 0x0:
                if (marker == 0x00) return nullptr;
                if (marker == 0x08) return false;
                if (marker == 0x09) return true;
                throw Error("bplist_marker_invalid");
            case 0x1: {
                const unsigned n = 1u << low;
                if (n > 16) throw Error("bplist_int_width_invalid");
                if (n == 16) return static_cast<int64_t>(Uint(at + 8, 8));  // 128-bit: low half
                const uint64_t x = Uint(at, n);
                return n == 8 ? json(static_cast<int64_t>(x)) : json(x);
            }
            case 0x2: {
                if (low == 2) { uint32_t w = static_cast<uint32_t>(Uint(at, 4)); float f; std::memcpy(&f, &w, 4); return Finite(f); }
                if (low == 3) { uint64_t w = Uint(at, 8); double d; std::memcpy(&d, &w, 8); return Finite(d); }
                throw Error("bplist_real_width_invalid");
            }
            case 0x3: { if (marker != 0x33) throw Error("bplist_date_marker_invalid"); uint64_t w = Uint(at, 8); double d; std::memcpy(&d, &w, 8); return {{"$date", Finite(d)}}; }
            case 0x4: { const uint64_t n = Length(at, low); return {{"$data", HexBytes(v.Data(at, n), std::min<uint64_t>(n, 64))}, {"$size", n}}; }
            case 0x5: { const uint64_t n = Length(at, low); return v.Text(at, n); }
            case 0x6: { const uint64_t n = Length(at, low); if (n > (1u << 28)) throw Error("bplist_string_limit"); return bounded::Utf16(v.Slice(at, n * 2), false); }
            case 0x8: return {{"$uid", Uint(at, low + 1u)}};
            case 0xA: case 0xC: {
                const uint64_t n = Length(at, low);
                json out = json::array();
                path.push_back(index);
                for (uint64_t i = 0; i < n; ++i) out.push_back(Object(Uint(at + i * refSize, refSize), path));
                path.pop_back();
                return hi == 0xC ? json{{"$set", out}} : out;
            }
            case 0xD: {
                const uint64_t n = Length(at, low);
                json out = json::object();
                path.push_back(index);
                for (uint64_t i = 0; i < n; ++i) {
                    const json key = Object(Uint(at + i * refSize, refSize), path);
                    const json value = Object(Uint(at + (n + i) * refSize, refSize), path);
                    out[key.is_string() ? key.get<std::string>() : key.dump()] = value;
                }
                path.pop_back();
                return out;
            }
            default: throw Error("bplist_marker_invalid");
        }
    }
};

json BinaryPlist(const ByteSource& source) {
    const auto bytes = bounded::Read(source, 0, source.Size(), 256u << 20);
    BPlist p; p.v = View(bytes);
    if (std::memcmp(p.v.Data(0, 8), "bplist00", 8) != 0) throw Error("bplist_magic");
    const uint64_t t = p.v.Size() - 32;
    p.v.Data(t, 32);
    p.offsetSize = p.v.U8(t + 6); p.refSize = p.v.U8(t + 7);
    p.count = p.v.U64(t + 8, false); p.top = p.v.U64(t + 16, false); p.table = p.v.U64(t + 24, false);
    if (!p.offsetSize || p.offsetSize > 8 || !p.refSize || p.refSize > 8) throw Error("bplist_trailer_sizes_invalid");
    if (p.count > 10000000 || !bounded::Range(t, p.table, p.count * p.offsetSize)) throw Error("bplist_offset_table_invalid");
    std::vector<uint64_t> path;
    json root = p.Object(p.top, path);
    json result = {{"format", "bplist"}, {"object_count", p.count}, {"root", root}};
    const bool keyed = root.is_object() && root.contains("$archiver") && root["$archiver"].is_string() &&
                       root["$archiver"].get<std::string>() == "NSKeyedArchiver" && root.contains("$objects") && root["$objects"].is_array();
    if (keyed) {
        std::set<std::string> classes;
        for (const auto& o : root["$objects"])
            if (o.is_object()) {
                if (o.contains("$classname") && o["$classname"].is_string()) classes.insert(o["$classname"].get<std::string>());
                if (o.contains("$classes") && o["$classes"].is_array())  // the class hierarchy
                    for (const auto& c : o["$classes"]) if (c.is_string()) classes.insert(c.get<std::string>());
            }
        result["keyed_archive"] = {{"objects", root["$objects"].size()}, {"classes", classes},
                                   {"note", "class names are listed; nothing is instantiated"}};
    }
    return result;
}

// ------------------------------------------------------------------ pickle ---

enum class Arg { None, U1, U2, I4, U4, U8, Line, TwoLines, Len1, Len4, Len8, F8, Long1, Long4 };
struct PickleOp { uint8_t code; const char* name; Arg arg; };
const PickleOp kPickle[] = {
    {'(', "MARK", Arg::None}, {'.', "STOP", Arg::None}, {'0', "POP", Arg::None}, {'1', "POP_MARK", Arg::None},
    {'2', "DUP", Arg::None}, {'F', "FLOAT", Arg::Line}, {'I', "INT", Arg::Line}, {'J', "BININT", Arg::I4},
    {'K', "BININT1", Arg::U1}, {'L', "LONG", Arg::Line}, {'M', "BININT2", Arg::U2}, {'N', "NONE", Arg::None},
    {'P', "PERSID", Arg::Line}, {'Q', "BINPERSID", Arg::None}, {'R', "REDUCE", Arg::None}, {'S', "STRING", Arg::Line},
    {'T', "BINSTRING", Arg::Len4}, {'U', "SHORT_BINSTRING", Arg::Len1}, {'V', "UNICODE", Arg::Line}, {'X', "BINUNICODE", Arg::Len4},
    {'a', "APPEND", Arg::None}, {'b', "BUILD", Arg::None}, {'c', "GLOBAL", Arg::TwoLines}, {'d', "DICT", Arg::None},
    {'}', "EMPTY_DICT", Arg::None}, {'e', "APPENDS", Arg::None}, {'g', "GET", Arg::Line}, {'h', "BINGET", Arg::U1},
    {'i', "INST", Arg::TwoLines}, {'j', "LONG_BINGET", Arg::U4}, {'l', "LIST", Arg::None}, {']', "EMPTY_LIST", Arg::None},
    {'o', "OBJ", Arg::None}, {'p', "PUT", Arg::Line}, {'q', "BINPUT", Arg::U1}, {'r', "LONG_BINPUT", Arg::U4},
    {'s', "SETITEM", Arg::None}, {'t', "TUPLE", Arg::None}, {')', "EMPTY_TUPLE", Arg::None}, {'u', "SETITEMS", Arg::None},
    {'G', "BINFLOAT", Arg::F8}, {0x80, "PROTO", Arg::U1}, {0x81, "NEWOBJ", Arg::None}, {0x82, "EXT1", Arg::U1},
    {0x83, "EXT2", Arg::U2}, {0x84, "EXT4", Arg::I4}, {0x85, "TUPLE1", Arg::None}, {0x86, "TUPLE2", Arg::None},
    {0x87, "TUPLE3", Arg::None}, {0x88, "NEWTRUE", Arg::None}, {0x89, "NEWFALSE", Arg::None}, {0x8A, "LONG1", Arg::Long1},
    {0x8B, "LONG4", Arg::Long4}, {'B', "BINBYTES", Arg::Len4}, {'C', "SHORT_BINBYTES", Arg::Len1},
    {0x8C, "SHORT_BINUNICODE", Arg::Len1}, {0x8D, "BINUNICODE8", Arg::Len8}, {0x8E, "BINBYTES8", Arg::Len8},
    {0x8F, "EMPTY_SET", Arg::None}, {0x90, "ADDITEMS", Arg::None}, {0x91, "FROZENSET", Arg::None},
    {0x92, "NEWOBJ_EX", Arg::None}, {0x93, "STACK_GLOBAL", Arg::None}, {0x94, "MEMOIZE", Arg::None},
    {0x95, "FRAME", Arg::U8}, {0x96, "BYTEARRAY8", Arg::Len8}, {0x97, "NEXT_BUFFER", Arg::None}, {0x98, "READONLY_BUFFER", Arg::None},
};

const PickleOp* FindPickleOp(uint8_t code) {
    for (const auto& op : kPickle) if (op.code == code) return &op;
    return nullptr;
}

// What an import would let a pickle do when it is loaded.
std::string Assess(const std::string& module, const std::string& name) {
    static const std::set<std::string> dangerousModules = {
        "os", "posix", "nt", "subprocess", "sys", "shutil", "socket", "runpy", "importlib", "ctypes", "code",
        "pty", "webbrowser", "commands", "multiprocessing", "asyncio", "pickle", "_pickle", "marshal", "types", "operator"};
    static const std::set<std::string> dangerousBuiltins = {
        "eval", "exec", "compile", "__import__", "getattr", "setattr", "open", "breakpoint", "globals", "locals", "vars", "input"};
    const std::string root = module.substr(0, module.find('.'));
    if (dangerousModules.count(root)) return "dangerous";
    if ((module == "builtins" || module == "__builtin__") && dangerousBuiltins.count(name)) return "dangerous";
    if (module.rfind("torch", 0) == 0 && (name.rfind("_rebuild_", 0) == 0 || name.find("Storage") != std::string::npos)) return "tensor_rebuild";
    if (module == "collections" && name == "OrderedDict") return "container";
    if ((module == "numpy.core.multiarray" || module == "numpy._core.multiarray") && name == "_reconstruct") return "array_rebuild";
    if (module == "numpy" && (name == "dtype" || name == "ndarray")) return "array_rebuild";
    if (module == "builtins" || module == "__builtin__" || module == "copy_reg" || module == "copyreg" || module == "datetime" || module == "decimal")
        return "standard";
    return "unknown";
}

} // namespace

json DisassemblePickle(const std::vector<uint8_t>& bytes, size_t maxInstructions, std::string& error) {
    error.clear();
    try {
        const View v(bytes);
        // Symbolic stack: strings are kept so STACK_GLOBAL can be resolved; the
        // rest is opaque. Nothing is constructed or called.
        struct Item { bool mark = false; bool text = false; std::string s; };
        std::vector<Item> stack;
        std::map<uint64_t, Item> memo;
        json instructions = json::array(), imports = json::array(), counts = json::object();
        uint64_t at = 0, persistent = 0, opcodes = 0;
        int protocol = 0;
        bool stopped = false, dangerous = false, truncated = false;
        auto pop = [&]() { if (stack.empty()) throw Error("pickle_stack_underflow"); Item i = stack.back(); stack.pop_back(); return i; };
        auto popMark = [&]() { while (true) { const Item i = pop(); if (i.mark) return; } };
        auto push = [&](Item i) { if (stack.size() > 1000000) throw Error("pickle_stack_limit"); stack.push_back(std::move(i)); };
        auto line = [&]() {
            size_t len = 0;
            while (v.U8(at + len) != '\n') if (++len > 1 << 20) throw Error("pickle_line_limit");
            std::string s = v.Text(at, len);
            at += len + 1;
            return s;
        };
        auto addImport = [&](const std::string& module, const std::string& name, const char* op) {
            const std::string assessment = Assess(module, name);
            dangerous = dangerous || assessment == "dangerous";
            if (imports.size() < 4096) imports.push_back({{"module", module}, {"name", name}, {"opcode", op}, {"assessment", assessment}});
        };
        while (!stopped) {
            if (++opcodes > 50000000) throw Error("pickle_opcode_limit");
            const uint64_t offset = at;
            const uint8_t code = v.U8(at++);
            const PickleOp* op = FindPickleOp(code);
            if (!op) throw Error("pickle_opcode_invalid:" + HexU64(code));
            json arg = nullptr;
            Item value;  // what the opcode pushes, when it pushes a literal
            auto takeLen = [&](uint64_t n) -> std::string {
                if (n > (256u << 20)) throw Error("pickle_length_limit");
                const std::string s = v.Text(at, std::min<uint64_t>(n, 1u << 16));
                v.Data(at, n);
                at += n;
                return s;
            };
            switch (op->arg) {
                case Arg::None: break;
                case Arg::U1: arg = v.U8(at); at += 1; break;
                case Arg::U2: arg = v.U16(at); at += 2; break;
                case Arg::I4: arg = static_cast<int32_t>(v.U32(at)); at += 4; break;
                case Arg::U4: arg = v.U32(at); at += 4; break;
                case Arg::U8: arg = v.U64(at); at += 8; break;
                case Arg::F8: { uint64_t b = v.U64(at, false); double d; std::memcpy(&d, &b, 8); arg = Finite(d); at += 8; break; }
                case Arg::Line: arg = line(); break;
                case Arg::TwoLines: { const std::string module = line(); const std::string name = line(); arg = module + " " + name; break; }
                case Arg::Len1: { const uint64_t n = v.U8(at); at += 1; value.s = takeLen(n); arg = value.s; break; }
                case Arg::Len4: { const uint64_t n = v.U32(at); at += 4; value.s = takeLen(n); arg = value.s; break; }
                case Arg::Len8: { const uint64_t n = v.U64(at); at += 8; value.s = takeLen(n); arg = value.s; break; }
                case Arg::Long1: { const uint64_t n = v.U8(at); at += 1; v.Data(at, n); at += n; arg = std::to_string(n) + "-byte integer"; break; }
                case Arg::Long4: { const uint64_t n = v.U32(at); at += 4; if (n > (64u << 20)) throw Error("pickle_long_limit"); v.Data(at, n); at += n; arg = std::to_string(n) + "-byte integer"; break; }
            }
            const std::string name = op->name;
            counts[name] = counts.value(name, 0) + 1;
            switch (code) {
                case '(': push({true, false, ""}); break;
                case '.': stopped = true; break;
                case '0': pop(); break;
                case '1': popMark(); break;
                case '2': { const Item i = pop(); push(i); push(i); break; }
                case 'P': ++persistent; push({}); break;
                case 'Q': pop(); ++persistent; push({}); break;
                case 'R': pop(); pop(); push({}); break;
                case 'a': pop(); break;
                case 'b': pop(); break;
                case 'c': { const std::string s = arg.get<std::string>(); addImport(s.substr(0, s.find(' ')), s.substr(s.find(' ') + 1), "GLOBAL"); push({}); break; }
                case 'i': { const std::string s = arg.get<std::string>(); addImport(s.substr(0, s.find(' ')), s.substr(s.find(' ') + 1), "INST"); popMark(); push({}); break; }
                case 'd': case 'l': case 't': case 'o': case 0x91: popMark(); push({}); break;
                case 'e': case 'u': case 0x90: popMark(); break;
                case 's': pop(); pop(); break;
                case 'g': case 'h': case 'j': {
                    const uint64_t index = arg.is_string() ? std::strtoull(arg.get<std::string>().c_str(), nullptr, 10) : arg.get<uint64_t>();
                    const auto it = memo.find(index);
                    push(it == memo.end() ? Item{} : it->second);
                    break;
                }
                case 'p': case 'q': case 'r': {
                    if (stack.empty()) throw Error("pickle_stack_underflow");
                    const uint64_t index = arg.is_string() ? std::strtoull(arg.get<std::string>().c_str(), nullptr, 10) : arg.get<uint64_t>();
                    memo[index] = stack.back();
                    break;
                }
                case 0x94: if (stack.empty()) throw Error("pickle_stack_underflow"); memo[memo.size()] = stack.back(); break;
                case 0x80: protocol = arg.get<int>(); break;
                case 0x81: pop(); pop(); push({}); break;
                case 0x92: pop(); pop(); pop(); push({}); break;
                case 0x85: pop(); push({}); break;
                case 0x86: pop(); pop(); push({}); break;
                case 0x87: pop(); pop(); pop(); push({}); break;
                case 0x93: {
                    const Item n = pop(), m = pop();
                    addImport(m.text ? m.s : "<computed>", n.text ? n.s : "<computed>", "STACK_GLOBAL");
                    push({});
                    break;
                }
                case 0x95: case 0x98: break;
                case 'S': case 'T': case 'U': case 'V': case 'X': case 0x8C: case 0x8D: {
                    Item s; s.text = true; s.s = arg.is_string() ? arg.get<std::string>() : "";
                    if (code == 'S' && s.s.size() >= 2) s.s = s.s.substr(1, s.s.size() - 2);  // quoted repr
                    push(std::move(s));
                    break;
                }
                default: push({}); break;  // every remaining opcode pushes one value
            }
            if (instructions.size() < maxInstructions) {
                json row = {{"offset", offset}, {"op", name}};
                if (!arg.is_null()) row["arg"] = arg.is_string() ? json(bounded::Clip(arg.get<std::string>(), 200)) : arg;
                instructions.push_back(std::move(row));
            } else truncated = true;
        }
        json result = {{"format", "pickle"}, {"protocol", protocol}, {"opcode_count", opcodes}, {"size", at},
                       {"imports", imports}, {"opcode_counts", counts}, {"persistent_ids", persistent},
                       {"calls", counts.value("REDUCE", 0) + counts.value("NEWOBJ", 0) + counts.value("NEWOBJ_EX", 0) +
                                 counts.value("OBJ", 0) + counts.value("INST", 0) + counts.value("BUILD", 0)},
                       {"dangerous_imports", dangerous}, {"executed", false},
                       {"instructions", instructions}, {"truncated", truncated}};
        if (at < v.Size()) result["trailing_bytes"] = v.Size() - at;
        return result;
    } catch (const Error& e) { error = e.what(); return nullptr; }
}


json PickleLiteral(const std::vector<uint8_t>& bytes, std::string& error) {
    error.clear();
    try {
        const View v(bytes);
        std::vector<json> stack;
        std::vector<size_t> marks;
        std::map<uint64_t, json> memo;
        uint64_t at = 0, steps = 0;
        auto pop = [&]() { if (stack.empty() || (!marks.empty() && marks.back() == stack.size())) throw Error("pickle_stack_underflow"); json x = std::move(stack.back()); stack.pop_back(); return x; };
        auto popMark = [&]() {
            if (marks.empty()) throw Error("pickle_mark_missing");
            const size_t m = marks.back(); marks.pop_back();
            std::vector<json> items(std::make_move_iterator(stack.begin() + m), std::make_move_iterator(stack.end()));
            stack.resize(m);
            return items;
        };
        auto top = [&]() -> json& { if (stack.empty()) throw Error("pickle_stack_underflow"); return stack.back(); };
        auto text = [&](uint64_t n) { if (n > (64u << 20)) throw Error("pickle_length_limit"); std::string s = v.Text(at, n); at += n; return s; };
        auto raw = [&](uint64_t n) { if (n > (64u << 20)) throw Error("pickle_length_limit"); json b = {{"$bytes", HexBytes(v.Data(at, n), n)}}; at += n; return b; };
        auto setItems = [&](json& target, std::vector<json>& items) {
            if (!target.is_object()) throw Error("pickle_setitem_target");
            if (items.size() % 2) throw Error("pickle_setitems_odd");
            for (size_t i = 0; i < items.size(); i += 2) {
                const std::string key = items[i].is_string() ? items[i].get<std::string>() : items[i].dump();
                target[key] = std::move(items[i + 1]);
            }
        };
        while (true) {
            if (++steps > 20000000 || stack.size() > 10000000) throw Error("pickle_budget");
            const uint8_t op = v.U8(at++);
            switch (op) {
                case 0x80: at += 1; break;                       // PROTO
                case 0x95: at += 8; break;                       // FRAME
                case '.': if (stack.size() != 1 || !marks.empty()) throw Error("pickle_stack_not_single"); return stack[0];
                case '(': marks.push_back(stack.size()); break;
                case '}': stack.push_back(json::object()); break;
                case ']': stack.push_back(json::array()); break;
                case ')': stack.push_back(json::array()); break;
                case 'N': stack.push_back(nullptr); break;
                case 0x88: stack.push_back(true); break;
                case 0x89: stack.push_back(false); break;
                case 'K': stack.push_back(v.U8(at)); at += 1; break;
                case 'M': stack.push_back(v.U16(at)); at += 2; break;
                case 'J': stack.push_back(static_cast<int32_t>(v.U32(at))); at += 4; break;
                case 0x8A: {                                     // LONG1, little-endian two's complement
                    const uint8_t n = v.U8(at++);
                    if (n > 8) throw Error("pickle_long_too_wide");
                    uint64_t x = 0;
                    for (uint8_t i = 0; i < n; ++i) x |= uint64_t(v.U8(at + i)) << (8 * i);
                    if (n && n < 8 && (v.U8(at + n - 1) & 0x80)) x |= ~uint64_t(0) << (8 * n);
                    at += n;
                    stack.push_back(static_cast<int64_t>(x));
                    break;
                }
                case 'G': { const uint64_t b = v.U64(at, false); double d; std::memcpy(&d, &b, 8); at += 8; stack.push_back(Finite(d)); break; }
                case 'X': { const uint32_t n = v.U32(at); at += 4; stack.push_back(text(n)); break; }
                case 0x8C: { const uint8_t n = v.U8(at++); stack.push_back(text(n)); break; }
                case 0x8D: { const uint64_t n = v.U64(at); at += 8; stack.push_back(text(n)); break; }
                case 'B': case 'T': { const uint32_t n = v.U32(at); at += 4; stack.push_back(raw(n)); break; }
                case 'C': case 'U': { const uint8_t n = v.U8(at++); stack.push_back(raw(n)); break; }
                case 0x8E: { const uint64_t n = v.U64(at); at += 8; stack.push_back(raw(n)); break; }
                case 'd': { auto items = popMark(); json d = json::object(); setItems(d, items); stack.push_back(std::move(d)); break; }
                case 'l': case 't': { auto items = popMark(); stack.push_back(json(items)); break; }
                case 0x85: { json a = pop(); stack.push_back(json::array({std::move(a)})); break; }
                case 0x86: { json b = pop(), a = pop(); stack.push_back(json::array({std::move(a), std::move(b)})); break; }
                case 0x87: { json c = pop(), b = pop(), a = pop(); stack.push_back(json::array({std::move(a), std::move(b), std::move(c)})); break; }
                case 's': { json value = pop(), key = pop(); std::vector<json> items = {std::move(key), std::move(value)}; setItems(top(), items); break; }
                case 'u': { auto items = popMark(); setItems(top(), items); break; }
                case 'a': { json x = pop(); if (!top().is_array()) throw Error("pickle_append_target"); top().push_back(std::move(x)); break; }
                case 'e': { auto items = popMark(); if (!top().is_array()) throw Error("pickle_append_target"); for (auto& x : items) top().push_back(std::move(x)); break; }
                case 0x94: memo[memo.size()] = top(); break;
                case 'q': memo[v.U8(at)] = top(); at += 1; break;
                case 'r': memo[v.U32(at)] = top(); at += 4; break;
                case 'h': case 'j': {
                    const uint64_t index = op == 'h' ? v.U8(at) : v.U32(at);
                    at += op == 'h' ? 1 : 4;
                    const auto it = memo.find(index);
                    if (it == memo.end()) throw Error("pickle_memo_missing");
                    stack.push_back(it->second);
                    break;
                }
                default: throw Error("pickle_not_plain_data");
            }
        }
    } catch (const Error& e) { error = e.what(); return nullptr; }
}

namespace {

// ------------------------------------------------------------------- NumPy ---

// The header is a Python dict literal; only its documented subset is read.
struct PyLiteral {
    const std::string& s;
    size_t at = 0;
    void Space() { while (at < s.size() && (s[at] == ' ' || s[at] == '\n')) ++at; }
    bool Eat(char c) { Space(); if (at < s.size() && s[at] == c) { ++at; return true; } return false; }
    json Value(unsigned depth = 0) {
        if (depth > 16) throw Error("npy_header_depth");
        Space();
        if (at >= s.size()) throw Error("npy_header_truncated");
        const char c = s[at];
        if (c == '\'' || c == '"') {
            const size_t end = s.find(c, at + 1);
            if (end == std::string::npos) throw Error("npy_header_string");
            json out = s.substr(at + 1, end - at - 1);
            at = end + 1;
            return out;
        }
        if (c == '(' || c == '[' || c == '{') {
            const char close = c == '(' ? ')' : c == '[' ? ']' : '}';
            ++at;
            json out = c == '{' ? json::object() : json::array();
            while (!Eat(close)) {
                json key = Value(depth + 1);
                if (c == '{') { if (!Eat(':')) throw Error("npy_header_colon"); out[key.get<std::string>()] = Value(depth + 1); }
                else out.push_back(std::move(key));
                Eat(',');
                if (out.size() > 4096) throw Error("npy_header_limit");
            }
            return out;
        }
        size_t end = at;
        while (end < s.size() && (std::isalnum(static_cast<unsigned char>(s[end])) || s[end] == '-' || s[end] == '_')) ++end;
        const std::string word = s.substr(at, end - at);
        at = end;
        if (word == "True") return true;
        if (word == "False") return false;
        if (word == "None") return nullptr;
        if (!word.empty() && (std::isdigit(static_cast<unsigned char>(word[0])) || word[0] == '-')) {
            // Bounded integer: no std::stoll, whose exceptions are not ours.
            const bool negative = word[0] == '-';
            if (word.size() == size_t(negative) || word.size() > 19) throw Error("npy_header_integer");
            int64_t x = 0;
            for (size_t i = negative; i < word.size(); ++i) {
                if (!std::isdigit(static_cast<unsigned char>(word[i]))) throw Error("npy_header_integer");
                x = x * 10 + (word[i] - '0');
            }
            return negative ? -x : x;
        }
        throw Error("npy_header_token");
    }
};

uint64_t ItemSize(const std::string& descr, bool& object) {
    object = false;
    if (descr.size() < 2) throw Error("npy_descr_invalid");
    const char kind = descr[1];
    if (kind == 'O') { object = true; return 8; }
    std::string digits;
    for (size_t i = 2; i < descr.size() && std::isdigit(static_cast<unsigned char>(descr[i])); ++i) digits += descr[i];
    if (digits.empty() || digits.size() > 9) throw Error("npy_descr_size");
    const uint64_t n = std::stoull(digits);  // at most 9 digits: cannot overflow
    if (kind == 'U') return n * 4;
    if (kind == 'M' || kind == 'm') return 8;
    return n;
}

json Npy(const ByteSource& source) {
    const auto head = bounded::Read(source, 0, std::min<uint64_t>(source.Size(), 12));
    View h(head);
    if (std::memcmp(h.Data(0, 6), "\x93NUMPY", 6) != 0) throw Error("npy_magic");
    const uint8_t major = h.U8(6), minor = h.U8(7);
    if (major < 1 || major > 3) throw Error("npy_version_unsupported");
    const uint64_t headerLength = major == 1 ? h.U16(8) : h.U32(8);
    const uint64_t headerStart = major == 1 ? 10 : 12;
    if (headerLength > (1u << 20)) throw Error("npy_header_limit");
    const auto header = bounded::Read(source, headerStart, headerLength);
    const std::string text = bounded::Utf8(header.data(), header.size());  // header bytes are untrusted
    PyLiteral parser{text};
    const json dict = parser.Value();
    if (!dict.is_object() || !dict.contains("descr") || !dict.contains("shape")) throw Error("npy_header_keys");
    uint64_t elements = 1;
    for (const auto& d : dict["shape"]) {
        if (!d.is_number_integer() || d.get<int64_t>() < 0) throw Error("npy_shape_invalid");
        const uint64_t n = d.get<uint64_t>();
        if (n && elements > UINT64_MAX / n) throw Error("npy_shape_overflow");
        elements *= n;
    }
    json result = {{"format", "npy"}, {"version", std::to_string(major) + "." + std::to_string(minor)},
                   {"shape", dict["shape"]}, {"fortran_order", dict.value("fortran_order", false)},
                   {"elements", elements}, {"data_offset", headerStart + headerLength}};
    const uint64_t dataBytes = source.Size() - (headerStart + headerLength);
    if (dict["descr"].is_string()) {
        bool object = false;
        const std::string descr = dict["descr"];
        const uint64_t item = ItemSize(descr, object);
        result["dtype"] = descr;
        result["item_size"] = item;
        if (object) {
            result["object_array"] = true;
            result["note"] = "object arrays are stored as a pickle; it is disassembled, never loaded";
            std::string error;
            const auto pickled = bounded::Read(source, headerStart + headerLength, dataBytes, 64u << 20);
            const json pickle = DisassemblePickle(pickled, 256, error);
            result["pickle"] = pickle.is_null() ? json{{"error", error}} : pickle;
        } else {
            if (elements && item > UINT64_MAX / elements) throw Error("npy_size_overflow");
            result["data_bytes"] = dataBytes;
            result["size_matches"] = elements * item == dataBytes;
        }
    } else {
        result["dtype"] = "structured";
        result["fields"] = dict["descr"];
        result["data_bytes"] = dataBytes;
    }
    return result;
}

// ------------------------------------------------------------- safetensors ---

uint64_t DtypeSize(const std::string& t) {
    static const std::map<std::string, uint64_t> sizes = {
        {"F64", 8}, {"F32", 4}, {"F16", 2}, {"BF16", 2}, {"I64", 8}, {"I32", 4}, {"I16", 2}, {"I8", 1}, {"U64", 8},
        {"U32", 4}, {"U16", 2}, {"U8", 1}, {"BOOL", 1}, {"F8_E4M3", 1}, {"F8_E5M2", 1}, {"C64", 8}};
    const auto it = sizes.find(t);
    if (it == sizes.end()) throw Error("safetensors_dtype_unknown:" + t);
    return it->second;
}

json Safetensors(const ByteSource& source, const InspectOptions& options) {
    uint64_t n = 0;
    if (!ReadU64(source, 0, n) || n < 2 || n > (100u << 20) || n > source.Size() - 8) throw Error("safetensors_header_length");
    const auto header = bounded::Read(source, 8, n);
    json doc;
    try { doc = json::parse(header.begin(), header.end()); } catch (const json::exception&) { throw Error("safetensors_header_json"); }
    if (!doc.is_object()) throw Error("safetensors_header_not_object");
    const uint64_t dataSize = source.Size() - 8 - n;
    json tensors = json::array();
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    uint64_t total = 0;
    for (const auto& [name, t] : doc.items()) {
        if (name == "__metadata__") continue;
        if (!t.is_object() || !t.contains("dtype") || !t.contains("shape") || !t.contains("data_offsets") ||
            !t["dtype"].is_string() || !t["shape"].is_array() || !t["data_offsets"].is_array() || t["data_offsets"].size() != 2 ||
            !t["data_offsets"][0].is_number_unsigned() || !t["data_offsets"][1].is_number_unsigned()) throw Error("safetensors_tensor_fields");
        for (const auto& d : t["shape"]) if (!d.is_number_unsigned()) throw Error("safetensors_shape_invalid");
        const std::string dtype = t["dtype"];
        uint64_t elements = 1;
        for (const auto& d : t["shape"]) { const uint64_t x = d.get<uint64_t>(); if (x && elements > UINT64_MAX / x) throw Error("safetensors_shape_overflow"); elements *= x; }
        const uint64_t begin = t["data_offsets"].at(0), end = t["data_offsets"].at(1);
        if (begin > end || end > dataSize) throw Error("safetensors_offsets_out_of_range");
        if (elements * DtypeSize(dtype) != end - begin) throw Error("safetensors_size_mismatch:" + name);
        ranges.emplace_back(begin, end);
        total += end - begin;
        if (tensors.size() < options.maxEntries) tensors.push_back({{"name", name}, {"dtype", dtype}, {"shape", t["shape"]}, {"bytes", end - begin}});
    }
    std::sort(ranges.begin(), ranges.end());
    for (size_t i = 1; i < ranges.size(); ++i) if (ranges[i].first < ranges[i - 1].second) throw Error("safetensors_overlapping_tensors");
    json result = {{"format", "safetensors"}, {"header_bytes", n}, {"tensor_count", ranges.size()}, {"tensors", tensors},
                   {"tensor_bytes", total}, {"data_bytes", dataSize}};
    if (doc.contains("__metadata__")) result["metadata"] = doc["__metadata__"];
    return result;
}

// -------------------------------------------------------------------- GGUF ---

const char* GgmlType(uint32_t t) {
    static const char* names[] = {"F32", "F16", "Q4_0", "Q4_1", "Q4_2", "Q4_3", "Q5_0", "Q5_1", "Q8_0", "Q8_1",
                                  "Q2_K", "Q3_K", "Q4_K", "Q5_K", "Q6_K", "Q8_K", "IQ2_XXS", "IQ2_XS", "IQ3_XXS",
                                  "IQ1_S", "IQ4_NL", "IQ3_S", "IQ2_S", "IQ4_XS", "I8", "I16", "I32", "I64", "F64",
                                  "IQ1_M", "BF16", "Q4_0_4_4", "Q4_0_4_8", "Q4_0_8_8", "TQ1_0", "TQ2_0"};
    return t < sizeof(names) / sizeof(names[0]) ? names[t] : "unknown";
}

struct Gguf {
    View v;
    uint64_t at = 0;
    std::string Str() {
        const uint64_t n = v.U64(at);
        if (n > (16u << 20)) throw Error("gguf_string_limit");
        std::string s = v.Text(at + 8, n);
        at += 8 + n;
        return s;
    }
    json Scalar(uint32_t type) {
        switch (type) {
            case 0: { const uint8_t x = v.U8(at); at += 1; return x; }
            case 1: { const int8_t x = static_cast<int8_t>(v.U8(at)); at += 1; return x; }
            case 2: { const uint16_t x = v.U16(at); at += 2; return x; }
            case 3: { const int16_t x = static_cast<int16_t>(v.U16(at)); at += 2; return x; }
            case 4: { const uint32_t x = v.U32(at); at += 4; return x; }
            case 5: { const int32_t x = static_cast<int32_t>(v.U32(at)); at += 4; return x; }
            case 6: { uint32_t b = v.U32(at); float f; std::memcpy(&f, &b, 4); at += 4; return Finite(f); }
            case 7: { const uint8_t x = v.U8(at); at += 1; if (x > 1) throw Error("gguf_bool_invalid"); return x == 1; }
            case 8: return Str();
            case 10: { const uint64_t x = v.U64(at); at += 8; return x; }
            case 11: { const int64_t x = static_cast<int64_t>(v.U64(at)); at += 8; return x; }
            case 12: { uint64_t b = v.U64(at); double d; std::memcpy(&d, &b, 8); at += 8; return Finite(d); }
            default: throw Error("gguf_value_type_invalid");
        }
    }
    json Value(uint32_t type) {
        if (type != 9) return Scalar(type);
        const uint32_t inner = v.U32(at); const uint64_t n = v.U64(at + 4); at += 12;
        if (inner == 9) throw Error("gguf_nested_array_unsupported");
        if (n > 100000000) throw Error("gguf_array_limit");
        json shown = json::array();
        for (uint64_t i = 0; i < n; ++i) { json x = Scalar(inner); if (shown.size() < 16) shown.push_back(std::move(x)); }
        return {{"array_type", inner}, {"count", n}, {"first", shown}};
    }
};

json GgufFile(const ByteSource& source, const InspectOptions& options) {
    const auto bytes = bounded::Read(source, 0, source.Size(), 1024u << 20);
    Gguf g; g.v = View(bytes);
    if (g.v.U32(0) != 0x46554747) throw Error("gguf_magic");
    const uint32_t version = g.v.U32(4);
    if (version < 2 || version > 3) throw Error("gguf_version_unsupported");
    const uint64_t tensors = g.v.U64(8), kvs = g.v.U64(16);
    if (tensors > 1000000 || kvs > 1000000) throw Error("gguf_count_limit");
    g.at = 24;
    json metadata = json::object();
    uint64_t alignment = 32;
    for (uint64_t i = 0; i < kvs; ++i) {
        const std::string key = g.Str();
        const uint32_t type = g.v.U32(g.at); g.at += 4;
        json value = g.Value(type);
        if (key == "general.alignment" && value.is_number_unsigned()) alignment = value.get<uint64_t>();
        if (metadata.size() < options.maxEntries) metadata[key] = std::move(value);
    }
    if (!alignment || alignment > 65536 || (alignment & (alignment - 1))) throw Error("gguf_alignment_invalid");
    json list = json::array();
    for (uint64_t i = 0; i < tensors; ++i) {
        const std::string name = g.Str();
        const uint32_t dims = g.v.U32(g.at); g.at += 4;
        if (dims > 8) throw Error("gguf_tensor_dims_invalid");
        json shape = json::array();
        for (uint32_t d = 0; d < dims; ++d) { shape.push_back(g.v.U64(g.at)); g.at += 8; }
        const uint32_t type = g.v.U32(g.at); const uint64_t offset = g.v.U64(g.at + 4); g.at += 12;
        if (offset % alignment) throw Error("gguf_tensor_offset_unaligned");
        if (list.size() < options.maxEntries) list.push_back({{"name", name}, {"shape", shape}, {"type", GgmlType(type)}, {"offset", offset}});
    }
    const uint64_t dataStart = (g.at + alignment - 1) / alignment * alignment;
    return {{"format", "gguf"}, {"version", version}, {"tensor_count", tensors}, {"metadata_count", kvs},
            {"metadata", metadata}, {"tensors", list}, {"alignment", alignment}, {"data_offset", dataStart},
            {"data_bytes", g.v.Size() > dataStart ? g.v.Size() - dataStart : 0}};
}

// -------------------------------------------------------------------- ONNX ---

struct Proto {
    View v;
    uint64_t at = 0, end = 0;
    bool Next(uint32_t& field, uint32_t& wire) {
        if (at >= end) return false;
        const uint64_t key = Varint();
        field = static_cast<uint32_t>(key >> 3);
        wire = key & 7;
        if (!field) throw Error("protobuf_field_zero");
        return true;
    }
    uint64_t Varint() {
        uint64_t x = 0;
        for (unsigned s = 0; s < 64; s += 7) {
            if (at >= end) throw Error("protobuf_truncated");
            const uint8_t b = v.U8(at++);
            x |= uint64_t(b & 0x7F) << s;
            if (!(b & 0x80)) return x;
        }
        throw Error("protobuf_varint_invalid");
    }
    View Bytes() {
        const uint64_t n = Varint();
        if (n > end - at) throw Error("protobuf_length_out_of_range");
        const View out = v.Slice(at, n);
        at += n;
        return out;
    }
    void Skip(uint32_t wire) {
        switch (wire) {
            case 0: Varint(); break;
            case 1: if (end - at < 8) throw Error("protobuf_truncated"); at += 8; break;
            case 2: Bytes(); break;
            case 5: if (end - at < 4) throw Error("protobuf_truncated"); at += 4; break;
            default: throw Error("protobuf_wire_type_invalid");
        }
    }
    static Proto Of(const View& b) { Proto p; p.v = b; p.end = b.Size(); return p; }
};

std::string Str(const View& b) { return b.Text(0, b.Size()); }

const char* OnnxType(uint64_t t) {
    static const char* names[] = {"undefined", "float", "uint8", "int8", "uint16", "int16", "int32", "int64", "string",
                                  "bool", "float16", "double", "uint32", "uint64", "complex64", "complex128", "bfloat16",
                                  "float8e4m3fn", "float8e4m3fnuz", "float8e5m2", "float8e5m2fnuz", "uint4", "int4", "float4e2m1"};
    return t < sizeof(names) / sizeof(names[0]) ? names[t] : "unknown";
}

json OnnxGraph(const View& bytes, const InspectOptions& options) {
    Proto p = Proto::Of(bytes);
    json nodes = json::array(), inits = json::array(), inputs = json::array(), outputs = json::array();
    std::string name;
    uint32_t field, wire;
    uint64_t nodeCount = 0;
    while (p.Next(field, wire)) {
        if (wire != 2) { p.Skip(wire); continue; }
        const View b = p.Bytes();
        Proto q = Proto::Of(b);
        uint32_t f, w;
        if (field == 1) {
            json node = {{"inputs", json::array()}, {"outputs", json::array()}};
            while (q.Next(f, w)) {
                if (w != 2) { q.Skip(w); continue; }
                const View s = q.Bytes();
                if (f == 1) node["inputs"].push_back(Str(s));
                else if (f == 2) node["outputs"].push_back(Str(s));
                else if (f == 3) node["name"] = Str(s);
                else if (f == 4) node["op_type"] = Str(s);
                else if (f == 7) node["domain"] = Str(s);
                else if (f == 5) node["attribute_count"] = node.value("attribute_count", 0) + 1;
            }
            ++nodeCount;
            if (nodes.size() < options.maxEntries) nodes.push_back(std::move(node));
        } else if (field == 2) {
            name = Str(b);
        } else if (field == 5) {
            json t = {{"dims", json::array()}};
            while (q.Next(f, w)) {
                if (f == 1 && w == 0) t["dims"].push_back(q.Varint());
                else if (f == 1 && w == 2) { Proto d = Proto::Of(q.Bytes()); while (d.at < d.end) t["dims"].push_back(d.Varint()); }
                else if (f == 2 && w == 0) t["data_type"] = OnnxType(q.Varint());
                else if (f == 8 && w == 2) t["name"] = Str(q.Bytes());
                else q.Skip(w);
            }
            if (inits.size() < options.maxEntries) inits.push_back(std::move(t));
        } else if (field == 11 || field == 12) {
            std::string n;
            while (q.Next(f, w)) { if (f == 1 && w == 2) n = Str(q.Bytes()); else q.Skip(w); }
            (field == 11 ? inputs : outputs).push_back(n);
        }
    }
    return {{"name", name}, {"node_count", nodeCount}, {"nodes", nodes}, {"initializers", inits},
            {"inputs", inputs}, {"outputs", outputs}};
}

json Onnx(const ByteSource& source, const InspectOptions& options) {
    const auto bytes = bounded::Read(source, 0, source.Size(), 2048u << 20);
    Proto p = Proto::Of(View(bytes));
    json result = {{"format", "onnx"}, {"opset_import", json::array()}, {"metadata", json::object()}};
    bool graph = false;
    uint32_t field, wire;
    while (p.Next(field, wire)) {
        if (field == 1 && wire == 0) { result["ir_version"] = p.Varint(); continue; }
        if (field == 5 && wire == 0) { result["model_version"] = p.Varint(); continue; }
        if (wire != 2) { p.Skip(wire); continue; }
        const View b = p.Bytes();
        switch (field) {
            case 2: result["producer_name"] = Str(b); break;
            case 3: result["producer_version"] = Str(b); break;
            case 4: result["domain"] = Str(b); break;
            case 6: result["doc_string"] = b.Text(0, std::min<size_t>(b.Size(), 4096)); break;
            case 7: result["graph"] = OnnxGraph(b, options); graph = true; break;
            case 8: {
                Proto q = Proto::Of(b); uint32_t f, w; json o = {{"domain", ""}};
                while (q.Next(f, w)) { if (f == 1 && w == 2) o["domain"] = Str(q.Bytes()); else if (f == 2 && w == 0) o["version"] = q.Varint(); else q.Skip(w); }
                result["opset_import"].push_back(std::move(o));
                break;
            }
            case 14: {
                Proto q = Proto::Of(b); uint32_t f, w; std::string k, val;
                while (q.Next(f, w)) { if (w == 2 && f == 1) k = Str(q.Bytes()); else if (w == 2 && f == 2) val = Str(q.Bytes()); else q.Skip(w); }
                result["metadata"][k] = val;
                break;
            }
            default: break;
        }
    }
    if (!graph || !result.contains("ir_version")) throw Error("onnx_model_without_graph");
    return result;
}

} // namespace

// --------------------------------------------------------------- Providers ---

// Untrusted headers (safetensors JSON, NumPy dict literal) reach nlohmann
// accessors: a type or range surprise there is a malformed file, not a crash.
#define CORTEX_TRY(expr) error.clear(); try { return expr; } catch (const Error& e) { error = e.what(); return nullptr; } \
    catch (const json::exception&) { error = "malformed_structure"; return nullptr; }

std::vector<Capability> SqliteProvider::Capabilities() const {
    return {{"static", CapabilityStatus::Experimental, "cortex_format_more_tests; sqlite3-built database and WAL",
             "header, schema (tables, indexes, views, triggers with SQL), row counts by b-tree walk, overflow pages, WAL frames"},
            {"decompile", CapabilityStatus::Unavailable, "", "not applicable"},
            {"live", CapabilityStatus::Unavailable, "", "the database is never opened by SQLite"}};
}
int SqliteProvider::Detect(const ByteSource& source) const {
    uint8_t m[16] = {};
    if (source.Read(0, m, 16) && !std::memcmp(m, "SQLite format 3\0", 16)) return 70;
    uint32_t w = 0;
    if (ReadU32(source, 0, w)) { w = Swap32(w); if ((w == 0x377F0682 || w == 0x377F0683) && source.Size() >= 32) return 70; }
    return 0;
}
json SqliteProvider::Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const {
    uint32_t w = 0;
    if (ReadU32(source, 0, w) && (Swap32(w) == 0x377F0682 || Swap32(w) == 0x377F0683)) { CORTEX_TRY(SqliteWal(source)) }
    CORTEX_TRY(Sqlite(source).Inspect(options))
}

std::vector<Capability> BinaryPlistProvider::Capabilities() const {
    return {{"static", CapabilityStatus::Experimental, "cortex_format_more_tests; plistlib-written fixtures",
             "every bplist00 object type, NSKeyedArchiver object and class inventory"},
            {"decompile", CapabilityStatus::Unavailable, "", "not applicable"},
            {"live", CapabilityStatus::Unavailable, "", "no archived object is instantiated"}};
}
int BinaryPlistProvider::Detect(const ByteSource& source) const {
    uint8_t m[8] = {}; return source.Read(0, m, 8) && !std::memcmp(m, "bplist00", 8) ? 70 : 0;
}
json BinaryPlistProvider::Inspect(const ByteSource& source, const InspectOptions&, std::string& error) const { CORTEX_TRY(BinaryPlist(source)) }

std::vector<Capability> PickleProvider::Capabilities() const {
    return {{"static", CapabilityStatus::Experimental, "cortex_format_more_tests; pickle fixtures checked against pickletools and a recording Unpickler",
             "protocol 0-5 opcode listing, imports (GLOBAL, INST, STACK_GLOBAL) with an assessment, call and persistent-id counts"},
            {"decompile", CapabilityStatus::Unavailable, "", "not applicable"},
            {"live", CapabilityStatus::Unavailable, "", "never unpickled: nothing is imported or called"}};
}
int PickleProvider::Detect(const ByteSource& source) const {
    uint8_t head[2] = {}, last = 0;
    if (source.Size() < 4 || !source.Read(0, head, 2) || !source.Read(source.Size() - 1, &last, 1)) return 0;
    return head[0] == 0x80 && head[1] >= 2 && head[1] <= 5 && last == '.' ? 40 : 0;
}
json PickleProvider::Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const {
    try {
        const auto bytes = bounded::Read(source, 0, source.Size(), 256u << 20);
        return DisassemblePickle(bytes, std::max<size_t>(options.maxEntries, 16), error);
    } catch (const Error& e) { error = e.what(); return nullptr; }
    catch (const json::exception&) { error = "malformed_structure"; return nullptr; }
}

std::vector<Capability> NpyProvider::Capabilities() const {
    return {{"static", CapabilityStatus::Experimental, "cortex_format_more_tests; numpy.save fixtures",
             "versions 1-3, dtype, shape, order, size check; object arrays have their pickle disassembled"},
            {"decompile", CapabilityStatus::Unavailable, "", "not applicable"},
            {"live", CapabilityStatus::Unavailable, "", "array data is never loaded"}};
}
int NpyProvider::Detect(const ByteSource& source) const {
    uint8_t m[6] = {}; return source.Read(0, m, 6) && !std::memcmp(m, "\x93NUMPY", 6) ? 70 : 0;
}
json NpyProvider::Inspect(const ByteSource& source, const InspectOptions&, std::string& error) const { CORTEX_TRY(Npy(source)) }

std::vector<Capability> SafetensorsProvider::Capabilities() const {
    return {{"static", CapabilityStatus::Experimental, "cortex_format_more_tests; safetensors.numpy fixture",
             "header, metadata, tensors with dtype/shape; offsets, sizes and overlaps checked"},
            {"decompile", CapabilityStatus::Unavailable, "", "not applicable"},
            {"live", CapabilityStatus::Unavailable, "", "tensors are never loaded"}};
}
int SafetensorsProvider::Detect(const ByteSource& source) const {
    uint64_t n = 0; uint8_t first = 0;
    if (!ReadU64(source, 0, n) || n < 2 || n > (100u << 20) || n > source.Size() - 8 || !source.Read(8, &first, 1)) return 0;
    return first == '{' ? 45 : 0;
}
json SafetensorsProvider::Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const { CORTEX_TRY(Safetensors(source, options)) }

std::vector<Capability> GgufProvider::Capabilities() const {
    return {{"static", CapabilityStatus::Experimental, "cortex_format_more_tests; gguf-py writer fixture checked against GGUFReader",
             "versions 2-3, metadata (arrays summarized), tensor table, alignment"},
            {"decompile", CapabilityStatus::Unavailable, "", "not applicable"},
            {"live", CapabilityStatus::Unavailable, "", "the model is never loaded"}};
}
int GgufProvider::Detect(const ByteSource& source) const { uint32_t m = 0; return ReadU32(source, 0, m) && m == 0x46554747 ? 70 : 0; }
json GgufProvider::Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const { CORTEX_TRY(GgufFile(source, options)) }

std::vector<Capability> OnnxProvider::Capabilities() const {
    return {{"static", CapabilityStatus::Experimental, "cortex_format_more_tests; onnx.helper fixture checked against onnx.load",
             "model fields, opsets, metadata, graph nodes, initializers, inputs and outputs"},
            {"decompile", CapabilityStatus::Unavailable, "", "not applicable"},
            {"live", CapabilityStatus::Unavailable, "", "the model is never run"}};
}
int OnnxProvider::Detect(const ByteSource& source) const {
    // No magic: a ModelProto starts with ir_version (field 1, varint) and
    // must parse with a graph. Only small, well-formed models are confirmed here.
    uint8_t head[3] = {};
    if (!source.Read(0, head, 3) || head[0] != 0x08 || head[1] == 0 || head[1] > 20) return 0;
    if (source.Size() > (64u << 20)) {
        // Large file: the field after ir_version must be one a ModelProto has.
        static const uint8_t next[] = {0x12, 0x1A, 0x22, 0x28, 0x32, 0x3A, 0x42, 0x72};
        return std::find(std::begin(next), std::end(next), head[2]) != std::end(next) ? 30 : 0;
    }
    InspectOptions options; options.maxEntries = 1;
    try { (void)Onnx(source, options); return 35; } catch (const Error&) { return 0; }
}
json OnnxProvider::Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const { CORTEX_TRY(Onnx(source, options)) }

#undef CORTEX_TRY

} // namespace cortex::artifacts
