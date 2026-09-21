#include "routes.h"
#include "../process/address.h"
#include "../patch/patch.h"
#include "../overlay/overlay.h"
#include "../action/action.h"

#include <nlohmann/json.hpp>
#include <sstream>
#include <iomanip>
#include <cctype>
#include <stdexcept>

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
    std::string compact;
    compact.reserve(hex.size());
    for (unsigned char c : hex) {
        if (std::isspace(c) || c == '_') continue;
        compact.push_back(static_cast<char>(c));
    }
    if (compact.size() >= 2 && compact[0] == '0' && (compact[1] == 'x' || compact[1] == 'X'))
        compact.erase(0, 2);
    if (compact.empty() || (compact.size() % 2) != 0)
        throw std::invalid_argument("invalid_hex_bytes");
    for (unsigned char c : compact) {
        if (!std::isxdigit(c)) throw std::invalid_argument("invalid_hex_bytes");
    }

    std::vector<uint8_t> out;
    out.reserve(compact.size() / 2);
    for (size_t i = 0; i < compact.size(); i += 2)
        out.push_back(static_cast<uint8_t>(std::stoul(compact.substr(i, 2), nullptr, 16)));
    return out;
}
json PatchToJson(const patch::PatchInfo& p) {
    return json{{"id", p.id}, {"address", HexAddr(p.address)},
                {"original_bytes", BytesToHex(p.originalBytes)},
                {"new_bytes", BytesToHex(p.newBytes)},
                {"label", p.label}, {"gateway", p.gateway ? HexAddr(p.gateway) : std::string()}};
}

} // namespace

