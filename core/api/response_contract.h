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

// The common operation result (roadmap step 2.5). New services return it;
// existing routes keep their shape until they are migrated one by one.
//
//   ok                    status == "completed" (kept for clients and
//                         mcp_tools::IsToolError that read `ok`)
//   status                completed | failed | refused | cancelled | timed_out
//   error, cause          stable code and human detail, only when not completed
//   target                {pid, generation} of the process that ran it
//   duration_ms           wall time spent in the operation
//   limits                the bounds applied (counts, sizes, timeouts)
//   data                  what the operation produced
//   evidence              references other records can cite
//   permissions_required  authorities the call needed (empty for reads)
//   rollback              how to undo it, or null when nothing changed
struct Operation {
    std::string status = "completed";
    std::string error;
    std::string cause;
    json target = json::object();
    uint64_t durationMs = 0;
    json limits = json::object();
    json data = json::object();
    json evidence = json::array();
    json permissionsRequired = json::array();
    json rollback = nullptr;
};

inline json OperationResult(const Operation& operation) {
    json result = {{"ok", operation.status == "completed"},
                   {"status", operation.status},
                   {"target", operation.target},
                   {"duration_ms", operation.durationMs},
                   {"limits", operation.limits},
                   {"data", operation.data},
                   {"evidence", operation.evidence},
                   {"permissions_required", operation.permissionsRequired},
                   {"rollback", operation.rollback}};
    if (operation.status != "completed") {
        result["error"] = IsStableErrorCode(operation.error) ? operation.error : std::string("operation_failed");
        if (!operation.cause.empty()) result["cause"] = operation.cause;
    }
    return result;
}

inline Operation Refused(const std::string& code, const std::string& cause, json target = json::object()) {
    Operation operation;
    operation.status = "refused";
    operation.error = code;
    operation.cause = cause;
    operation.target = std::move(target);
    return operation;
}

} // namespace api::response
