// OCR routes.
//
// POST /ocr accepts a PNG blob (base64) or a file path and returns recognized
// text plus per-word bounding boxes. Backend is Windows.Media.Ocr via a
// bundled PowerShell shim (Win10+ only, no external binaries to ship).

#include "routes.h"
#include "server.h"
#include "../ocr/ocr.h"
#include "../overlay/overlay.h"

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <vector>

using json = nlohmann::json;

namespace api {

void RegisterOcrRoutes(httplib::Server& svr) {
    svr.Post("/ocr", [](const httplib::Request& req, httplib::Response& res) {
        json body;
        try { body = json::parse(req.body.empty() ? "{}" : req.body); }
        catch (...) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":\"invalid_json\"}", "application/json");
            return;
        }

        std::vector<uint8_t> png;
        if (body.contains("image_base64") && body["image_base64"].is_string()) {
            png = ocr::Base64Decode(body["image_base64"].get<std::string>());
        } else if (body.contains("image_path") && body["image_path"].is_string()) {
            std::ifstream f(body["image_path"].get<std::string>(), std::ios::binary);
            if (!f) {
                res.status = 404;
                res.set_content("{\"ok\":false,\"error\":\"image_not_found\"}", "application/json");
                return;
            }
            std::stringstream buf; buf << f.rdbuf();
            const std::string& s = buf.str();
            png.assign(s.begin(), s.end());
        } else {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":\"missing_image\"}", "application/json");
            return;
        }

        const std::string language = body.value("language", std::string(""));
        auto r = ocr::Recognize(png, language);

        json out;
        // Prefer the JSON produced by the OCR shim (has words + boxes); fall
        // back to a minimal envelope if the shim returned nothing parseable.
        try {
            if (!r.json.empty()) out = json::parse(r.json);
            else                 out = json{{"ok", r.ok}, {"text", r.text}, {"error", r.error}};
        } catch (...) {
            out = json{{"ok", false}, {"raw", r.json}, {"error", "shim_output_not_json"}};
        }
        out["engine"] = r.engine;
        if (!r.error.empty() && !out.contains("error")) out["error"] = r.error;

        res.set_content(out.dump(), "application/json");
        overlay::LogApiCall("POST /ocr");
    });
}

} // namespace api