void RegisterPatchRoutes(httplib::Server& svr) {
    svr.Post("/patch/write", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto mutation = action::LockMutations();
            json body = json::parse(req.body);
            uintptr_t address = ParseAddress(body.at("address"));
            auto bytes = HexToBytes(body.at("bytes").get<std::string>());
            std::string label = body.value("label", "");

            int id = patch::Apply(address, bytes, label);
            if (id >= 0) action::Record("patch/write " + HexAddr(address), [id] { return patch::Revert(id); });
            json out;
            out["ok"] = id >= 0;
            if (id >= 0) out["id"] = id; else out["error"] = "read_or_write_failed";
            res.set_content(out.dump(), "application/json");
            overlay::LogApiCall("POST /patch/write @ " + HexAddr(address));
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Post("/patch/nop", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto mutation = action::LockMutations();
            json body = json::parse(req.body);
            uintptr_t address = ParseAddress(body.at("address"));
            size_t size = body.at("size").get<size_t>();
            std::string label = body.value("label", "");

            int id = patch::Nop(address, size, label);
            if (id >= 0) action::Record("patch/nop " + HexAddr(address), [id] { return patch::Revert(id); });
            json out;
            out["ok"] = id >= 0;
            if (id >= 0) out["id"] = id; else out["error"] = "read_or_write_failed";
            res.set_content(out.dump(), "application/json");
            overlay::LogApiCall("POST /patch/nop @ " + HexAddr(address) + " x" + std::to_string(size));
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Post("/patch/detour", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto mutation = action::LockMutations();
            json body = json::parse(req.body);
            uintptr_t address = ParseAddress(body.at("address"));
            uintptr_t target = ParseAddress(body.at("target"));
            int jmpSize = body.value("jmp_size", 5);

            std::string error;
            int id = patch::Detour(address, target, jmpSize, error);
            if (id >= 0) action::Record("patch/detour " + HexAddr(address), [id] { return patch::Revert(id); });
            json out;
            out["ok"] = id >= 0;
            if (id >= 0) out["id"] = id; else out["error"] = error;
            res.set_content(out.dump(), "application/json");
            overlay::LogApiCall("POST /patch/detour @ " + HexAddr(address) + " -> " + HexAddr(target));
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Post("/patch/trampoline", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto mutation = action::LockMutations();
            json body = json::parse(req.body);
            const uintptr_t address = ParseAddress(body.at("address"));
            const uintptr_t target = ParseAddress(body.at("target"));
            patch::TrampolineInfo info;
            std::string error;
            const bool ok = patch::CreateTrampoline(address, target, body.value("minimum_overwrite", size_t{5}), info, error);
            if (ok) action::Record("patch/trampoline " + HexAddr(address), [id=info.patchId] { return patch::Revert(id); });
            res.status = ok ? 200 : 400;
            res.set_content(ok ? json{{"ok",true},{"id",info.patchId},{"gateway",HexAddr(info.gateway)},
                                       {"overwritten_size",info.overwrittenSize},{"gateway_size",info.gatewaySize}}.dump()
                               : json{{"ok",false},{"error",error}}.dump(), "application/json");
            overlay::LogApiCall("POST /patch/trampoline @ " + HexAddr(address));
        } catch (const std::exception& e) {
            res.status = 400; res.set_content(json{{"ok",false},{"error",e.what()}}.dump(), "application/json");
        }
    });

    svr.Post("/patch/assemble", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto mutation = action::LockMutations();
            json body = json::parse(req.body);
            uintptr_t address = ParseAddress(body.at("address"));
            std::vector<std::string> lines;
            if (body.at("lines").is_string()) {
                std::istringstream ss(body.at("lines").get<std::string>());
                std::string l;
                while (std::getline(ss, l)) lines.push_back(l);
            } else {
                for (const auto& l : body.at("lines")) lines.push_back(l.get<std::string>());
            }
            bool write = body.value("write", false);
            std::string label = body.value("label", "");

            std::vector<uint8_t> bytes;
            std::string error;
            if (!patch::Assemble(lines, address, bytes, error)) {
                res.status = 400;
                res.set_content(json{{"ok", false}, {"error", error}}.dump(), "application/json");
                return;
            }

            json out{{"ok", true}, {"bytes", BytesToHex(bytes)}, {"size", bytes.size()}};
            if (write) {
                int id = patch::Apply(address, bytes, label);
                if (id >= 0) action::Record("patch/assemble " + HexAddr(address), [id] { return patch::Revert(id); });
                out["written"] = id >= 0;
                if (id >= 0) out["id"] = id; else out["write_error"] = "read_or_write_failed";
            }
            res.set_content(out.dump(), "application/json");
            overlay::LogApiCall("POST /patch/assemble @ " + HexAddr(address) + " (" + std::to_string(lines.size()) + " lignes)");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Post("/patch/alloc_cave", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto mutation = action::LockMutations();
            json body = json::parse(req.body);
            uintptr_t nearAddress = ParseAddress(body.at("near_address"));
            size_t size = body.at("size").get<size_t>();

            uintptr_t addr = patch::AllocNearCave(nearAddress, size);
            json out;
            out["ok"] = addr != 0;
            if (addr != 0) out["address"] = HexAddr(addr); else out["error"] = "alloc_failed";
            res.set_content(out.dump(), "application/json");
            overlay::LogApiCall("POST /patch/alloc_cave near " + HexAddr(nearAddress));
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Delete(R"(/patch/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
        auto mutation = action::LockMutations();
        int id = std::stoi(req.matches[1]);
        bool ok = patch::Revert(id);
        res.status = ok ? 200 : 404;
        res.set_content(json{{"ok", ok}}.dump(), "application/json");
        overlay::LogApiCall("DELETE /patch/" + std::to_string(id));
    });

    svr.Get("/patch/list", [](const httplib::Request&, httplib::Response& res) {
        json arr = json::array();
        for (const auto& p : patch::List()) arr.push_back(PatchToJson(p));
        res.set_content(json{{"ok", true}, {"patches", arr}}.dump(), "application/json");
        overlay::LogApiCall("GET /patch/list");
    });
}

} // namespace api
