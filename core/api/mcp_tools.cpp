#include "mcp_tools.h"

#include "routes.h"
#include "native_routes.h"
#include "semantic_tools.h"
#include "mcp_contract.h"
#include "../action/action.h"

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <windows.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace api::mcp_tools {
namespace {

using CancelFlag = std::shared_ptr<std::atomic<bool>>;
std::mutex g_cancelMutex;
std::unordered_map<std::string, std::weak_ptr<std::atomic<bool>>> g_cancelFlags;

std::string SanitizeToolName(std::string name) {
    for (auto& c : name)
        if (c == '/' || c == '.' || c == '-' || c == '{' || c == '}') c = '_';
    return name;
}

std::string RequestKey(const std::string& scope, const json& requestId) {
    return scope + "\n" + requestId.dump();
}

CancelFlag RegisterCancellation(const std::string& scope, const json& requestId) {
    auto flag = std::make_shared<std::atomic<bool>>(false);
    std::lock_guard<std::mutex> lock(g_cancelMutex);
    g_cancelFlags[RequestKey(scope, requestId)] = flag;
    return flag;
}

void UnregisterCancellation(const std::string& scope,
                            const json& requestId,
                            const CancelFlag& flag) {
    std::lock_guard<std::mutex> lock(g_cancelMutex);
    const auto key = RequestKey(scope, requestId);
    auto it = g_cancelFlags.find(key);
    if (it == g_cancelFlags.end()) return;
    auto active = it->second.lock();
    if (!active || active == flag) g_cancelFlags.erase(it);
}

void HandleNotification(const json& notification, const std::string& scope) {
    if (!notification.is_object() ||
        notification.value("method", std::string()) != "notifications/cancelled") return;
    const json params = notification.value("params", json::object());
    if (!params.is_object() || !params.contains("requestId")) return;

    std::lock_guard<std::mutex> lock(g_cancelMutex);
    auto it = g_cancelFlags.find(RequestKey(scope, params.at("requestId")));
    if (it == g_cancelFlags.end()) return;
    if (auto flag = it->second.lock()) flag->store(true, std::memory_order_release);
    else g_cancelFlags.erase(it);
}

json ManifestEntryToMcpTool(const json& entry) {
    const std::string name = entry.value("name", std::string());
    const auto risk = mcp_contract::ClassifyTool(
        name,
        entry.value("method", std::string("GET")),
        entry.value("path", std::string()));
    const std::string mutationWhen = entry.value("mutation_permission_when", std::string());
    json properties = json::object();
    json required = json::array();

    if (entry.contains("body") && entry["body"].is_object()) {
        for (auto it = entry["body"].begin(); it != entry["body"].end(); ++it) {
            properties[it.key()] = mcp_contract::SchemaForProperty(it.key(), it.value());
            if (mcp_contract::IsRequiredSpec(it.value())) required.push_back(it.key());
        }
    }

    if (entry.contains("query") && entry["query"].is_object()) {
        auto querySchema = mcp_contract::BuildQuerySchema(entry["query"]);
        if (querySchema.containerRequired) required.push_back("_query");
        properties["_query"] = std::move(querySchema.schema);
    }

    const auto pathParameters = mcp_contract::PathParameters(entry.value("path", std::string()));
    if (!pathParameters.empty()) {
        json pathProperties = json::object();
        json pathRequired = json::array();
        for (const auto& parameter : pathParameters) {
            pathProperties[parameter] = {
                {"oneOf", json::array({{{"type", "integer"}}, {{"type", "string"}}})},
                {"description", "Required path parameter."}
            };
            pathRequired.push_back(parameter);
        }
        properties["_path"] = {
            {"type", "object"},
            {"properties", std::move(pathProperties)},
            {"required", std::move(pathRequired)},
            {"description", "Substitutions for path placeholders."}
        };
        required.push_back("_path");
    }
    if (mcp_contract::RequiresMutationPermission(risk) || !mutationWhen.empty()) {
        properties["mutation_permission"] = {{"type", "boolean"},
            {"description", mutationWhen.empty() ? "Required explicit permission for this mutating/control/native operation."
                                                 : "Required when " + mutationWhen + "."}};
        if (mcp_contract::RequiresMutationPermission(risk)) required.push_back("mutation_permission");
    }

    json schema = {{"type", "object"}, {"properties", std::move(properties)}};
    if (!required.empty()) schema["required"] = std::move(required);

    json cortexMetadata = {
        {"risk", mcp_contract::RiskName(risk)},
        {"mutation_permission_required", mcp_contract::RequiresMutationPermission(risk)}
    };
    if (!mutationWhen.empty()) cortexMetadata["mutation_permission_when"] = mutationWhen;

    return {
        {"name", SanitizeToolName(name)},
        {"description", entry.value("description", std::string())},
        {"inputSchema", std::move(schema)},
        {"_cortex", std::move(cortexMetadata)},
        {"_http", {
            {"method", entry.value("method", std::string("GET"))},
            {"path", entry.value("path", std::string())}
        }}
    };
}

json BodyPayload(const json& arguments) {
    json body = arguments;
    body.erase("_path");
    body.erase("_query");
    body.erase("mutation_permission");
    return body;
}

json Dispatch(const std::string& method, const std::string& path, const json& body) {
    httplib::Headers headers = {{"Content-Type", "application/json"}};
    const std::string payload = body.is_null() ? std::string("{}") : body.dump();
    const auto response = DispatchNativeRoute(method, path, payload, headers);

    if (!response.found) {
        return {
            {"ok", false},
            {"error", "native_route_not_found"},
            {"method", method},
            {"path", path}
        };
    }

    json output;
    output["status"] = response.status;
    if (response.contentType.rfind("application/json", 0) == 0 ||
        (!response.body.empty() && (response.body.front() == '{' || response.body.front() == '['))) {
        try {
            output["result"] = json::parse(response.body);
        } catch (...) {
            output["result_raw"] = response.body;
        }
    } else {
        output["content_type"] = response.contentType;
        output["result_bytes"] = response.body.size();
    }
    return output;
}

bool IsToolError(const json& result, bool okFalseIsError = true) {
    if (result.contains("status")) {
        if (result["status"].is_number_integer() && result["status"].get<int>() >= 400) return true;
        if (result["status"].is_string()) {
            const std::string status = result["status"].get<std::string>();
            if (status == "failed" || status == "cancelled" || status == "timed_out") return true;
        }
    }
    if (okFalseIsError && result.contains("ok") && result["ok"].is_boolean() && !result["ok"].get<bool>()) return true;
    if (result.contains("result") && result["result"].is_object()) {
        const auto& nested = result["result"];
        if (okFalseIsError && nested.contains("ok") && nested["ok"].is_boolean() && !nested["ok"].get<bool>()) return true;
        if (nested.contains("status") && nested["status"].is_string() &&
            nested["status"].get<std::string>() == "failed") return true;
    }
    return false;
}

json ToolCallPayload(const json& result, bool okFalseIsError = true) {
    return {
        {"content", json::array({{{"type", "text"}, {"text", result.dump(2)}}})},
        {"structuredContent", result},
        {"isError", IsToolError(result, okFalseIsError)}
    };
}

bool FindSemanticTool(const std::string& wanted, json& result) {
    for (const auto& entry : semantic::Catalog()) {
        if (entry.value("name", std::string()) == wanted) {
            result = entry;
            return true;
        }
    }
    return false;
}

bool FindPrimitiveTool(const std::string& wanted, json& result) {
    for (const auto& entry : BuildToolsManifest()) {
        const std::string name = entry.value("name", std::string());
        if (name == "mcp") continue;
        if (name == wanted || SanitizeToolName(name) == wanted) {
            result = entry;
            return true;
        }
    }
    return false;
}

bool SemanticAllowsPrimitive(const json& semanticTool, const std::string& wanted) {
    if (!semanticTool.contains("_primitives") || !semanticTool["_primitives"].is_array()) return false;
    for (const auto& primitive : semanticTool["_primitives"]) {
        if (!primitive.is_string()) continue;
        const std::string allowed = primitive.get<std::string>();
        if (allowed == wanted || SanitizeToolName(allowed) == wanted) return true;
    }
    return false;
}

mcp_contract::ToolRisk EffectiveRiskForCall(const std::string& name,
                                            mcp_contract::ToolRisk risk,
                                            const json& arguments) {
    if (name == "struct_infer" && arguments.is_object() && arguments.value("define", false))
        return mcp_contract::ToolRisk::Control;
    return risk;
}

bool RequiresMutationPermissionForCall(const std::string& name,
                                       mcp_contract::ToolRisk risk,
                                       const json& arguments) {
    return mcp_contract::RequiresMutationPermission(EffectiveRiskForCall(name, risk, arguments));
}
bool SupportsTransactionalRollback(const std::string& name,
                                   mcp_contract::ToolRisk risk,
                                   const json& arguments) {
    if (!RequiresMutationPermissionForCall(name, risk, arguments)) return true;
    if (risk == mcp_contract::ToolRisk::NativeCall) return false;

    // These handlers record undo actions in action::Transaction. Active tools
    // outside this allowlist are rejected before execution until they expose a
    // reliable compensation/rollback contract.
    static const std::set<std::string> transactional = {
        "memory_write",
        "memory_write_batch",
        "memory_fill",
        "patch_assemble",
        "patch_write",
        "patch_trampoline",
        "freeze_add",
        "watch_add",
        "watch_page_access",
        "debug_breakpoint_add",
        "struct_infer"
    };
    if (transactional.find(name) != transactional.end()) return true;
    if (name == "batch_run")
        return arguments.is_object() && arguments.value("transactional", false);
    return false;
}

json DispatchPrimitive(const json& entry, const json& arguments) {
    const std::string method = entry.value("method", std::string("GET"));
    const auto rendered = mcp_contract::RenderPath(entry.value("path", std::string()), arguments);
    if (!rendered) return {{"ok", false}, {"error", rendered.error}};
    return Dispatch(method, rendered.path, BodyPayload(arguments));
}

bool ResolveReferences(json& value, const json& evidence, std::string& error) {
    if (value.is_object() && value.contains("$from_step")) {
        if (!value["$from_step"].is_number_integer()) {
            error = "invalid_step_reference";
            return false;
        }
        const int index = value["$from_step"].get<int>();
        if (index < 0 || static_cast<size_t>(index) >= evidence.size()) {
            error = "step_reference_out_of_range";
            return false;
        }
        const std::string pointer = value.value("pointer", std::string());
        try {
            const json& source = evidence.at(static_cast<size_t>(index)).at("output");
            if (pointer.empty()) value = source;
            else value = source.at(json::json_pointer(pointer));
            return true;
        } catch (const std::exception&) {
            error = "step_reference_not_found";
            return false;
        }
    }

    if (value.is_array()) {
        for (auto& item : value)
            if (!ResolveReferences(item, evidence, error)) return false;
    } else if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it)
            if (!ResolveReferences(it.value(), evidence, error)) return false;
    }
    return true;
}

