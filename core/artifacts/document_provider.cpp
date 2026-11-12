#include "document_provider.h"
#include "bounded_reader.h"
#include "inflate.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <set>

namespace cortex::artifacts {
namespace {
using bounded::Error;
using bounded::View;

// --------------------------------------------------------------------- PDF ---

struct Obj {
    enum Kind { Null, Bool, Number, String, Name, Array, Dict, Ref } kind = Null;
    double number = 0;
    bool flag = false;
    std::string text;  // raw string bytes, or the name without '/'
    uint32_t ref = 0, gen = 0;
    std::vector<Obj> items;
    std::vector<std::pair<std::string, Obj>> entries;
    const Obj* Get(const std::string& key) const {
        for (const auto& [k, v] : entries) if (k == key) return &v;
        return nullptr;
    }
    bool IsName(const std::string& n) const { return kind == Name && text == n; }
};

bool Space(uint8_t c) { return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32; }
bool Delim(uint8_t c) { return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' || c == '/' || c == '%'; }

struct PdfLexer {
    const std::vector<uint8_t>& b;
    size_t at = 0;
    size_t budget = 5000000;

    void Skip() {
        while (at < b.size()) {
            if (Space(b[at])) { ++at; continue; }
            if (b[at] == '%') { while (at < b.size() && b[at] != '\n' && b[at] != '\r') ++at; continue; }
            break;
        }
    }
    bool Peek(const char* word) const {
        const size_t n = std::strlen(word);
        return at + n <= b.size() && std::memcmp(b.data() + at, word, n) == 0;
    }
    std::string Token() {
        const size_t start = at;
        while (at < b.size() && !Space(b[at]) && !Delim(b[at])) ++at;
        return std::string(reinterpret_cast<const char*>(b.data() + start), at - start);
    }
    static int Hex(uint8_t c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

    Obj Value(unsigned depth = 0) {
        if (depth > 64) throw Error("pdf_nesting_limit");
        if (!budget--) throw Error("pdf_token_budget");
        Skip();
        Obj o;
        if (at >= b.size()) return o;
        const uint8_t c = b[at];
        if (c == '/') {
            ++at;
            const std::string raw = Token();
            o.kind = Obj::Name;
            for (size_t i = 0; i < raw.size(); ++i) {
                if (raw[i] == '#' && i + 2 < raw.size() && Hex(raw[i + 1]) >= 0 && Hex(raw[i + 2]) >= 0) {
                    o.text += static_cast<char>(Hex(raw[i + 1]) * 16 + Hex(raw[i + 2])); i += 2;
                } else o.text += raw[i];
            }
        } else if (c == '(') {
            ++at;
            o.kind = Obj::String;
            int nest = 1;
            while (at < b.size()) {
                const uint8_t ch = b[at++];
                if (ch == '\\' && at < b.size()) {
                    const uint8_t e = b[at++];
                    switch (e) {
                        case 'n': o.text += '\n'; break; case 'r': o.text += '\r'; break; case 't': o.text += '\t'; break;
                        case 'b': o.text += '\b'; break; case 'f': o.text += '\f'; break;
                        case '\r': if (at < b.size() && b[at] == '\n') ++at; break;
                        case '\n': break;
                        default:
                            if (e >= '0' && e <= '7') {
                                int v = e - '0';
                                for (int k = 0; k < 2 && at < b.size() && b[at] >= '0' && b[at] <= '7'; ++k) v = v * 8 + (b[at++] - '0');
                                o.text += static_cast<char>(v & 0xFF);
                            } else o.text += static_cast<char>(e);
                    }
                } else if (ch == '(') { ++nest; o.text += '('; }
                else if (ch == ')') { if (--nest == 0) break; o.text += ')'; }
                else o.text += static_cast<char>(ch);
                if (o.text.size() > (16u << 20)) throw Error("pdf_string_limit");
            }
        } else if (c == '<' && at + 1 < b.size() && b[at + 1] == '<') {
            at += 2;
            o.kind = Obj::Dict;
            while (true) {
                Skip();
                if (at >= b.size()) throw Error("pdf_dictionary_unterminated");
                if (Peek(">>")) { at += 2; break; }
                if (b[at] != '/') { // malformed key: skip a token to make progress
                    Token(); if (at < b.size() && Delim(b[at]) && b[at] != '/') ++at;
                    continue;
                }
                const Obj key = Value(depth + 1);
                o.entries.emplace_back(key.text, Value(depth + 1));
                if (o.entries.size() > 100000) throw Error("pdf_dictionary_limit");
            }
        } else if (c == '<') {
            ++at;
            o.kind = Obj::String;
            int high = -1;
            while (at < b.size() && b[at] != '>') {
                const int h = Hex(b[at++]);
                if (h < 0) continue;
                if (high < 0) high = h; else { o.text += static_cast<char>(high * 16 + h); high = -1; }
            }
            if (high >= 0) o.text += static_cast<char>(high * 16);
            ++at;
        } else if (c == '[') {
            ++at;
            o.kind = Obj::Array;
            while (true) {
                Skip();
                if (at >= b.size()) throw Error("pdf_array_unterminated");
                if (b[at] == ']') { ++at; break; }
                const size_t before = at;
                o.items.push_back(Value(depth + 1));
                if (at == before) ++at;
                if (o.items.size() > 1000000) throw Error("pdf_array_limit");
            }
        } else if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.') {
            const std::string t = Token();
            o.kind = Obj::Number;
            o.number = std::strtod(t.c_str(), nullptr);
            // "num gen R" is a reference.
            const size_t save = at;
            Skip();
            if (at < b.size() && b[at] >= '0' && b[at] <= '9' && t.find('.') == std::string::npos && t[0] != '-' && t[0] != '+') {
                const std::string g = Token();
                Skip();
                if (at < b.size() && b[at] == 'R' && (at + 1 >= b.size() || Space(b[at + 1]) || Delim(b[at + 1]))) {
                    ++at;
                    o.kind = Obj::Ref; o.ref = static_cast<uint32_t>(std::strtoul(t.c_str(), nullptr, 10));
                    o.gen = static_cast<uint32_t>(std::strtoul(g.c_str(), nullptr, 10));
                    return o;
                }
            }
            at = save;
        } else if (c == ')' || c == '>' || c == ']' || c == '}' || c == '{') {
            ++at;
        } else {
            const std::string word = Token();
            if (word == "true" || word == "false") { o.kind = Obj::Bool; o.flag = word == "true"; }
            else if (word.empty()) ++at;
        }
        return o;
    }
};

std::string PdfText(const std::string& raw) {
    const auto* p = reinterpret_cast<const uint8_t*>(raw.data());
    if (raw.size() >= 2 && p[0] == 0xFE && p[1] == 0xFF) return bounded::Utf16(View(p + 2, (raw.size() - 2) & ~size_t(1)), false);
    if (raw.size() >= 2 && p[0] == 0xFF && p[1] == 0xFE) return bounded::Utf16(View(p + 2, (raw.size() - 2) & ~size_t(1)), true);
    std::string out;  // PDFDocEncoding, read as Latin-1 (exact for ASCII)
    for (uint8_t c : raw) bounded::AppendCodePoint(out, c);
    return out;
}

struct PdfObject { Obj value; uint64_t streamStart = 0, streamLength = 0; bool stream = false, lengthKnown = false; };

struct Pdf {
    std::vector<uint8_t> bytes;
    std::map<uint32_t, PdfObject> objects;
    std::vector<Obj> trailers;
    uint64_t objectStreams = 0, xrefStreams = 0, decodeFailures = 0;

    const Obj* Resolve(const Obj* o, unsigned depth = 0) const {
        while (o && o->kind == Obj::Ref && depth++ < 16) {
            const auto it = objects.find(o->ref);
            if (it == objects.end()) return nullptr;
            o = &it->second.value;
        }
        return o;
    }

    bool Decode(const PdfObject& po, std::vector<uint8_t>& out) {
        if (!po.stream) return false;
        const Obj* filter = Resolve(po.value.Get("Filter"));
        const uint8_t* data = bytes.data() + po.streamStart;
        if (!filter) { out.assign(data, data + po.streamLength); return true; }
        const Obj* name = filter->kind == Obj::Array && filter->items.size() == 1 ? &filter->items[0] : filter;
        if (!name->IsName("FlateDecode")) return false;
        const Obj* parms = Resolve(po.value.Get("DecodeParms"));
        if (parms && parms->kind == Obj::Dict) { const Obj* pred = parms->Get("Predictor"); if (pred && pred->number > 1) return false; }
        if (po.streamLength < 2 || (data[0] & 0x0F) != 8) { ++decodeFailures; return false; }
        size_t consumed = 0; std::string error;
        if (!InflatePrefix(data + 2, po.streamLength - 2, out, 64u << 20, consumed, error)) { ++decodeFailures; return false; }
        return true;
    }

    void Scan() {
        PdfLexer lex{bytes};
        for (size_t i = 1; i + 3 <= bytes.size(); ++i) {
            if (bytes[i] != 'o' || bytes[i + 1] != 'b' || bytes[i + 2] != 'j') continue;
            if (!Space(bytes[i - 1]) || (i + 3 < bytes.size() && !Space(bytes[i + 3]) && !Delim(bytes[i + 3]))) continue;
            size_t j = i - 1;
            while (j > 0 && Space(bytes[j])) --j;
            size_t genEnd = j + 1;
            while (j > 0 && bytes[j] >= '0' && bytes[j] <= '9') --j;
            if (j + 1 == genEnd || !Space(bytes[j])) continue;
            const size_t genStart = j + 1;
            while (j > 0 && Space(bytes[j])) --j;
            const size_t numEnd = j + 1;
            while (j > 0 && bytes[j] >= '0' && bytes[j] <= '9') --j;
            const size_t numStart = (bytes[j] >= '0' && bytes[j] <= '9') ? j : j + 1;
            if (numStart == numEnd || (numStart > 0 && !Space(bytes[numStart - 1]))) continue;
            const uint32_t num = static_cast<uint32_t>(std::strtoul(std::string(bytes.begin() + numStart, bytes.begin() + numEnd).c_str(), nullptr, 10));
            (void)genStart;
            lex.at = i + 3;
            PdfObject po;
            po.value = lex.Value();
            lex.Skip();
            if (lex.Peek("stream")) {
                lex.at += 6;
                if (lex.at < bytes.size() && bytes[lex.at] == '\r') ++lex.at;
                if (lex.at < bytes.size() && bytes[lex.at] == '\n') ++lex.at;
                po.stream = true;
                po.streamStart = lex.at;
                const Obj* length = po.value.Get("Length");
                if (length && length->kind == Obj::Number && length->number >= 0 && po.streamStart + uint64_t(length->number) <= bytes.size()) {
                    po.streamLength = static_cast<uint64_t>(length->number); po.lengthKnown = true;
                }
            }
            if (objects.size() > 2000000) throw Error("pdf_object_limit");
            objects[num] = std::move(po);
            if (lex.at > i) i = lex.at - 1;
        }
        // Lengths given as references, then any still unknown: up to "endstream".
        for (auto& [num, po] : objects) {
            if (!po.stream || po.lengthKnown) continue;
            const Obj* length = Resolve(po.value.Get("Length"));
            if (length && length->kind == Obj::Number && length->number >= 0 && po.streamStart + uint64_t(length->number) <= bytes.size()) {
                po.streamLength = static_cast<uint64_t>(length->number);
            } else {
                static const char end[] = "endstream";
                const auto it = std::search(bytes.begin() + po.streamStart, bytes.end(), end, end + 9);
                po.streamLength = static_cast<uint64_t>(it - bytes.begin()) - po.streamStart;
            }
            po.lengthKnown = true;
        }
        // Compressed object streams.
        std::vector<std::pair<uint32_t, Obj>> inner;
        for (auto& [num, po] : objects) {
            if (po.value.kind != Obj::Dict) continue;
            const Obj* type = po.value.Get("Type");
            if (type && type->IsName("XRef")) { ++xrefStreams; trailers.push_back(po.value); continue; }
            if (!type || !type->IsName("ObjStm")) continue;
            ++objectStreams;
            std::vector<uint8_t> data;
            if (!Decode(po, data)) continue;
            const Obj* n = po.value.Get("N"); const Obj* first = po.value.Get("First");
            if (!n || !first || n->number < 0 || n->number > 100000) continue;
            PdfLexer l{data};
            std::vector<std::pair<uint32_t, size_t>> index;
            for (int k = 0; k < static_cast<int>(n->number); ++k) {
                const Obj a = l.Value(), b = l.Value();
                if (a.kind != Obj::Number || b.kind != Obj::Number) break;
                index.emplace_back(static_cast<uint32_t>(a.number), static_cast<size_t>(first->number + b.number));
            }
            for (const auto& [objNum, offset] : index) {
                if (offset >= data.size()) continue;
                l.at = offset;
                inner.emplace_back(objNum, l.Value());
            }
        }
        for (auto& [num, value] : inner) if (!objects.count(num)) { PdfObject po; po.value = std::move(value); objects[num] = std::move(po); }
        // Classic trailers.
        PdfLexer t{bytes};
        static const char word[] = "trailer";
        for (auto it = bytes.begin(); (it = std::search(it, bytes.end(), word, word + 7)) != bytes.end(); it += 7) {
            t.at = static_cast<size_t>(it - bytes.begin()) + 7;
            const Obj dict = t.Value();
            if (dict.kind == Obj::Dict) trailers.push_back(dict);
            if (trailers.size() > 4096) break;
        }
    }
};

json PdfInspect(const ByteSource& source, const InspectOptions& options) {
    Pdf pdf;
    pdf.bytes = bounded::Read(source, 0, source.Size(), 256u << 20);
    std::string version;
    {
        static const char head[] = "%PDF-";
        const auto end = pdf.bytes.begin() + std::min<size_t>(pdf.bytes.size(), 1024);
        const auto it = std::search(pdf.bytes.begin(), end, head, head + 5);
        if (it == end) throw Error("pdf_header");
        for (auto p = it + 5; p != pdf.bytes.end() && version.size() < 8 && ((*p >= '0' && *p <= '9') || *p == '.'); ++p) version += static_cast<char>(*p);
    }
    pdf.Scan();
    // Encryption and document information from the trailers (last wins).
    const Obj* encrypt = nullptr; const Obj* info = nullptr;
    for (const auto& t : pdf.trailers) {
        if (const Obj* e = t.Get("Encrypt")) encrypt = e;
        if (const Obj* i = t.Get("Info")) info = i;
    }
    const bool encrypted = encrypt != nullptr;
    auto text = [&](const Obj* o) -> json {
        o = pdf.Resolve(o);
        if (!o || o->kind != Obj::String) return nullptr;
        if (encrypted) return "(encrypted)";
        std::string s = PdfText(o->text);
        s = bounded::Clip(s, 4096);
        return s;
    };
    json result = {{"format", "pdf"}, {"version", version}, {"objects", pdf.objects.size()},
                   {"object_streams", pdf.objectStreams}, {"xref_streams", pdf.xrefStreams}, {"encrypted", encrypted}};
    {
        static const char eof[] = "%%EOF";
        uint64_t n = 0;
        for (auto it = pdf.bytes.begin(); (it = std::search(it, pdf.bytes.end(), eof, eof + 5)) != pdf.bytes.end(); it += 5) ++n;
        result["revisions"] = n;
    }
    if (encrypted) {
        const Obj* e = pdf.Resolve(encrypt);
        json enc = json::object();
        if (e && e->kind == Obj::Dict) {
            if (const Obj* f = e->Get("Filter")) enc["filter"] = bounded::Utf8(reinterpret_cast<const uint8_t*>(f->text.data()), f->text.size());
            if (const Obj* v = e->Get("V")) enc["v"] = v->number;
            if (const Obj* r = e->Get("R")) enc["r"] = r->number;
        }
        enc["note"] = "strings and streams are not decrypted";
        result["encryption"] = enc;
    }
    json infoJson = json::object();
    if (const Obj* i = pdf.Resolve(info); i && i->kind == Obj::Dict)
        for (const char* key : {"Title", "Author", "Subject", "Keywords", "Creator", "Producer", "CreationDate", "ModDate"})
            if (const Obj* v = i->Get(key)) { const json t = text(v); if (!t.is_null()) infoJson[key] = t; }
    result["info"] = infoJson;

    uint64_t pages = 0, embeddedStreams = 0, additional = 0;
    bool openAction = false, acroform = false, xfa = false;
    json js = json::array(), uris = json::array(), launches = json::array(), files = json::array();
    std::map<std::string, uint64_t> actions;
    std::function<void(const Obj&, uint32_t, unsigned)> walk = [&](const Obj& o, uint32_t owner, unsigned depth) {
        if (depth > 32) return;
        if (o.kind == Obj::Array) { for (const auto& item : o.items) walk(item, owner, depth + 1); return; }
        if (o.kind != Obj::Dict) return;
        const Obj* s = o.Get("S");
        if (s && s->kind == Obj::Name) {
            static const std::set<std::string> kinds = {"JavaScript", "Launch", "URI", "SubmitForm", "ImportData", "GoToR", "GoToE", "RichMediaExecute", "ResetForm", "Rendition"};
            if (kinds.count(s->text)) ++actions[s->text];
            if (s->text == "URI") { const json u = text(o.Get("URI")); if (!u.is_null() && uris.size() < options.maxEntries) uris.push_back(u); }
            if (s->text == "Launch") {
                const Obj* f = pdf.Resolve(o.Get("F"));
                json target = f && f->kind == Obj::String ? text(f) : f && f->kind == Obj::Dict ? text(f->Get("F")) : json(nullptr);
                if (launches.size() < options.maxEntries) launches.push_back(target);
            }
        }
        if (const Obj* code = o.Get("JS")) {
            json row = {{"object", owner}};
            const Obj* c = pdf.Resolve(code);
            if (c && c->kind == Obj::String) row["script"] = text(c);
            else if (code->kind == Obj::Ref) {
                const auto it = pdf.objects.find(code->ref);
                std::vector<uint8_t> data;
                if (encrypted) row["script"] = "(encrypted)";
                else if (it != pdf.objects.end() && pdf.Decode(it->second, data))
                    row["script"] = bounded::Utf8(data.data(), std::min<size_t>(data.size(), 4096));
                else row["script"] = nullptr;
            }
            if (js.size() < options.maxEntries) js.push_back(std::move(row));
        }
        if (o.Get("AA")) ++additional;
        if (o.Get("EF")) {
            const json name = !text(o.Get("UF")).is_null() ? text(o.Get("UF")) : text(o.Get("F"));
            if (files.size() < options.maxEntries) files.push_back(name);
        }
        if (o.Get("XFA")) xfa = true;
        for (const auto& [k, v] : o.entries) walk(v, owner, depth + 1);
    };
    for (const auto& [num, po] : pdf.objects) {
        const Obj& v = po.value;
        if (v.kind == Obj::Dict) {
            const Obj* type = v.Get("Type");
            if (type && type->IsName("Page")) ++pages;
            if (type && type->IsName("EmbeddedFile")) ++embeddedStreams;
            if (type && type->IsName("Catalog")) {
                if (v.Get("OpenAction")) openAction = true;
                if (v.Get("AcroForm")) acroform = true;
            }
        }
        walk(v, num, 0);
    }
    result["pages"] = pages;
    result["javascript"] = js;
    result["actions"] = actions;
    result["uris"] = uris;
    result["launch_targets"] = launches;
    result["embedded_files"] = files;
    result["embedded_file_streams"] = embeddedStreams;
    result["open_action"] = openAction;
    result["additional_actions"] = additional;
    result["acroform"] = acroform;
    result["xfa"] = xfa;
    result["executed"] = false;
    if (pdf.decodeFailures) result["warnings"] = json::array({"undecodable_streams: " + std::to_string(pdf.decodeFailures)});
    return result;
}

// --------------------------------------------------------------------- LNK ---

// A NUL-terminated UTF-16LE string at `at` (bounded by the view).
std::string Utf16Z(const View& v, uint64_t at) {
    uint64_t end = at;
    while (end + 1 < v.Size() && (v.U8(end) || v.U8(end + 1))) { end += 2; if (end - at > 8192) break; }
    return bounded::Utf16(v.Slice(at, end - at));
}

std::string LnkString(const View& v, uint64_t& at, bool unicode) {
    const uint16_t count = v.U16(at);
    at += 2;
    const uint64_t bytes = uint64_t(count) * (unicode ? 2 : 1);
    std::string s = unicode ? bounded::Utf16(v.Slice(at, bytes)) : v.Text(at, bytes);
    at += bytes;
    return s;
}

json FileTime(uint64_t ft) {
    if (!ft) return nullptr;
    return static_cast<int64_t>(ft / 10000000ULL) - 11644473600LL;  // Unix seconds
}

json Lnk(const ByteSource& source) {
    const auto bytes = bounded::Read(source, 0, source.Size(), 16u << 20);
    const View v(bytes);
    static const uint8_t clsid[16] = {0x01, 0x14, 0x02, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46};
    if (v.U32(0) != 0x4C || std::memcmp(v.Data(4, 16), clsid, 16) != 0) throw Error("lnk_header");
    const uint32_t flags = v.U32(20);
    const bool unicode = flags & 0x80;
    static const char* flagNames[] = {"HasLinkTargetIDList", "HasLinkInfo", "HasName", "HasRelativePath", "HasWorkingDir",
                                      "HasArguments", "HasIconLocation", "IsUnicode", "ForceNoLinkInfo", "HasExpString",
                                      "RunInSeparateProcess", "Unused1", "HasDarwinID", "RunAsUser", "HasExpIcon", "NoPidlAlias",
                                      "Unused2", "RunWithShimLayer", "ForceNoLinkTrack", "EnableTargetMetadata",
                                      "DisableLinkPathTracking", "DisableKnownFolderTracking", "DisableKnownFolderAlias",
                                      "AllowLinkToLink", "UnaliasOnSave", "PreferEnvironmentPath", "KeepLocalIDListForUNCTarget"};
    json flagList = json::array();
    for (int i = 0; i < 27; ++i) if (flags & (1u << i)) flagList.push_back(flagNames[i]);
    static const char* show[] = {"", "normal", "", "maximized", "", "", "", "minimized_no_active"};
    const uint32_t showCommand = v.U32(60);
    json result = {{"format", "lnk"}, {"flags", flagList}, {"file_attributes", v.U32(24)},
                   {"creation_time", FileTime(v.U64(28))}, {"access_time", FileTime(v.U64(36))}, {"write_time", FileTime(v.U64(44))},
                   {"target_size", v.U32(52)}, {"icon_index", static_cast<int32_t>(v.U32(56))},
                   {"show_command", showCommand < 8 && show[showCommand][0] ? show[showCommand] : std::to_string(showCommand)},
                   {"hotkey", v.U16(64)}};
    uint64_t at = 0x4C;
    if (flags & 0x1) {
        const uint16_t size = v.U16(at);
        const View list = v.Slice(at + 2, size);
        uint64_t q = 0, items = 0;
        std::string path;
        json kinds = json::array();
        while (q + 2 <= list.Size()) {
            const uint16_t n = list.U16(q);
            if (!n) break;
            if (n < 3) throw Error("lnk_idlist_item_invalid");
            const View item = list.Slice(q, n);
            const uint8_t type = item.U8(2), cls = type & 0x70;
            // Shell items: root folder (GUID), volume ("C:\"), file entries (short name at 14).
            if (type == 0x1F) kinds.push_back("root_folder");
            else if (cls == 0x20) { kinds.push_back("volume"); path = item.CString(3, nullptr, 256); }
            else if (cls == 0x30 && n > 14) {
                kinds.push_back(type & 1 ? "directory" : "file");
                const std::string name = item.CString(14, nullptr, 512);
                if (!path.empty() && path.back() != '\\') path += '\\';
                path += name;
            } else if (cls == 0x40) kinds.push_back("network");
            else if (cls == 0x60) kinds.push_back("uri");
            else kinds.push_back("other");
            q += n; ++items;
        }
        result["id_list_items"] = items;
        result["id_list_kinds"] = kinds;
        if (!path.empty()) result["target_path"] = path;
        at += 2 + uint64_t(size);
    }
    if ((flags & 0x2) && !(flags & 0x100)) {
        const uint32_t size = v.U32(at);
        const View info = v.Slice(at, size);
        const uint32_t header = info.U32(4), liFlags = info.U32(8);
        json li = json::object();
        if (liFlags & 1) {
            const uint32_t volOff = info.U32(12), baseOff = info.U32(16);
            const View vol = info.Slice(volOff, info.U32(volOff));
            static const char* drives[] = {"unknown", "no_root_dir", "removable", "fixed", "remote", "cdrom", "ramdisk"};
            const uint32_t type = vol.U32(4);
            li["drive_type"] = type < 7 ? drives[type] : "unknown";
            li["drive_serial"] = HexU64(vol.U32(8));
            const uint32_t labelOff = vol.U32(12);
            li["volume_label"] = labelOff == 0x14 ? Utf16Z(vol, vol.U32(16)) : vol.CString(labelOff, nullptr, 256);
            li["local_base_path"] = info.CString(baseOff, nullptr, 4096);
            if (header >= 0x24) {
                const uint32_t uOff = info.U32(28);
                if (uOff) li["local_base_path_unicode"] = Utf16Z(info, uOff);
            }
        }
        if (liFlags & 2) {
            const uint32_t netOff = info.U32(20);
            const View net = info.Slice(netOff, info.U32(netOff));
            li["network_share"] = net.CString(net.U32(8), nullptr, 4096);
        }
        const uint32_t suffixOff = info.U32(24);
        if (suffixOff) li["common_path_suffix"] = info.CString(suffixOff, nullptr, 4096);
        result["link_info"] = li;
        at += size;
    }
    static const std::pair<uint32_t, const char*> strings[] = {{0x4, "name"}, {0x8, "relative_path"}, {0x10, "working_dir"},
                                                               {0x20, "arguments"}, {0x40, "icon_location"}};
    json data = json::object();
    for (const auto& [bit, name] : strings) if (flags & bit) data[name] = LnkString(v, at, unicode);
    result["strings"] = data;
    json blocks = json::array();
    while (at + 4 <= v.Size()) {
        const uint32_t size = v.U32(at);
        if (size < 4) break;
        const View block = v.Slice(at, size);
        const uint32_t sig = block.U32(4);
        json row = {{"signature", HexU64(sig)}, {"size", size}};
        switch (sig) {
            case 0xA0000001: row["kind"] = "environment"; row["target"] = block.CString(8, nullptr, 260);
                if (size >= 788) row["target_unicode"] = Utf16Z(block.Slice(268, 520), 0);
                result["environment_target"] = row["target"]; break;
            case 0xA0000002: row["kind"] = "console"; break;
            case 0xA0000003: row["kind"] = "tracker"; row["machine_id"] = block.CString(16, nullptr, 16); result["tracker_machine_id"] = row["machine_id"]; break;
            case 0xA0000004: row["kind"] = "console_codepage"; break;
            case 0xA0000005: row["kind"] = "special_folder"; break;
            case 0xA0000006: row["kind"] = "darwin"; break;
            case 0xA0000007: row["kind"] = "icon_environment"; row["target"] = block.CString(8, nullptr, 260); break;
            case 0xA0000008: row["kind"] = "shim"; break;
            case 0xA0000009: row["kind"] = "property_store"; break;
            case 0xA000000B: row["kind"] = "known_folder"; break;
            case 0xA000000C: row["kind"] = "vista_id_list"; break;
            default: row["kind"] = "unknown"; break;
        }
        if (blocks.size() < 64) blocks.push_back(std::move(row));
        at += size;
    }
    result["extra_data"] = blocks;
    return result;
}

} // namespace

std::vector<Capability> PdfProvider::Capabilities() const {
    return {{"static", CapabilityStatus::Experimental, "cortex_format_more_tests; pypdf-written documents checked against pypdf",
             "objects (incl. FlateDecode object streams), pages, info, JavaScript, actions, URIs, embedded files, forms, encryption flag"},
            {"decompile", CapabilityStatus::Unavailable, "", "not applicable"},
            {"live", CapabilityStatus::Unavailable, "", "scripts are listed, never run; encrypted content is not decrypted"}};
}
int PdfProvider::Detect(const ByteSource& source) const {
    const auto head = source.ReadSome(0, 1024);
    static const char m[] = "%PDF-";
    return std::search(head.begin(), head.end(), m, m + 5) != head.end() ? 70 : 0;
}
json PdfProvider::Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const {
    error.clear();
    try { return PdfInspect(source, options); } catch (const Error& e) { error = e.what(); return nullptr; }
}

std::vector<Capability> LnkProvider::Capabilities() const {
    return {{"static", CapabilityStatus::Experimental, "cortex_format_more_tests; pylnk3-written links checked against LnkParse3",
             "header and times, link info (paths, volume), string data, extra-data blocks"},
            {"decompile", CapabilityStatus::Unavailable, "", "not applicable"},
            {"live", CapabilityStatus::Unavailable, "", "the link target is never opened"}};
}
int LnkProvider::Detect(const ByteSource& source) const {
    uint32_t size = 0, clsid = 0; return ReadU32(source, 0, size) && ReadU32(source, 4, clsid) && size == 0x4C && clsid == 0x00021401 ? 70 : 0;
}
json LnkProvider::Inspect(const ByteSource& source, const InspectOptions&, std::string& error) const {
    error.clear();
    try { return Lnk(source); } catch (const Error& e) { error = e.what(); return nullptr; }
}

} // namespace cortex::artifacts
