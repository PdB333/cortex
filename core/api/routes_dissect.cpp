#include "routes.h"
#include "../process/address.h"
#include "../dissect/dissect.h"
#include "../overlay/overlay.h"

#include <nlohmann/json.hpp>
#include <cstring>
#include <iomanip>
#include <sstream>

using json = nlohmann::json;

namespace api {

namespace {

std::string Hex(uintptr_t a) {
    std::ostringstream s;
    s << "0x" << std::hex << a;
    return s.str();
}

uintptr_t ParseAddress(const json& body, const char* key) { return process::ResolveAddress(body.at(key)); }

std::string BytesToHex(const std::vector<uint8_t>& buf) {
    std::ostringstream hex;
    for (uint8_t b : buf) hex << std::setw(2) << std::setfill('0') << std::hex << static_cast<int>(b);
    return hex.str();
}

// For ranges of exactly 1/2/4/8 bytes, decode a handful of plausible typed
// interpretations so the caller doesn't have to guess the field's type by
// eye -- e.g. a 4-byte range showing "hp went 100 -> 80" as both i32 and
// float lets the AI immediately tell which one looks like a real value.
json GuessTypes(const std::vector<uint8_t>& buf) {
    json guesses = json::object();
    if (buf.size() == 1) {
        guesses["i8"] = static_cast<int8_t>(buf[0]);
        guesses["u8"] = buf[0];
    } else if (buf.size() == 2) {
        int16_t i16; uint16_t u16;
        memcpy(&i16, buf.data(), 2); memcpy(&u16, buf.data(), 2);
        guesses["i16"] = i16;
        guesses["u16"] = u16;
    } else if (buf.size() == 4) {
        int32_t i32; uint32_t u32; float f32;
        memcpy(&i32, buf.data(), 4); memcpy(&u32, buf.data(), 4); memcpy(&f32, buf.data(), 4);
        guesses["i32"] = i32;
        guesses["u32"] = u32;
        guesses["float"] = f32;
    } else if (buf.size() == 8) {
        int64_t i64; uint64_t u64; double f64;
        memcpy(&i64, buf.data(), 8); memcpy(&u64, buf.data(), 8); memcpy(&f64, buf.data(), 8);
        guesses["i64"] = i64;
        guesses["u64"] = u64;
        guesses["double"] = f64;
    }
    return guesses;
}

} // namespace

void RegisterDissectRoutes(httplib::Server& svr) {
    svr.Post("/dissect/snapshot", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            uintptr_t address = ParseAddress(body, "address");
            size_t size = body.at("size").get<size_t>();

            int id = dissect::TakeSnapshot(address, size);
            if (id < 0) {
                res.status = 400;
                res.set_content(json{{"ok", false}, {"error", "read_failed"}}.dump(), "application/json");
                return;
            }

            res.set_content(json{{"ok", true}, {"id", id}}.dump(), "application/json");
            overlay::LogApiCall("POST /dissect/snapshot @ " + Hex(address) + " (id " + std::to_string(id) + ")");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Get("/dissect/snapshots", [](const httplib::Request&, httplib::Response& res) {
        auto list = dissect::ListSnapshots();
        json arr = json::array();
        for (const auto& s : list) arr.push_back({{"id", s.id}, {"address", Hex(s.address)}, {"size", s.size}});
        res.set_content(json{{"ok", true}, {"snapshots", arr}}.dump(), "application/json");
    });

    svr.Delete(R"(/dissect/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
        int id = std::stoi(req.matches[1]);
        bool removed = dissect::DeleteSnapshot(id);
        if (!removed) {
            res.status = 404;
            res.set_content(json{{"ok", false}, {"error", "unknown_snapshot"}}.dump(), "application/json");
            return;
        }
        res.set_content(json{{"ok", true}}.dump(), "application/json");
        overlay::LogApiCall("DELETE /dissect/" + std::to_string(id));
    });

    svr.Post("/dissect/diff", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            int idA = body.at("a").get<int>();
            int idB = body.at("b").get<int>();

            std::vector<dissect::DiffRange> diffs;
            std::string error;
            if (!dissect::DiffSnapshots(idA, idB, diffs, error)) {
                res.status = 400;
                res.set_content(json{{"ok", false}, {"error", error}}.dump(), "application/json");
                return;
            }

            json arr = json::array();
            for (const auto& d : diffs) {
                arr.push_back({{"offset", d.offset},
                                {"size", d.before.size()},
                                {"before", BytesToHex(d.before)},
                                {"after", BytesToHex(d.after)},
                                {"before_guess", GuessTypes(d.before)},
                                {"after_guess", GuessTypes(d.after)}});
            }
            res.set_content(json{{"ok", true}, {"diffs", arr}}.dump(), "application/json");
            overlay::LogApiCall("POST /dissect/diff " + std::to_string(idA) + " vs " + std::to_string(idB) +
                                 " (" + std::to_string(diffs.size()) + " zones)");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });
}

} // namespace api