json RollbackToJson(const std::vector<action::RollbackResult>& rollback) {
    json result = json::array();
    for (const auto& item : rollback)
        result.push_back({{"id", item.id}, {"ok", item.ok}});
    return result;
}

json ActionsSince(uint64_t checkpoint) {
    json result = json::array();
    for (const auto& entry : action::List()) {
        if (entry.id < checkpoint) continue;
        result.push_back({{"id", entry.id},
                          {"timestamp_ms", entry.timestampMs},
                          {"description", entry.description}});
    }
    return result;
}

json TerminateExecution(json plan,
                        const std::string& status,
                        const std::string& error,
                        json evidence,
                        std::unique_ptr<action::Transaction>& transaction) {
    plan["status"] = status;
    plan["error"] = error;
    plan["evidence"] = std::move(evidence);
    if (transaction && transaction->active()) {
        const auto rollback = transaction->Rollback();
        plan["rollback_results"] = RollbackToJson(rollback);
        plan["lifecycle"]["current"] = "rolled_back";
    } else {
        plan["lifecycle"]["current"] = status == "cancelled" ? "cancelled" : "failed";
    }
    return plan;
}

json ExecuteSemantic(const std::string& wanted,
                     const json& arguments,
                     const json& requestId,
                     const std::string& cancellationScope) {
    json plan = semantic::PlanFor(wanted, arguments);
    if (IsToolError(plan) || !arguments.value("execute", false)) return plan;

    json semanticTool;
    if (!FindSemanticTool(wanted, semanticTool)) return semantic::Failure("unknown_semantic_tool", "Semantic tool not found.");

    struct PreparedStep {
        std::string requestedName;
        std::string canonicalName;
        json manifest;
        json arguments;
        mcp_contract::ToolRisk risk = mcp_contract::ToolRisk::Analyze;
    };

    std::vector<PreparedStep> steps;
    bool needsTransaction = false;
    const bool mutationPermission = arguments.value("mutation_permission", false);

    for (const auto& rawStep : arguments.at("steps")) {
        const std::string stepName = rawStep.at("tool").get<std::string>();
        if (!SemanticAllowsPrimitive(semanticTool, stepName)) {
            plan["status"] = "failed";
            plan["error"] = "primitive_not_allowed_for_semantic_tool";
            plan["rejected_tool"] = stepName;
            plan["lifecycle"]["current"] = "failed";
            return plan;
        }

        json nestedSemantic;
        if (FindSemanticTool(stepName, nestedSemantic)) {
            plan["status"] = "failed";
            plan["error"] = "nested_semantic_execution_not_supported";
            plan["rejected_tool"] = stepName;
            plan["lifecycle"]["current"] = "failed";
            return plan;
        }

        json manifest;
        if (!FindPrimitiveTool(stepName, manifest)) {
            plan["status"] = "failed";
            plan["error"] = "unknown_primitive_tool";
            plan["rejected_tool"] = stepName;
            plan["lifecycle"]["current"] = "failed";
            return plan;
        }

        const std::string canonical = manifest.value("name", stepName);
        const auto risk = mcp_contract::ClassifyTool(
            canonical,
            manifest.value("method", std::string("GET")),
            manifest.value("path", std::string()));
        const json stepArguments = rawStep.value("arguments", json::object());
        const auto effectiveRisk = EffectiveRiskForCall(canonical, risk, stepArguments);

        if (RequiresMutationPermissionForCall(canonical, effectiveRisk, stepArguments) && !mutationPermission) {
            plan["status"] = "failed";
            plan["error"] = "mutation_permission_required";
            plan["rejected_tool"] = canonical;
            plan["risk"] = mcp_contract::RiskName(effectiveRisk);
            plan["lifecycle"]["current"] = "failed";
            return plan;
        }
        if (!SupportsTransactionalRollback(canonical, effectiveRisk, stepArguments)) {
            plan["status"] = "failed";
            plan["error"] = "primitive_lacks_safe_rollback_contract";
            plan["rejected_tool"] = canonical;
            plan["risk"] = mcp_contract::RiskName(effectiveRisk);
            plan["lifecycle"]["current"] = "failed";
            return plan;
        }
        if (RequiresMutationPermissionForCall(canonical, effectiveRisk, stepArguments)) needsTransaction = true;

        steps.push_back({stepName, canonical, std::move(manifest), stepArguments, effectiveRisk});
    }

    const int64_t timeoutMs = arguments.value("timeout_ms", static_cast<int64_t>(30000));
    const ULONGLONG started = GetTickCount64();
    const CancelFlag cancelled = RegisterCancellation(cancellationScope, requestId);

    std::unique_ptr<action::Transaction> transaction;
    if (needsTransaction) transaction = std::make_unique<action::Transaction>();
    const uint64_t checkpoint = transaction ? transaction->checkpoint() : action::Checkpoint();

    json evidence = json::array();
    for (size_t index = 0; index < steps.size(); ++index) {
        if (cancelled->load(std::memory_order_acquire)) {
            UnregisterCancellation(cancellationScope, requestId, cancelled);
            return TerminateExecution(std::move(plan), "cancelled", "request_cancelled",
                                      std::move(evidence), transaction);
        }
        if (GetTickCount64() - started >= static_cast<ULONGLONG>(timeoutMs)) {
            UnregisterCancellation(cancellationScope, requestId, cancelled);
            return TerminateExecution(std::move(plan), "timed_out", "semantic_execution_timeout",
                                      std::move(evidence), transaction);
        }

        json resolvedArguments = steps[index].arguments;
        std::string referenceError;
        if (!ResolveReferences(resolvedArguments, evidence, referenceError)) {
            UnregisterCancellation(cancellationScope, requestId, cancelled);
            plan["failed_step"] = index;
            return TerminateExecution(std::move(plan), "failed", referenceError,
                                      std::move(evidence), transaction);
        }

        json output = DispatchPrimitive(steps[index].manifest, resolvedArguments);
        evidence.push_back({
            {"step", index},
            {"tool", steps[index].canonicalName},
            {"risk", mcp_contract::RiskName(steps[index].risk)},
            {"arguments", resolvedArguments},
            {"output", output}
        });

        if (IsToolError(output, steps[index].manifest.value("ok_false_is_error", true))) {
            UnregisterCancellation(cancellationScope, requestId, cancelled);
            plan["failed_step"] = index;
            return TerminateExecution(std::move(plan), "failed", "primitive_execution_failed",
                                      std::move(evidence), transaction);
        }
        if (cancelled->load(std::memory_order_acquire)) {
            UnregisterCancellation(cancellationScope, requestId, cancelled);
            return TerminateExecution(std::move(plan), "cancelled", "request_cancelled",
                                      std::move(evidence), transaction);
        }
        if (GetTickCount64() - started >= static_cast<ULONGLONG>(timeoutMs)) {
            UnregisterCancellation(cancellationScope, requestId, cancelled);
            return TerminateExecution(std::move(plan), "timed_out", "semantic_execution_timeout",
                                      std::move(evidence), transaction);
        }
    }

    UnregisterCancellation(cancellationScope, requestId, cancelled);
    plan["status"] = "completed";
    plan["evidence"] = std::move(evidence);
    plan["elapsed_ms"] = GetTickCount64() - started;

    if (transaction) {
        if (arguments.value("rollback_on_success", false)) {
            plan["rollback_results"] = RollbackToJson(transaction->Rollback());
            plan["reversible_actions"] = json::array();
            plan["lifecycle"]["current"] = "rolled_back";
        } else {
            plan["reversible_actions"] = ActionsSince(checkpoint);
            transaction->Commit();
            plan["lifecycle"]["current"] = "completed";
        }
    } else {
        plan["reversible_actions"] = json::array();
        plan["lifecycle"]["current"] = "completed";
    }
    return plan;
}

