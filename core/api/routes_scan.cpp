#include "routes.h"
#include "../memory/scan.h"
#include "../overlay/overlay.h"

#include <nlohmann/json.hpp>
#include <sstream>

using json = nlohmann::json;

namespace api {

namespace {

json ScanResultToJson(const memscan::ScanResult& r) {
    std::ostringstream addr;
    addr << "0x" << std::hex << r.address;
    json value;
    constexpr int64_t kMaxExactJsonInteger = 9007199254740991LL;
    if (std::holds_alternative<int64_t>(r.value)) {
        const int64_t v = std::get<int64_t>(r.value);
        value = (v >= -kMaxExactJsonInteger && v <= kMaxExactJsonInteger) ? json(v) : json(std::to_string(v));
    } else if (std::holds_alternative<uint64_t>(r.value)) {
        const uint64_t v = std::get<uint64_t>(r.value);
        value = v <= static_cast<uint64_t>(kMaxExactJsonInteger) ? json(v) : json(std::to_string(v));
    }
    else value = std::get<double>(r.value);
    json out{{"address", addr.str()}, {"value", value}};
    if (!r.type.empty()) out["type"] = r.type;
    return out;
}

std::optional<std::string> OptScalar(const json& body, const char* key) {
    if (!body.contains(key) || body.at(key).is_null()) return std::nullopt;
    const json& value = body.at(key);
    return value.is_string() ? value.get<std::string>() : value.dump();
}

std::optional<uintptr_t> OptAddress(const json& body, const char* key) {
    if (!body.contains(key) || body.at(key).is_null()) return std::nullopt;
    const json& jaddr = body.at(key);
    if (jaddr.is_string()) return static_cast<uintptr_t>(std::stoull(jaddr.get<std::string>(), nullptr, 0));
    return static_cast<uintptr_t>(jaddr.get<uint64_t>());
}

} // namespace

void RegisterScanRoutes(httplib::Server& svr) {
    svr.Post("/scan/new", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            std::string type = body.at("type").get<std::string>();
            auto value = OptScalar(body, "value");
            auto rangeStart = OptAddress(body, "start");
            auto rangeEnd = OptAddress(body, "end");

            memscan::ScanOptions options;
            options.writableOnly = body.value("writable_only", true);
            options.executableOnly = body.value("executable_only", false);
            options.copyOnWriteOnly = body.value("copy_on_write", false);
            options.alignment = body.value("alignment", 0u);
            options.pauseProcess = body.value("pause_process", false);
            options.excludeCortex = body.value("exclude_cortex", true);

            size_t count = 0;
            bool truncated = false;
            int id = memscan::ScanNew(type, value, rangeStart, rangeEnd, count, truncated, options);

            json out;
            out["ok"] = id >= 0;
            if (id >= 0) {
                out["scan_id"] = id;
                out["count"] = count;
                out["truncated"] = truncated;
            } else {
                out["error"] = (type == "all" && !value.has_value()) ? "all_requires_value" : "unknown_type";
            }
            res.set_content(out.dump(), "application/json");
            overlay::LogApiCall("POST /scan/new " + type + (value.has_value() ? " =" + *value : " (unknown)"));
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Post("/scan/next", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            int id = body.at("scan_id").get<int>();
            std::string filterStr = body.at("filter").get<std::string>();
            auto filter = memscan::ParseFilter(filterStr);
            if (!filter.has_value()) {
                res.status = 400;
                res.set_content(json{{"ok", false}, {"error", "unknown_filter"}}.dump(), "application/json");
                return;
            }
            auto value = OptScalar(body, "value");
            auto value2 = OptScalar(body, "value2");
            bool pauseProcess = body.value("pause_process", false);

            size_t count = 0;
            bool ok = memscan::ScanNext(id, *filter, value, value2, count, pauseProcess);

            json out;
            out["ok"] = ok;
            if (ok) out["count"] = count; else out["error"] = "scan_not_found";
            res.set_content(out.dump(), "application/json");
            overlay::LogApiCall("POST /scan/next #" + std::to_string(id) + " " + filterStr);
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Get(R"(/scan/results/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
        int id = std::stoi(req.matches[1]);
        size_t offset = req.has_param("offset") ? std::stoul(req.get_param_value("offset")) : 0;
        size_t limit = req.has_param("limit") ? std::stoul(req.get_param_value("limit")) : 100;

        std::vector<memscan::ScanResult> results;
        size_t total = 0;
        if (!memscan::ScanResults(id, offset, limit, results, total)) {
            res.status = 404;
            res.set_content(json{{"ok", false}, {"error", "scan_not_found"}}.dump(), "application/json");
            return;
        }

        json arr = json::array();
        for (const auto& r : results) arr.push_back(ScanResultToJson(r));
        res.set_content(json{{"ok", true}, {"total", total}, {"results", arr}}.dump(), "application/json");
    });

    svr.Delete(R"(/scan/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
        int id = std::stoi(req.matches[1]);
        bool ok = memscan::ScanReset(id);
        res.status = ok ? 200 : 404;
        res.set_content(json{{"ok", ok}}.dump(), "application/json");
        overlay::LogApiCall("DELETE /scan/" + std::to_string(id));
    });

    svr.Get("/scan/list", [](const httplib::Request&, httplib::Response& res) {
        json arr = json::array();
        for (const auto& s : memscan::ScanList()) {
            arr.push_back({{"scan_id", s.id}, {"type", s.type}, {"count", s.count}});
        }
        res.set_content(json{{"ok", true}, {"scans", arr}}.dump(), "application/json");
    });

    svr.Post("/scan/aob", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            std::string pattern = body.at("pattern").get<std::string>();
            std::string module = body.value("module", std::string());

            bool truncated = false;
            auto matches = memscan::AobScan(pattern, module, truncated);

            json arr = json::array();
            for (uintptr_t a : matches) {
                std::ostringstream addr;
                addr << "0x" << std::hex << a;
                arr.push_back(addr.str());
            }
            res.set_content(json{{"ok", true}, {"addresses", arr}, {"truncated", truncated}}.dump(), "application/json");
            overlay::LogApiCall("POST /scan/aob \"" + pattern + "\"" + (module.empty() ? "" : " in " + module));
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Post("/scan/pointers", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            uintptr_t target = *OptAddress(body, "target");
            uint32_t maxOffset = body.value("max_offset", 0u);

            bool truncated = false;
            auto hits = memscan::FindPointersTo(target, maxOffset, truncated);

            json arr = json::array();
            for (const auto& h : hits) {
                std::ostringstream addr;
                addr << "0x" << std::hex << h.address;
                arr.push_back({{"address", addr.str()}, {"offset", h.offset}});
            }
            res.set_content(json{{"ok", true}, {"results", arr}, {"truncated", truncated}}.dump(), "application/json");
            overlay::LogApiCall("POST /scan/pointers");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Post("/scan/pointer_path", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            uintptr_t target = *OptAddress(body, "target");
            int maxDepth = body.value("max_depth", 5);
            uint32_t maxOffset = body.value("max_offset", 256u);

            bool truncated = false;
            auto results = memscan::PointerScan(target, maxDepth, maxOffset, truncated);

            json arr = json::array();
            for (const auto& r : results) {
                json offsets = json::array();
                for (int64_t o : r.offsets) offsets.push_back(o);
                std::ostringstream baseOff;
                baseOff << "0x" << std::hex << r.base_offset;
                arr.push_back({{"module", r.module}, {"base_offset", baseOff.str()}, {"offsets", offsets}});
            }
            res.set_content(json{{"ok", true}, {"results", arr}, {"truncated", truncated}}.dump(), "application/json");
            overlay::LogApiCall("POST /scan/pointer_path (" + std::to_string(results.size()) + " chemins)");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Post("/scan/strings", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            size_t minLength = body.value("min_length", 4);
            std::string contains = body.value("contains", std::string());
            std::string module = body.value("module", std::string());
            std::string encodingStr = body.value("encoding", std::string("ascii"));
            auto encoding = encodingStr == "utf16" ? memscan::StringEncoding::Utf16 : memscan::StringEncoding::Ascii;

            bool truncated = false;
            auto hits = memscan::StringScan(minLength, contains, module, encoding, truncated);

            json arr = json::array();
            for (const auto& h : hits) {
                std::ostringstream addr;
                addr << "0x" << std::hex << h.address;
                arr.push_back({{"address", addr.str()}, {"value", h.value}});
            }
            res.set_content(json{{"ok", true}, {"results", arr}, {"truncated", truncated}}.dump(), "application/json");
            overlay::LogApiCall("POST /scan/strings (" + std::to_string(hits.size()) + " trouvees)");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Post("/scan/intersect", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            std::vector<int> ids;
            for (const auto& v : body.at("scan_ids")) ids.push_back(v.get<int>());

            std::vector<uintptr_t> addresses;
            bool ok = memscan::ScanIntersect(ids, addresses);

            json out;
            out["ok"] = ok;
            if (ok) {
                json arr = json::array();
                for (uintptr_t a : addresses) {
                    std::ostringstream addr;
                    addr << "0x" << std::hex << a;
                    arr.push_back(addr.str());
                }
                out["addresses"] = arr;
                out["count"] = addresses.size();
            } else {
                out["error"] = "need_at_least_two_valid_scan_ids";
            }
            res.set_content(out.dump(), "application/json");
            overlay::LogApiCall("POST /scan/intersect (" + std::to_string(ids.size()) + " scans)");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Post("/scan/code_caves", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            size_t minSize = body.value("min_size", 16);
            std::string module = body.value("module", std::string());

            bool truncated = false;
            auto caves = memscan::FindCodeCaves(minSize, module, truncated);

            json arr = json::array();
            for (const auto& c : caves) {
                std::ostringstream addr;
                addr << "0x" << std::hex << c.address;
                arr.push_back({{"address", addr.str()}, {"size", c.size}});
            }
            res.set_content(json{{"ok", true}, {"caves", arr}, {"truncated", truncated}}.dump(), "application/json");
            overlay::LogApiCall("POST /scan/code_caves (" + std::to_string(caves.size()) + " trouvees)");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });
}

} // namespace api
