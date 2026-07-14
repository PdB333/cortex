#include "routes.h"
#include "../memory/memory.h"
#include "../memory/scan.h"
#include "../memory/provenance.h"
#include "../overlay/overlay.h"
#include "../action/action.h"

#include <nlohmann/json.hpp>
#include <sstream>
#include <iomanip>
#include <cstring>

using json = nlohmann::json;

namespace api {

namespace {

uintptr_t ParseAddress(const json& jaddr) {
    if (jaddr.is_string()) {
        return static_cast<uintptr_t>(std::stoull(jaddr.get<std::string>(), nullptr, 0));
    }
    return static_cast<uintptr_t>(jaddr.get<uint64_t>());
}

std::string BytesToHex(const std::vector<uint8_t>& buf) {
    std::ostringstream hex;
    for (uint8_t b : buf) hex << std::setw(2) << std::setfill('0') << std::hex << static_cast<int>(b);
    return hex.str();
}

std::string HexAddr(uintptr_t address) {
    std::ostringstream out;
    out << "0x" << std::hex << address;
    return out.str();
}

std::vector<uint8_t> HexToBytes(const std::string& hex) {
    std::vector<uint8_t> out;
    std::string s = hex;
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s = s.substr(2);
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        out.push_back(static_cast<uint8_t>(std::stoi(s.substr(i, 2), nullptr, 16)));
    }
    return out;
}

// Reads `type` at `address` into a JSON value. Sets `ok` to false (without
// throwing) if the read type is unknown or the memory access fails.
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

bool WriteTypedValue(uintptr_t address, const std::string& type, const json& jvalue,
                     std::vector<uint8_t>* encoded = nullptr) {
    std::vector<uint8_t> buf;
    if (!EncodeTypedValue(type, jvalue, buf)) return false;
    if (encoded) *encoded = buf;
    return memory::WriteBytes(address, buf);
}

void RecordMemoryUndo(uintptr_t address, const std::vector<uint8_t>& original, const std::string& description) {
    if (original.empty()) return;
    action::Record(description, [address, original] { return memory::WriteBytes(address, original); });
}

} // namespace

