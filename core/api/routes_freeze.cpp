#include "routes.h"
#include "../process/address.h"
#include "../freeze/freeze.h"
#include "../overlay/overlay.h"
#include "../action/action.h"

#include <nlohmann/json.hpp>
#include <sstream>
#include <iomanip>
#include <cstring>

using json = nlohmann::json;

namespace api {

namespace {

uintptr_t ParseAddress(const json& jaddr) { return process::ResolveAddress(jaddr); }

std::string HexAddr(uintptr_t a) {
    std::ostringstream s;
    s << "0x" << std::hex << a;
    return s.str();
}

std::string BytesToHex(const std::vector<uint8_t>& buf) {
    std::ostringstream hex;
    for (uint8_t b : buf) hex << std::setw(2) << std::setfill('0') << std::hex << static_cast<int>(b);
    return hex.str();
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

// Encodes `jvalue` as raw bytes for `type`, for the freeze thread to
// rewrite verbatim on every tick (it never re-derives from JSON).
bool EncodeTypedValue(const std::string& type, const json& jvalue, std::vector<uint8_t>& out) {
    if (type == "i8" || type == "u8") { uint8_t v = static_cast<uint8_t>(jvalue.get<int>()); out = {v}; }
    else if (type == "i16" || type == "u16") { uint16_t v = static_cast<uint16_t>(jvalue.get<int>()); out.resize(2); memcpy(out.data(), &v, 2); }
    else if (type == "i32" || type == "u32") { uint32_t v = static_cast<uint32_t>(jvalue.get<int64_t>()); out.resize(4); memcpy(out.data(), &v, 4); }
    else if (type == "i64") { int64_t v = jvalue.is_string() ? std::stoll(jvalue.get<std::string>(), nullptr, 0) : jvalue.get<int64_t>(); out.resize(8); memcpy(out.data(), &v, 8); }
    else if (type == "u64") { uint64_t v = jvalue.is_string() ? std::stoull(jvalue.get<std::string>(), nullptr, 0) : jvalue.get<uint64_t>(); out.resize(8); memcpy(out.data(), &v, 8); }
    else if (type == "float") { float v = jvalue.get<float>(); out.resize(4); memcpy(out.data(), &v, 4); }
    else if (type == "double") { double v = jvalue.get<double>(); out.resize(8); memcpy(out.data(), &v, 8); }
    else if (type == "bytes") { out = HexToBytes(jvalue.get<std::string>()); }
    else return false;
    return true;
}

} // namespace

void RegisterFreezeRoutes(httplib::Server& svr) {
    svr.Post("/freeze", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto mutation = action::LockMutations();
            json body = json::parse(req.body);
            uintptr_t address = ParseAddress(body.at("address"));
            std::string type = body.at("type").get<std::string>();
            std::string label = body.value("label", std::string(""));
            int64_t ttlMs = body.value("ttl_ms", static_cast<int64_t>(0));

            std::vector<uint8_t> bytes;
            if (!EncodeTypedValue(type, body.at("value"), bytes)) {
                res.status = 400;
                res.set_content(json{{"ok", false}, {"error", "invalid_type"}}.dump(), "application/json");
                return;
            }

            int id = freeze::Add(address, type, bytes, label, ttlMs);
            action::Record("freeze/add " + HexAddr(address), [id] { return freeze::Remove(id); });
            res.set_content(json{{"ok", true}, {"id", id}}.dump(), "application/json");
            overlay::LogApiCall("POST /freeze " + type + " @ " + HexAddr(address));
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Delete(R"(/freeze/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
        auto mutation = action::LockMutations();
        int id = std::stoi(req.matches[1]);
        bool ok = freeze::Remove(id);
        res.status = ok ? 200 : 404;
        res.set_content(json{{"ok", ok}}.dump(), "application/json");
        overlay::LogApiCall("DELETE /freeze/" + std::to_string(id));
    });

    svr.Get("/freeze/list", [](const httplib::Request&, httplib::Response& res) {
        json arr = json::array();
        for (const auto& f : freeze::List()) {
            arr.push_back({{"id", f.id},
                            {"address", HexAddr(f.address)},
                            {"type", f.type},
                            {"value_bytes", BytesToHex(f.valueBytes)},
                            {"label", f.label},
                            {"ttl_ms_remaining", f.ttlMsRemaining}});
        }
        res.set_content(json{{"ok", true}, {"freezes", arr}}.dump(), "application/json");
        overlay::LogApiCall("GET /freeze/list");
    });
}

} // namespace api
