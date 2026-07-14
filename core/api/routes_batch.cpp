#include "routes.h"
#include "../memory/memory.h"
#include "../memory/scan.h"
#include "../disasm/disasm.h"
#include "../analysis/analysis.h"
#include "../struct/structs.h"
#include "../patch/patch.h"
#include "../freeze/freeze.h"
#include "../dissect/dissect.h"
#include "../overlay/overlay.h"
#include "../action/action.h"

#include <nlohmann/json.hpp>
#include <cstring>
#include <iomanip>
#include <sstream>

using json = nlohmann::json;

namespace api {

namespace {

// batch/run's whole reason for existing: the AI was paying one HTTP round-
// trip per primitive op (read this, then read that, then patch, then
// re-read to confirm) even when the steps are known up front and have no
// real dependency on an intermediate LLM decision. This route accepts a
// list of ops and executes every one of them in-process, one function call
// after another, inside a single request handler -- no HTTP, no socket,
// no JSON round-trip between steps. It's still just plumbing to the exact
// same internal namespaces every other routes_*.cpp file calls (memory,
// memscan, disasm, analysis, structs, patch, freeze, dissect); batch/run's
// only job is to sequence several of those calls behind one response.

std::string Hex(uintptr_t a) {
    std::ostringstream s;
    s << "0x" << std::hex << a;
    return s.str();
}

uintptr_t ParseAddress(const json& v) {
    if (v.is_string()) return static_cast<uintptr_t>(std::stoull(v.get<std::string>(), nullptr, 0));
    return static_cast<uintptr_t>(v.get<uint64_t>());
}

uintptr_t ParseAddressAt(const json& body, const char* key) { return ParseAddress(body.at(key)); }

std::string BytesToHex(const std::vector<uint8_t>& buf) {
    std::ostringstream hex;
    for (uint8_t b : buf) hex << std::setw(2) << std::setfill('0') << std::hex << static_cast<int>(b);
    return hex.str();
}

std::vector<uint8_t> HexToBytes(const std::string& hexIn) {
    std::vector<uint8_t> out;
    std::string s = hexIn;
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s = s.substr(2);
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        out.push_back(static_cast<uint8_t>(std::stoi(s.substr(i, 2), nullptr, 16)));
    }
    return out;
}

json ReadTypedValue(uintptr_t address, const std::string& type, int count, bool& ok) {
    std::vector<uint8_t> buf;
    json result;
    ok = false;

    if (type == "i8") { if ((ok = memory::ReadBytes(address, 1, buf))) result = static_cast<int8_t>(buf[0]); }
    else if (type == "u8") { if ((ok = memory::ReadBytes(address, 1, buf))) result = buf[0]; }
    else if (type == "i16") { if ((ok = memory::ReadBytes(address, 2, buf))) { int16_t v; memcpy(&v, buf.data(), 2); result = v; } }
    else if (type == "u16") { if ((ok = memory::ReadBytes(address, 2, buf))) { uint16_t v; memcpy(&v, buf.data(), 2); result = v; } }
    else if (type == "i32") { if ((ok = memory::ReadBytes(address, 4, buf))) { int32_t v; memcpy(&v, buf.data(), 4); result = v; } }
    else if (type == "u32") { if ((ok = memory::ReadBytes(address, 4, buf))) { uint32_t v; memcpy(&v, buf.data(), 4); result = v; } }
    else if (type == "i64") { if ((ok = memory::ReadBytes(address, 8, buf))) { int64_t v; memcpy(&v, buf.data(), 8); result = (v >= -9007199254740991LL && v <= 9007199254740991LL) ? json(v) : json(std::to_string(v)); } }
    else if (type == "u64") { if ((ok = memory::ReadBytes(address, 8, buf))) { uint64_t v; memcpy(&v, buf.data(), 8); result = v <= 9007199254740991ULL ? json(v) : json(std::to_string(v)); } }
    else if (type == "float") { if ((ok = memory::ReadBytes(address, 4, buf))) { float v; memcpy(&v, buf.data(), 4); result = v; } }
    else if (type == "double") { if ((ok = memory::ReadBytes(address, 8, buf))) { double v; memcpy(&v, buf.data(), 8); result = v; } }
    else if (type == "bytes") {
        int n = count > 0 ? count : 16;
        if ((ok = memory::ReadBytes(address, n, buf))) result = BytesToHex(buf);
    } else if (type == "string") {
        int n = count > 0 ? count : 64;
        auto s = memory::ReadString(address, n);
        ok = s.has_value();
        if (ok) result = *s;
    }
    return result;
}

bool EncodeTypedValue(const std::string& type, const json& jvalue, std::vector<uint8_t>& buf) {
    if (type == "i8" || type == "u8") { uint8_t v = static_cast<uint8_t>(jvalue.get<int>()); buf = {v}; }
    else if (type == "i16" || type == "u16") { uint16_t v = static_cast<uint16_t>(jvalue.get<int>()); buf.resize(2); memcpy(buf.data(), &v, 2); }
    else if (type == "i32" || type == "u32") { uint32_t v = static_cast<uint32_t>(jvalue.get<int64_t>()); buf.resize(4); memcpy(buf.data(), &v, 4); }
    else if (type == "i64") { int64_t v = jvalue.is_string() ? std::stoll(jvalue.get<std::string>(), nullptr, 0) : jvalue.get<int64_t>(); buf.resize(8); memcpy(buf.data(), &v, 8); }
    else if (type == "u64") { uint64_t v = jvalue.is_string() ? std::stoull(jvalue.get<std::string>(), nullptr, 0) : jvalue.get<uint64_t>(); buf.resize(8); memcpy(buf.data(), &v, 8); }
    else if (type == "float") { float v = jvalue.get<float>(); buf.resize(4); memcpy(buf.data(), &v, 4); }
    else if (type == "double") { double v = jvalue.get<double>(); buf.resize(8); memcpy(buf.data(), &v, 8); }
    else if (type == "bytes") { buf = HexToBytes(jvalue.get<std::string>()); }
    else return false;

    return !buf.empty();
}

bool WriteTypedValue(uintptr_t address, const std::string& type, const json& jvalue) {
    std::vector<uint8_t> buf;
    if (!EncodeTypedValue(type, jvalue, buf)) return false;
    return memory::WriteBytes(address, buf);
}

json ResolveReference(const std::string& reference, const json& results) {
    if (reference.size() < 2 || reference[0] != '$') return reference;
    size_t pos = 1;
    size_t end = reference.find('.', pos);
    const std::string indexText = reference.substr(pos, end - pos);
    if (indexText.empty() || indexText.find_first_not_of("0123456789") != std::string::npos) return reference;
    size_t index = static_cast<size_t>(std::stoull(indexText));
    if (index >= results.size()) throw std::runtime_error("reference_step_out_of_range");
    json value = results.at(index);
    while (end != std::string::npos) {
        pos = end + 1;
        end = reference.find('.', pos);
        std::string key = reference.substr(pos, end - pos);
        if (value.is_array()) {
            if (key.empty() || key.find_first_not_of("0123456789") != std::string::npos)
                throw std::runtime_error("invalid_array_reference");
            value = value.at(static_cast<size_t>(std::stoull(key)));
        } else {
            value = value.at(key);
        }
    }
    return value;
}

json ResolveReferences(const json& input, const json& results) {
    if (input.is_string()) return ResolveReference(input.get<std::string>(), results);
    if (input.is_array()) {
        json output = json::array();
        for (const auto& value : input) output.push_back(ResolveReferences(value, results));
        return output;
    }
    if (input.is_object()) {
        json output = json::object();
        for (auto it = input.begin(); it != input.end(); ++it) output[it.key()] = ResolveReferences(it.value(), results);
        return output;
    }
    return input;
}

bool IsIrreversibleBatchMutation(const std::string& op) {
    // Removing an already-journalled object changes its identity; replaying
    // it would produce a new id and could invalidate references/older undo
    // entries. Refuse those two operations in an atomic batch.
    return op == "patch_revert" || op == "freeze_remove";
}

// Executes a single op and returns its result JSON. Throws on malformed
// input (missing/wrong-typed fields) -- the caller catches per-op so one
// bad step doesn't abort the rest of the batch.
json RunOp(const json& step) {
    std::string op = step.at("op").get<std::string>();

    if (op == "memory_read") {
        uintptr_t address = ParseAddressAt(step, "address");
        std::string type = step.at("type").get<std::string>();
        int count = step.value("count", 0);
        bool ok = false;
        json value = ReadTypedValue(address, type, count, ok);
        if (!ok) return {{"ok", false}, {"error", "invalid_memory_or_type"}};
        return {{"ok", true}, {"value", value}};
    }

    if (op == "memory_write") {
        uintptr_t address = ParseAddressAt(step, "address");
        std::string type = step.at("type").get<std::string>();
        std::vector<uint8_t> encoded, original;
        bool encodedOk = EncodeTypedValue(type, step.at("value"), encoded);
        bool ok = encodedOk && memory::ReadBytes(address, encoded.size(), original) &&
                  memory::WriteBytes(address, encoded);
        if (!ok) return {{"ok", false}, {"error", "invalid_memory_or_type"}};
        action::Record("batch memory_write " + Hex(address),
                       [address, original] { return memory::WriteBytes(address, original); });
        return {{"ok", true}};
    }

    if (op == "aob_scan") {
        std::string pattern = step.at("pattern").get<std::string>();
        std::string module = step.value("module", std::string());
        bool truncated = false;
        auto hits = memscan::AobScan(pattern, module, truncated);
        json arr = json::array();
        for (auto a : hits) arr.push_back(Hex(a));
        return {{"ok", true}, {"matches", arr}, {"truncated", truncated}};
    }

    if (op == "string_scan") {
        size_t minLength = step.value("min_length", static_cast<size_t>(4));
        std::string contains = step.value("contains", std::string());
        std::string module = step.value("module", std::string());
        std::string encStr = step.value("encoding", std::string("ascii"));
        auto encoding = encStr == "utf16" ? memscan::StringEncoding::Utf16 : memscan::StringEncoding::Ascii;
        bool truncated = false;
        auto hits = memscan::StringScan(minLength, contains, module, encoding, truncated);
        json arr = json::array();
        for (const auto& h : hits) arr.push_back({{"address", Hex(h.address)}, {"value", h.value}});
        return {{"ok", true}, {"matches", arr}, {"truncated", truncated}};
    }

    if (op == "disasm") {
        uintptr_t address = ParseAddressAt(step, "address");
        int count = step.value("count", 10);
        bool ok = false;
        auto instrs = disasm::Disassemble(address, count, ok);
        if (!ok) return {{"ok", false}, {"error", "address_not_readable"}};
        json arr = json::array();
        for (const auto& i : instrs) {
            arr.push_back({{"address", Hex(i.address)}, {"size", i.size}, {"bytes", i.bytes_hex},
                            {"mnemonic", i.mnemonic}, {"text", i.text}});
        }
        return {{"ok", true}, {"instructions", arr}};
    }

    if (op == "analysis_xrefs") {
        uintptr_t target = ParseAddressAt(step, "target");
        std::string module = step.value("module", std::string());
        bool includeData = step.value("include_data", true);
        bool truncated = false;
        auto xrefs = analysis::FindXRefs(target, module, truncated);
        if (includeData) {
            bool dataTruncated = false;
            auto dataXrefs = analysis::FindDataXRefs(target, module, dataTruncated);
            xrefs.insert(xrefs.end(), dataXrefs.begin(), dataXrefs.end());
            truncated = truncated || dataTruncated;
        }
        json arr = json::array();
        for (const auto& x : xrefs) arr.push_back({{"from", Hex(x.from)}, {"type", x.type}});
        return {{"ok", true}, {"xrefs", arr}, {"truncated", truncated}};
    }

    if (op == "analysis_vtable") {
        uintptr_t address = ParseAddressAt(step, "address");
        int maxEntries = step.value("max_entries", 256);
        bool ok = false;
        auto dump = analysis::DumpVTable(address, maxEntries, ok);
        if (!ok) return {{"ok", false}, {"error", "address_not_readable"}};
        json entries = json::array();
        for (const auto& e : dump.entries) entries.push_back({{"slot", Hex(e.slot)}, {"address", Hex(e.address)}});
        return {{"ok", true}, {"entries", entries}, {"type_name", dump.typeName}, {"truncated", dump.truncated}};
    }

    if (op == "struct_read") {
        std::string name = step.at("name").get<std::string>();
        uintptr_t base = ParseAddressAt(step, "address");
        json fields, errors;
        if (!structs::Read(name, base, fields, errors)) return {{"ok", false}, {"error", "unknown_struct"}};
        return {{"ok", true}, {"fields", fields}, {"errors", errors}};
    }

    if (op == "struct_write") {
        std::string name = step.at("name").get<std::string>();
        uintptr_t base = ParseAddressAt(step, "address");
        json originalFields, readErrors;
        if (!structs::Read(name, base, originalFields, readErrors)) return {{"ok", false}, {"error", "unknown_struct"}};
        json originalSubset = json::object();
        for (auto it = step.at("values").begin(); it != step.at("values").end(); ++it)
            if (originalFields.contains(it.key())) originalSubset[it.key()] = originalFields.at(it.key());
        json errors;
        if (!structs::Write(name, base, step.at("values"), errors)) return {{"ok", false}, {"error", "unknown_struct"}};
        if (!originalSubset.empty()) {
            action::Record("batch struct_write " + name, [name, base, originalSubset] {
                json undoErrors;
                return structs::Write(name, base, originalSubset, undoErrors) && undoErrors.empty();
            });
        }
        return {{"ok", true}, {"errors", errors}};
    }

    if (op == "patch_apply") {
        uintptr_t address = ParseAddressAt(step, "address");
        auto bytes = HexToBytes(step.at("bytes").get<std::string>());
        std::string label = step.value("label", std::string());
        int id = patch::Apply(address, bytes, label);
        if (id < 0) return {{"ok", false}, {"error", "patch_failed"}};
        action::Record("batch patch_apply " + Hex(address), [id] { return patch::Revert(id); });
        return {{"ok", true}, {"id", id}};
    }

    if (op == "patch_nop") {
        uintptr_t address = ParseAddressAt(step, "address");
        size_t size = step.at("size").get<size_t>();
        std::string label = step.value("label", std::string());
        int id = patch::Nop(address, size, label);
        if (id < 0) return {{"ok", false}, {"error", "patch_failed"}};
        action::Record("batch patch_nop " + Hex(address), [id] { return patch::Revert(id); });
        return {{"ok", true}, {"id", id}};
    }

    if (op == "patch_revert") {
        int id = step.at("id").get<int>();
        if (!patch::Revert(id)) return {{"ok", false}, {"error", "unknown_patch"}};
        return {{"ok", true}};
    }

    if (op == "freeze_add") {
        uintptr_t address = ParseAddressAt(step, "address");
        std::string type = step.at("type").get<std::string>();
        std::vector<uint8_t> buf;
        // WriteTypedValue writes straight to memory, so its encoding can't be
        // reused here -- freeze::Add needs the encoded bytes without an
        // immediate write. Encode inline instead.
        json probe = step.at("value");
        bool ok = false;
        if (type == "i8" || type == "u8") { uint8_t v = static_cast<uint8_t>(probe.get<int>()); buf = {v}; ok = true; }
        else if (type == "i16" || type == "u16") { uint16_t v = static_cast<uint16_t>(probe.get<int>()); buf.resize(2); memcpy(buf.data(), &v, 2); ok = true; }
        else if (type == "i32" || type == "u32") { uint32_t v = static_cast<uint32_t>(probe.get<int64_t>()); buf.resize(4); memcpy(buf.data(), &v, 4); ok = true; }
        else if (type == "i64") { int64_t v = probe.is_string() ? std::stoll(probe.get<std::string>(), nullptr, 0) : probe.get<int64_t>(); buf.resize(8); memcpy(buf.data(), &v, 8); ok = true; }
        else if (type == "u64") { uint64_t v = probe.is_string() ? std::stoull(probe.get<std::string>(), nullptr, 0) : probe.get<uint64_t>(); buf.resize(8); memcpy(buf.data(), &v, 8); ok = true; }
        else if (type == "float") { float v = probe.get<float>(); buf.resize(4); memcpy(buf.data(), &v, 4); ok = true; }
        else if (type == "double") { double v = probe.get<double>(); buf.resize(8); memcpy(buf.data(), &v, 8); ok = true; }
        else if (type == "bytes") { buf = HexToBytes(probe.get<std::string>()); ok = true; }
        if (!ok) return {{"ok", false}, {"error", "invalid_type"}};
        std::string label = step.value("label", std::string());
        int id = freeze::Add(address, type, buf, label);
        action::Record("batch freeze_add " + Hex(address), [id] { return freeze::Remove(id); });
        return {{"ok", true}, {"id", id}};
    }

    if (op == "freeze_remove") {
        int id = step.at("id").get<int>();
        if (!freeze::Remove(id)) return {{"ok", false}, {"error", "unknown_freeze"}};
        return {{"ok", true}};
    }

    if (op == "dissect_snapshot") {
        uintptr_t address = ParseAddressAt(step, "address");
        size_t size = step.at("size").get<size_t>();
        int id = dissect::TakeSnapshot(address, size);
        if (id < 0) return {{"ok", false}, {"error", "read_failed"}};
        return {{"ok", true}, {"id", id}};
    }

    if (op == "dissect_diff") {
        int idA = step.at("a").get<int>();
        int idB = step.at("b").get<int>();
        std::vector<dissect::DiffRange> diffs;
        std::string error;
        if (!dissect::DiffSnapshots(idA, idB, diffs, error)) return {{"ok", false}, {"error", error}};
        json arr = json::array();
        for (const auto& d : diffs) {
            arr.push_back({{"offset", d.offset}, {"size", d.before.size()},
                            {"before", BytesToHex(d.before)}, {"after", BytesToHex(d.after)}});
        }
        return {{"ok", true}, {"diffs", arr}};
    }

    return {{"ok", false}, {"error", "unknown_op"}};
}

} // namespace