void RegisterMemoryRoutes(httplib::Server& svr) {
    svr.Get("/memory/ownership", [](const httplib::Request&, httplib::Response& res) {
        json ranges=json::array();
        for(const auto& range:provenance::List()) ranges.push_back({{"id",range.id},{"base",HexAddr(range.base)},
            {"size",range.size},{"owner",range.owner},{"label",range.label},{"transient",range.transient}});
        res.set_content(json{{"ok",true},{"ranges",ranges}}.dump(),"application/json");
    });
    svr.Post("/memory/read", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            uintptr_t address = ParseAddress(body.at("address"));
            std::string type = body.at("type").get<std::string>();
            int count = body.value("count", 0);

            bool ok = false;
            json value = ReadTypedValue(address, type, count, ok);

            json out;
            out["ok"] = ok;
            if (ok) out["value"] = value;
            else out["error"] = "invalid_memory_or_type";
            res.set_content(out.dump(), "application/json");
            overlay::LogApiCall("POST /memory/read " + type + " @ " + body.at("address").dump());
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Post("/memory/read_batch", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            json results = json::array();
            for (const auto& r : body.at("reads")) {
                uintptr_t address = ParseAddress(r.at("address"));
                std::string type = r.at("type").get<std::string>();
                int count = r.value("count", 0);

                bool ok = false;
                json value = ReadTypedValue(address, type, count, ok);
                json entry;
                entry["ok"] = ok;
                if (ok) entry["value"] = value; else entry["error"] = "invalid_memory_or_type";
                results.push_back(entry);
            }
            res.set_content(json{{"results", results}}.dump(), "application/json");
            overlay::LogApiCall("POST /memory/read_batch (" + std::to_string(results.size()) + ")");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Post("/memory/write_batch", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto mutation = action::LockMutations();
            json body = json::parse(req.body);
            json results = json::array();
            for (const auto& w : body.at("writes")) {
                uintptr_t address = ParseAddress(w.at("address"));
                std::string type = w.at("type").get<std::string>();
                std::vector<uint8_t> encoded, original;
                bool encOk = EncodeTypedValue(type, w.at("value"), encoded);
                bool readOk = encOk && memory::ReadBytes(address, encoded.size(), original);
                bool ok = readOk && memory::WriteBytes(address, encoded);
                if (ok) RecordMemoryUndo(address, original, "memory/write_batch " + type);
                json entry;
                entry["ok"] = ok;
                if (!ok) entry["error"] = "invalid_memory_or_type";
                results.push_back(entry);
            }
            res.set_content(json{{"results", results}}.dump(), "application/json");
            overlay::LogApiCall("POST /memory/write_batch (" + std::to_string(results.size()) + ")");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Post("/memory/write", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto mutation = action::LockMutations();
            json body = json::parse(req.body);
            uintptr_t address = ParseAddress(body.at("address"));
            std::string type = body.at("type").get<std::string>();
            std::vector<uint8_t> encoded, original;
            bool encOk = EncodeTypedValue(type, body.at("value"), encoded);
            bool readOk = encOk && memory::ReadBytes(address, encoded.size(), original);
            bool ok = readOk && memory::WriteBytes(address, encoded);
            if (ok) RecordMemoryUndo(address, original, "memory/write " + type);

            json out;
            out["ok"] = ok;
            if (!ok) out["error"] = "invalid_memory_or_type";
            res.set_content(out.dump(), "application/json");
            overlay::LogApiCall("POST /memory/write " + type + " @ " + body.at("address").dump());
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Get("/memory/dump_typed", [](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_param("address")) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", "missing_address"}}.dump(), "application/json");
            return;
        }
        uintptr_t address = static_cast<uintptr_t>(std::stoull(req.get_param_value("address"), nullptr, 0));
        int size = req.has_param("size") ? std::stoi(req.get_param_value("size")) : 32;
        if (size < 1) size = 1;
        if (size > 4096) size = 4096;

        std::vector<uint8_t> buf;
        if (!memory::ReadBytes(address, static_cast<size_t>(size), buf)) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", "not_readable"}}.dump(), "application/json");
            return;
        }

        // Interprets the leading bytes of the same dump as every numeric type
        // at once -- a la Cheat Engine's hex/typed dump view, so the caller
        // doesn't need N separate /memory/read calls to eyeball what a block
        // of unknown-typed memory might represent.
        json types;
        if (buf.size() >= 1) { types["i8"] = static_cast<int8_t>(buf[0]); types["u8"] = buf[0]; }
        if (buf.size() >= 2) {
            int16_t v; memcpy(&v, buf.data(), 2); types["i16"] = v;
            uint16_t u; memcpy(&u, buf.data(), 2); types["u16"] = u;
        }
        if (buf.size() >= 4) {
            int32_t v; memcpy(&v, buf.data(), 4); types["i32"] = v;
            uint32_t u; memcpy(&u, buf.data(), 4); types["u32"] = u;
            float f; memcpy(&f, buf.data(), 4); types["float"] = f;
        }
        if (buf.size() >= 8) {
            int64_t v; memcpy(&v, buf.data(), 8); types["i64"] = v;
            uint64_t u; memcpy(&u, buf.data(), 8); types["u64"] = u;
            double d; memcpy(&d, buf.data(), 8); types["double"] = d;
        }

        std::string ascii;
        ascii.reserve(buf.size());
        for (uint8_t b : buf) ascii.push_back((b >= 0x20 && b < 0x7F) ? static_cast<char>(b) : '.');

        res.set_content(json{{"ok", true}, {"bytes", BytesToHex(buf)}, {"ascii", ascii}, {"types", types}}.dump(),
                         "application/json");
        overlay::LogApiCall("GET /memory/dump_typed @ " + req.get_param_value("address"));
    });

    svr.Post("/memory/fill", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto mutation = action::LockMutations();
            json body = json::parse(req.body);
            uintptr_t address = ParseAddress(body.at("address"));
            std::vector<uint8_t> pattern = HexToBytes(body.at("pattern").get<std::string>());
            if (pattern.empty()) {
                res.status = 400;
                res.set_content(json{{"ok", false}, {"error", "empty_pattern"}}.dump(), "application/json");
                return;
            }

            size_t length = 0;
            if (body.contains("size")) {
                length = body.at("size").get<size_t>();
            } else if (body.contains("end")) {
                uintptr_t end = ParseAddress(body.at("end"));
                length = end > address ? end - address : 0;
            } else {
                res.status = 400;
                res.set_content(json{{"ok", false}, {"error", "missing_size_or_end"}}.dump(), "application/json");
                return;
            }
            constexpr size_t kMaxFillSize = 64u * 1024 * 1024;
            if (length == 0 || length > kMaxFillSize) {
                res.status = 400;
                res.set_content(json{{"ok", false}, {"error", "invalid_size"}}.dump(), "application/json");
                return;
            }

            std::vector<uint8_t> buf(length);
            for (size_t i = 0; i < length; ++i) buf[i] = pattern[i % pattern.size()];

            std::vector<uint8_t> original;
            bool ok = memory::ReadBytes(address, length, original) && memory::WriteBytes(address, buf);
            if (ok) RecordMemoryUndo(address, original, "memory/fill " + std::to_string(length) + " bytes");
            json out;
            out["ok"] = ok;
            if (ok) out["bytes_written"] = length; else out["error"] = "write_failed";
            res.set_content(out.dump(), "application/json");
            overlay::LogApiCall("POST /memory/fill @ " + body.at("address").dump() + " (" +
                                 std::to_string(length) + " octets)");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Get("/memory/regions", [](const httplib::Request&, httplib::Response& res) {
        auto regions = memscan::ListRegions();
        json arr = json::array();
        for (const auto& r : regions) {
            std::ostringstream addr;
            addr << "0x" << std::hex << r.base;
            arr.push_back({{"base", addr.str()},
                            {"size", r.size},
                            {"protect", r.protect},
                            {"state", r.state},
                            {"type", r.type},
                            {"module", r.moduleName}});
        }
        res.set_content(json{{"ok", true}, {"regions", arr}}.dump(), "application/json");
        overlay::LogApiCall("GET /memory/regions (" + std::to_string(regions.size()) + ")");
    });
}

} // namespace api
