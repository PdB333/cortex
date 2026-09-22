#include "screenshot_model.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>

namespace cortex::application {
namespace {

using json = nlohmann::json;

json RouteResult(const json& output) {
    if (!output.is_object()) return output;
    const auto found = output.find("result");
    return found != output.end() ? *found : output;
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

} // namespace

void ScreenshotModel::Reset() {
    imageBase64_.clear();
    meta_.clear();
    ++generation_;
}

bool ScreenshotModel::EnsureRuntime(std::string* error) {
    if (error) error->clear();
    if (payload_.Ready()) return true;
    return payload_.TryConnectExisting(error);
}

bool ScreenshotModel::Capture(const std::string& rawMode, std::string* error) {
    const std::string mode = Lower(rawMode);
    if (mode != "auto" && mode != "render" &&
        mode != "window" && mode != "last") {
        if (error) *error = "invalid_capture_mode";
        return false;
    }
    if (!EnsureRuntime(error)) return false;

    const json query = {
        {"encoding", "base64"},
        {"mode", mode},
        {"timeout_ms", mode == "auto" ? 1800 : 8000}
    };

    json output;
    if (!payload_.CallTool("screenshot", {{"_query", query}}, output, error))
        return false;

    const json result = RouteResult(output);
    const std::string image = result.value("image_base64", std::string());
    if (image.empty()) {
        if (error) *error = "capture_missing_image";
        return false;
    }

    imageBase64_ = image;
    meta_ = result.value("source", std::string("unknown")) + " | " +
            std::to_string(image.size()) + " bytes base64";
    ++generation_;
    return true;
}

} // namespace cortex::application
