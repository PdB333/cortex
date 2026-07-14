#include "routes.h"
#include "../analysis/analysis.h"
#include "../overlay/overlay.h"

#include <nlohmann/json.hpp>
#include <sstream>
#include <iomanip>

using json = nlohmann::json;

namespace api {

namespace {

std::string Hex(uintptr_t a) {
    std::ostringstream s;
    s << "0x" << std::hex << a;
    return s.str();
}

uintptr_t ParseAddress(const json& body, const char* key) {
    const json& v = body.at(key);
    if (v.is_string()) return static_cast<uintptr_t>(std::stoull(v.get<std::string>(), nullptr, 0));
    return static_cast<uintptr_t>(v.get<uint64_t>());
}

} // namespace

void RegisterAnalysisRoutes(httplib::Server& svr) {
    svr.Post("/analysis/functions", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            std::string module = body.at("module").get<std::string>();

            bool truncated = false;
            auto functions = analysis::FindFunctions(module, truncated);

            json arr = json::array();
            for (const auto& f : functions) arr.push_back({{"address", Hex(f.address)}, {"size", f.size}});
            res.set_content(json{{"ok", true}, {"functions", arr}, {"truncated", truncated}}.dump(),
                             "application/json");
            overlay::LogApiCall("POST /analysis/functions " + module + " (" +
                                 std::to_string(functions.size()) + " trouvees)");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Post("/analysis/cfg", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            uintptr_t address = ParseAddress(body, "address");

            bool ok = false;
            auto cfg = analysis::BuildCFG(address, ok);
            if (!ok) {
                res.status = 400;
                res.set_content(json{{"ok", false}, {"error", "address_not_readable"}}.dump(), "application/json");
                return;
            }

            json blocks = json::array();
            for (const auto& b : cfg.blocks) blocks.push_back({{"start", Hex(b.start)}, {"end", Hex(b.end)}});
            json edges = json::array();
            for (const auto& e : cfg.edges) {
                edges.push_back({{"from", Hex(e.from)}, {"to", e.to ? Hex(e.to) : "0"}, {"type", e.type}});
            }
            res.set_content(json{{"ok", true}, {"blocks", blocks}, {"edges", edges}, {"truncated", cfg.truncated}}
                                 .dump(),
                             "application/json");
            overlay::LogApiCall("POST /analysis/cfg @ " + Hex(address) + " (" +
                                 std::to_string(cfg.blocks.size()) + " blocs)");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Post("/analysis/xrefs", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            uintptr_t target = ParseAddress(body, "target");
            std::string module = body.value("module", std::string());
            bool includeData = body.value("include_data", true);

            bool truncated = false;
            auto xrefs = analysis::FindXRefs(target, module, truncated);

            bool dataTruncated = false;
            if (includeData) {
                auto dataXrefs = analysis::FindDataXRefs(target, module, dataTruncated);
                xrefs.insert(xrefs.end(), dataXrefs.begin(), dataXrefs.end());
                truncated = truncated || dataTruncated;
            }

            json arr = json::array();
            for (const auto& x : xrefs) arr.push_back({{"from", Hex(x.from)}, {"type", x.type}});
            res.set_content(json{{"ok", true}, {"xrefs", arr}, {"truncated", truncated}}.dump(), "application/json");
            overlay::LogApiCall("POST /analysis/xrefs @ " + Hex(target) + " (" +
                                 std::to_string(xrefs.size()) + " trouvees)");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Post("/analysis/vtable", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            uintptr_t address = ParseAddress(body, "address");
            int maxEntries = body.value("max_entries", 256);

            bool ok = false;
            auto dump = analysis::DumpVTable(address, maxEntries, ok);
            if (!ok) {
                res.status = 400;
                res.set_content(json{{"ok", false}, {"error", "address_not_readable"}}.dump(), "application/json");
                return;
            }

            json entries = json::array();
            for (const auto& e : dump.entries) entries.push_back({{"slot", Hex(e.slot)}, {"address", Hex(e.address)}});
            res.set_content(json{{"ok", true},
                                  {"entries", entries},
                                  {"type_name", dump.typeName},
                                  {"truncated", dump.truncated}}
                                 .dump(),
                             "application/json");
            overlay::LogApiCall("POST /analysis/vtable @ " + Hex(address) + " (" +
                                 std::to_string(dump.entries.size()) + " slots" +
                                 (dump.typeName.empty() ? "" : ", " + dump.typeName) + ")");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Post("/analysis/structure", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            uintptr_t address = ParseAddress(body, "address");

            bool ok = false;
            auto structured = analysis::StructureCFG(address, ok);
            if (!ok) {
                res.status = 400;
                res.set_content(json{{"ok", false}, {"error", "address_not_readable"}}.dump(), "application/json");
                return;
            }

            json lines = json::array();
            for (const auto& l : structured.lines) {
                lines.push_back({{"address", Hex(l.address)},
                                  {"depth", l.depth},
                                  {"text", l.text},
                                  {"is_annotation", l.isAnnotation}});
            }
            json headers = json::array();
            for (auto h : structured.loopHeaders) headers.push_back(Hex(h));

            res.set_content(json{{"ok", true},
                                  {"lines", lines},
                                  {"loop_headers", headers},
                                  {"truncated", structured.truncated}}
                                 .dump(),
                             "application/json");
            overlay::LogApiCall("POST /analysis/structure @ " + Hex(address) + " (" +
                                 std::to_string(structured.lines.size()) + " lignes, " +
                                 std::to_string(structured.loopHeaders.size()) + " boucles)");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Get("/analysis/pe_headers", [](const httplib::Request& req, httplib::Response& res) {
        try {
            if (!req.has_param("module")) throw std::runtime_error("missing module param");
            std::string module = req.get_param_value("module");

            bool ok = false;
            auto info = analysis::DissectPeHeaders(module, ok);
            if (!ok) {
                res.status = 400;
                res.set_content(json{{"ok", false}, {"error", "module_or_pe_signature_invalid"}}.dump(),
                                 "application/json");
                return;
            }

            json sections = json::array();
            for (const auto& s : info.sections) {
                sections.push_back({{"name", s.name},
                                     {"virtual_address", Hex(s.virtualAddress)},
                                     {"virtual_size", s.virtualSize},
                                     {"raw_size", s.rawSize},
                                     {"characteristics", s.characteristics}});
            }
            json imports = json::array();
            for (const auto& imp : info.imports) {
                imports.push_back({{"module", imp.moduleName},
                                    {"function", imp.functionName},
                                    {"ordinal", imp.ordinal},
                                    {"iat_slot", Hex(imp.iatSlot)}});
            }
            json exports = json::array();
            for (const auto& exp : info.exports) {
                exports.push_back({{"name", exp.name}, {"ordinal", exp.ordinal}, {"address", Hex(exp.address)}});
            }

            res.set_content(json{{"ok", true},
                                  {"base", Hex(info.base)},
                                  {"size_of_image", info.sizeOfImage},
                                  {"entry_point", Hex(info.entryPoint)},
                                  {"machine", info.machine},
                                  {"subsystem", info.subsystem},
                                  {"timestamp", info.timestamp},
                                  {"characteristics", info.characteristics},
                                  {"is_dll", info.isDll},
                                  {"sections", sections},
                                  {"imports", imports},
                                  {"exports", exports},
                                  {"imports_truncated", info.importsTruncated},
                                  {"exports_truncated", info.exportsTruncated}}
                                 .dump(),
                             "application/json");
            overlay::LogApiCall("GET /analysis/pe_headers " + module + " (" +
                                 std::to_string(info.sections.size()) + " sections, " +
                                 std::to_string(info.imports.size()) + " imports, " +
                                 std::to_string(info.exports.size()) + " exports)");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Post("/analysis/scan_patches", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            std::string module = body.at("module").get<std::string>();
            size_t minRunLength = body.value("min_run_length", static_cast<size_t>(16));

            bool ok = false;
            auto result = analysis::ScanPatches(module, minRunLength, ok);
            if (!ok) {
                res.status = 400;
                res.set_content(
                    json{{"ok", false}, {"error", "module_not_found_or_file_unreadable_or_pe_invalid"}}.dump(),
                    "application/json");
                return;
            }

            json runs = json::array();
            for (const auto& r : result.runs) {
                std::ostringstream diskHex, memHex;
                for (uint8_t b : r.diskBytes) diskHex << std::setw(2) << std::setfill('0') << std::hex << (int)b;
                for (uint8_t b : r.memoryBytes) memHex << std::setw(2) << std::setfill('0') << std::hex << (int)b;
                runs.push_back({{"address", Hex(r.address)},
                                 {"size", r.diskBytes.size()},
                                 {"disk_bytes", diskHex.str()},
                                 {"memory_bytes", memHex.str()}});
            }
            res.set_content(json{{"ok", true}, {"runs", runs}, {"truncated", result.truncated}}.dump(),
                             "application/json");
            overlay::LogApiCall("POST /analysis/scan_patches " + module + " (" +
                                 std::to_string(result.runs.size()) + " patches trouves)");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });
}

} // namespace api