json CallToolScoped(const std::string& wanted,
                    const json& arguments,
                    mcp_protocol::ToolProfile profile,
                    const json& requestId,
                    const std::string& cancellationScope) {
    json semanticTool;
    if (FindSemanticTool(wanted, semanticTool))
        return ToolCallPayload(ExecuteSemantic(wanted, arguments, requestId, cancellationScope));

    if (profile != mcp_protocol::ToolProfile::All) {
        return ToolCallPayload({
            {"ok", false},
            {"error", "primitive_tool_hidden"},
            {"message", "Primitive tools are hidden in the compact MCP profile. Start Cortex MCP with --tools all to expose them."}
        });
    }

    json manifest;
    if (!FindPrimitiveTool(wanted, manifest))
        return ToolCallPayload({{"ok", false}, {"error", "unknown_tool"}});

    const auto risk = mcp_contract::ClassifyTool(
        manifest.value("name", wanted),
        manifest.value("method", std::string("GET")),
        manifest.value("path", std::string()));
    const auto effectiveRisk = EffectiveRiskForCall(manifest.value("name", wanted), risk, arguments);
    if (RequiresMutationPermissionForCall(manifest.value("name", wanted), effectiveRisk, arguments) &&
        !arguments.value("mutation_permission", false)) {
        return ToolCallPayload({{"ok", false},
                                {"error", "mutation_permission_required"},
                                {"tool", manifest.value("name", wanted)},
                                {"risk", mcp_contract::RiskName(effectiveRisk)}});
    }
    return ToolCallPayload(DispatchPrimitive(manifest, arguments), manifest.value("ok_false_is_error", true));
}

} // namespace

