#pragma once

#include <nlohmann/json.hpp>
#include <cctype>
#include <string>

namespace api::response {

using json = nlohmann::json;

inline bool IsStableErrorCode(const std::string& code) {
    if (code.empty()) return false;
    for (unsigned char c : code) {
        if (!(std::islower(c) || std::isdigit(c) || c == '_')) return false;
    }
    return true;
}

inline json Success(json data = json::object(), const std::string& requestId = {}) {
    json result = {{"ok", true}, {"data", std::move(data)}};
    if (!requestId.empty()) result["request_id"] = requestId;
    return result;
}

inline json Error(const std::string& code,
                  const std::string& message = {},
                  const std::string& requestId = {}) {
    json result = {{"ok", false}, {"error", code}};
    if (!message.empty()) result["message"] = message;
    if (!requestId.empty()) result["request_id"] = requestId;
    return result;
}

} // namespace api::response