void RegisterBatchRoutes(httplib::Server& svr) {
    svr.Post("/batch/run", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto mutation = action::LockMutations();
            json body = json::parse(req.body);
            const json& ops = body.at("ops");
            if (!ops.is_array()) throw std::runtime_error("ops_must_be_an_array");
            const bool stopOnError = body.value("stop_on_error", false);
            const bool transactional = body.value("transactional", false);
            const uint64_t checkpoint = action::Checkpoint();

            json results = json::array();
            bool allOk = true;
            bool rolledBack = false;
            json rollbackResults = json::array();
            for (const auto& rawStep : ops) {
                json step = ResolveReferences(rawStep, results);
                std::string opName = step.value("op", std::string("?"));
                json r;
                try {
                    if (transactional && IsIrreversibleBatchMutation(opName)) {
                        r = {{"ok", false}, {"error", "operation_not_transactional"}};
                    } else {
                        r = RunOp(step);
                    }
                    r["op"] = opName;
                    results.push_back(r);
                } catch (const std::exception& e) {
                    r = {{"op", opName}, {"ok", false}, {"error", e.what()}};
                    results.push_back(r);
                }

                if (!r.value("ok", false)) {
                    allOk = false;
                    if (transactional) {
                        auto undo = action::RollbackTo(checkpoint);
                        for (const auto& item : undo) {
                            rollbackResults.push_back({{"id", item.id}, {"ok", item.ok}});
                        }
                        rolledBack = true;
                        break;
                    }
                    if (stopOnError) break;
                }
            }

            res.set_content(json{{"ok", allOk}, {"results", results}, {"transactional", transactional},
                                 {"rolled_back", rolledBack}, {"rollback_results", rollbackResults}}.dump(),
                            "application/json");
            overlay::LogApiCall("POST /batch/run (" + std::to_string(results.size()) + " ops)");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });
}

} // namespace api