mcp_protocol::ToolProfile ParseProfile(const std::string& value,
                                       mcp_protocol::ToolProfile fallback) {
    if (value == "compact") return mcp_protocol::ToolProfile::Compact;
    if (value == "all") return mcp_protocol::ToolProfile::All;
    return fallback;
}

json ListTools(mcp_protocol::ToolProfile profile) {
    json tools = json::array();
    for (const auto& entry : semantic::Catalog()) tools.push_back(entry);
    if (profile == mcp_protocol::ToolProfile::All) {
        for (const auto& entry : BuildToolsManifest()) {
            const std::string name = entry.value("name", std::string());
            if (name == "mcp") continue;
            tools.push_back(ManifestEntryToMcpTool(entry));
        }
    }
    return tools;
}

json CallTool(const std::string& wanted,
              const json& arguments,
              mcp_protocol::ToolProfile profile,
              const json& requestId) {
    return CallToolScoped(wanted, arguments, profile, requestId, {});
}

mcp_protocol::Result Handle(const json& input,
                            mcp_protocol::ToolProfile profile,
                            const std::string& transportProtocolVersion,
                            const std::string& cancellationScope) {
    mcp_protocol::Handler handler;
    handler.profile = profile;
    handler.transportProtocolVersion = transportProtocolVersion;
    handler.listTools = ListTools;
    handler.callTool = [cancellationScope](const std::string& name,
                                           const json& arguments,
                                           mcp_protocol::ToolProfile toolProfile,
                                           const json& requestId) {
        return CallToolScoped(name, arguments, toolProfile, requestId, cancellationScope);
    };
    handler.notification = [cancellationScope](const json& notification) {
        HandleNotification(notification, cancellationScope);
    };
    return mcp_protocol::Handle(input, handler);
}

} // namespace api::mcp_tools

