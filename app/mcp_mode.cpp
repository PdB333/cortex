#include "mcp_mode.h"

#include "ai_activity_controller.h"
#include "api/mcp_protocol.h"
#include "services/payload_client.h"
#include "target/catalog.h"
#include "target/local_backend.h"
#include "target/session_manager.h"

#include <nlohmann/json.hpp>

#include <QByteArray>
#include <QUuid>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using json = nlohmann::json;
using cortex::target::TargetDescriptor;

constexpr std::size_t kMaxStdioMessageBytes = 4u * 1024u * 1024u;
constexpr std::size_t kMaxConcurrentRequests = 64;

struct TargetSelector {
    std::optional<std::uint64_t> pid;
    std::string process;
};

struct Options {
    std::vector<TargetSelector> targets;
    std::string toolProfile = "compact";
    bool help = false;
};

struct TargetRuntime {
    TargetDescriptor target;
    std::unique_ptr<cortex::target::SessionManager> sessions;
    std::unique_ptr<cortex::services::PayloadClient> payload;
};

using TargetRuntimePtr = std::shared_ptr<TargetRuntime>;
using RuntimeList = std::vector<TargetRuntimePtr>;

struct RunState {
    std::mutex outputMutex;
    std::mutex activeMutex;
    std::condition_variable activeChanged;
    std::size_t active = 0;

    // shared_ptr snapshots keep a detached runtime alive until any in-flight
    // request that already selected it has completed.
    std::mutex runtimeMutex;
    std::mutex targetMutationMutex;
    // Dynamic targets can be attached after the MCP lifecycle handshake.
    // Cache legacy initialize state so a newly attached runtime can be
    // brought to the same protocol state before it becomes routable.
    std::mutex protocolMutex;
    std::optional<json> initializeMessage;
    bool initializedNotificationSeen = false;
    RuntimeList runtimes;
    cortex::target::Catalog* catalog = nullptr;
    std::string runtimeDirectory;
    std::string toolProfile;
    std::atomic<std::uint64_t> activitySequence{1};
    std::string activitySessionId;
    std::string activityClientName = "MCP client";
    std::string activityClientVersion;
};

std::string LowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

void PrintUsage(std::ostream& stream) {
    stream << "Cortex MCP stdio mode\n\n"
           << "Usage:\n"
           << "  cortex.exe mcp [--tools compact|all]\n"
           << "  cortex.exe mcp --pid <pid> [--pid <pid> ...] [--tools compact|all]\n"
           << "  cortex.exe mcp --process <name> [--process <name> ...] [--tools compact|all]\n"
           << "  cortex.exe mcp --pid <pid> --process <name> [--tools compact|all]\n\n"
           << "Without --pid/--process, Cortex starts targetless and exposes cortex_processes,\n"
           << "cortex_attach, cortex_detach and cortex_targets so an MCP client can choose\n"
           << "processes dynamically. --pid/--process remain startup auto-attach shortcuts.\n"
           << "With multiple attached targets, normal tools/call requests select a target\n"
           << "through the _cortex_target argument exposed in tools/list.\n";
}

bool ParseOptions(int argc, char** argv, Options& options, std::string& error) {
    for (int index = 2; index < argc; ++index) {
        const std::string argument = argv[index] ? argv[index] : "";
        if (argument == "--help" || argument == "-h") {
            options.help = true;
            continue;
        }
        if (argument == "--pid" && index + 1 < argc) {
            const std::string value = argv[++index] ? argv[index] : "";
            try {
                std::size_t consumed = 0;
                const auto pid = std::stoull(value, &consumed, 10);
                if (consumed != value.size() || pid == 0) throw std::invalid_argument("pid");
                TargetSelector selector;
                selector.pid = static_cast<std::uint64_t>(pid);
                options.targets.push_back(std::move(selector));
            } catch (...) {
                error = "--pid must be a positive integer";
                return false;
            }
            continue;
        }
        if (argument == "--process" && index + 1 < argc) {
            TargetSelector selector;
            selector.process = argv[++index] ? argv[index] : "";
            if (selector.process.empty()) {
                error = "--process requires a non-empty process name";
                return false;
            }
            options.targets.push_back(std::move(selector));
            continue;
        }
        if (argument == "--tools" && index + 1 < argc) {
            options.toolProfile = argv[++index] ? argv[index] : "";
            if (options.toolProfile != "compact" && options.toolProfile != "all") {
                error = "--tools must be compact or all";
                return false;
            }
            continue;
        }
        error = "unknown or incomplete argument: " + argument;
        return false;
    }

    return true;
}

std::optional<TargetDescriptor> ResolveUniqueTarget(const TargetSelector& selector,
                                                     const std::vector<TargetDescriptor>& targets,
                                                     std::string& error) {
    if (selector.pid) {
        const auto found = std::find_if(targets.begin(), targets.end(), [&](const TargetDescriptor& target) {
            return target.processId == *selector.pid;
        });
        if (found == targets.end()) {
            error = "target pid not found: " + std::to_string(*selector.pid);
            return std::nullopt;
        }
        return *found;
    }

    const std::string wanted = LowerAscii(selector.process);
    std::vector<TargetDescriptor> exact;
    std::vector<TargetDescriptor> partial;
    for (const auto& target : targets) {
        const std::string name = LowerAscii(target.name);
        if (name == wanted) exact.push_back(target);
        else if (name.find(wanted) != std::string::npos) partial.push_back(target);
    }

    const auto& matches = exact.empty() ? partial : exact;
    if (matches.empty()) {
        error = "process not found: " + selector.process;
        return std::nullopt;
    }
    if (matches.size() > 1) {
        error = "process target is ambiguous: " + selector.process + " (matching pids";
        const std::size_t shown = std::min<std::size_t>(matches.size(), 8);
        for (std::size_t index = 0; index < shown; ++index)
            error += (index == 0 ? " " : ", ") + std::to_string(matches[index].processId);
        if (matches.size() > shown) error += ", ...";
        error += "); use --pid";
        return std::nullopt;
    }
    return matches.front();
}

json MessageId(const json& message) {
    if (message.is_object() && message.contains("id")) return message.at("id");
    return nullptr;
}

bool IsNotification(const json& message) {
    return message.is_object() && !message.contains("id");
}

json TransportError(const json& id, const std::string& code, const std::string& message) {
    return {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"error", {
            {"code", -32000},
            {"message", message},
            {"data", {{"code", code}}}
        }}
    };
}

void WriteOutput(const std::shared_ptr<RunState>& state, const json& response) {
    std::lock_guard<std::mutex> lock(state->outputMutex);
    std::cout << response.dump() << '\n';
    std::cout.flush();
}

bool ReserveWorker(const std::shared_ptr<RunState>& state) {
    std::lock_guard<std::mutex> lock(state->activeMutex);
    if (state->active >= kMaxConcurrentRequests) return false;
    ++state->active;
    return true;
}

void ReleaseWorker(const std::shared_ptr<RunState>& state) {
    {
        std::lock_guard<std::mutex> lock(state->activeMutex);
        if (state->active > 0) --state->active;
    }
    state->activeChanged.notify_all();
}

void WaitForWorkers(const std::shared_ptr<RunState>& state) {
    std::unique_lock<std::mutex> lock(state->activeMutex);
    state->activeChanged.wait(lock, [&] { return state->active == 0; });
}

std::uint64_t ActivityTimestampMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string ActivityScalarText(const json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_number_unsigned()) return std::to_string(value.get<std::uint64_t>());
    if (value.is_number_integer()) return std::to_string(value.get<std::int64_t>());
    if (value.is_number_float()) return std::to_string(value.get<double>());
    if (value.is_boolean()) return value.get<bool>() ? "true" : "false";
    return {};
}

bool IsSensitiveActivityKey(const std::string& key) {
    const std::string lowered = LowerAscii(key);
    return lowered.find("token") != std::string::npos ||
           lowered.find("password") != std::string::npos ||
           lowered.find("secret") != std::string::npos ||
           lowered.find("authorization") != std::string::npos ||
           lowered.find("credential") != std::string::npos ||
           lowered.find("api_key") != std::string::npos ||
           lowered.find("apikey") != std::string::npos ||
           lowered.find("cookie") != std::string::npos;
}

void SanitizeActivityJson(json& value, int depth = 0) {
    if (depth > 4) {
        value = "<nested>";
        return;
    }
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (IsSensitiveActivityKey(it.key())) it.value() = "<redacted>";
            else SanitizeActivityJson(it.value(), depth + 1);
        }
        return;
    }
    if (value.is_array()) {
        if (value.size() > 24) {
            value = "<array: " + std::to_string(value.size()) + " items>";
            return;
        }
        for (auto& item : value) SanitizeActivityJson(item, depth + 1);
        return;
    }
    if (value.is_string()) {
        std::string text = value.get<std::string>();
        if (text.size() > 320) {
            text.resize(320);
            text += "...";
            value = std::move(text);
        }
    }
}

std::string CompactActivityDetails(const json& source, std::size_t maxBytes = 1400) {
    json sanitized = source;
    SanitizeActivityJson(sanitized);
    std::string text = sanitized.dump();
    if (text.size() > maxBytes) {
        text.resize(maxBytes);
        text += "...";
    }
    return text;
}

void PublishActivity(const std::shared_ptr<RunState>& state, json event) {
    if (!state || state->activitySessionId.empty()) return;
    event["schema"] = "cortex.ai.activity.v1";
    event["session_id"] = state->activitySessionId;
    event["timestamp_ms"] = ActivityTimestampMs();
    event["sequence"] = state->activitySequence.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(state->protocolMutex);
        event["client"] = state->activityClientName;
        if (!state->activityClientVersion.empty()) event["client_version"] = state->activityClientVersion;
    }
    cortex::app::PublishAiActivity(QByteArray::fromStdString(event.dump()), 5);
}

void PublishSessionActivity(const std::shared_ptr<RunState>& state,
                            const std::string& phase,
                            const std::string& summary,
                            const std::string& details = {}) {
    json event = {{"kind", "session"}, {"phase", phase}, {"summary", summary}};
    if (!details.empty()) event["details"] = details;
    PublishActivity(state, std::move(event));
}

json ToolPayload(const json& value, bool isError = false) {
    return {
        {"content", json::array({{{"type", "text"}, {"text", value.dump(2)}}})},
        {"structuredContent", value},
        {"isError", isError}
    };
}

json LocalToolResponse(const json& id, const json& value, bool isError = false) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", ToolPayload(value, isError)}};
}

json LocalToolFailure(const std::string& code, const std::string& message) {
    return {{"ok", false}, {"error", {{"code", code}, {"message", message}}}};
}

json TargetDescriptorJson(const TargetDescriptor& target, bool attached, bool alive = true) {
    return {
        {"id", target.id}, {"name", target.name}, {"pid", target.processId},
        {"selector", std::to_string(target.processId)},
        {"platform", cortex::target::PlatformName(target.platform)},
        {"architecture", cortex::target::ArchitectureName(target.architecture)},
        {"executable_path", target.executablePath}, {"window_title", target.windowTitle},
        {"attached", attached}, {"alive", alive}
    };
}

RuntimeList RuntimeSnapshot(const std::shared_ptr<RunState>& state) {
    std::lock_guard<std::mutex> lock(state->runtimeMutex);
    return state->runtimes;
}

bool PruneDeadRuntimes(const std::shared_ptr<RunState>& state) {
    std::lock_guard<std::mutex> lock(state->runtimeMutex);
    const auto oldSize = state->runtimes.size();
    state->runtimes.erase(std::remove_if(state->runtimes.begin(), state->runtimes.end(), [](const TargetRuntimePtr& runtime) {
        return !runtime || !runtime->sessions || !runtime->sessions->Active() || !runtime->sessions->Active()->Alive();
    }), state->runtimes.end());
    return state->runtimes.size() != oldSize;
}

void EmitToolsListChanged(const std::shared_ptr<RunState>& state) {
    WriteOutput(state, {{"jsonrpc", "2.0"}, {"method", "notifications/tools/list_changed"}});
}

std::string ActivityTargetText(const std::shared_ptr<RunState>& state, const json& arguments) {
    if (arguments.is_object()) {
        if (arguments.contains("_cortex_target")) {
            const std::string selected = ActivityScalarText(arguments.at("_cortex_target"));
            if (!selected.empty()) return selected;
        }
        if (arguments.contains("pid")) {
            const std::string pid = ActivityScalarText(arguments.at("pid"));
            if (!pid.empty()) return "PID " + pid;
        }
        if (arguments.contains("process") && arguments.at("process").is_string())
            return arguments.at("process").get<std::string>();
    }

    const auto runtimes = RuntimeSnapshot(state);
    if (runtimes.size() == 1 && runtimes.front())
        return runtimes.front()->target.name + " (PID " + std::to_string(runtimes.front()->target.processId) + ")";
    return {};
}

std::string HumanizeToolSuffix(std::string text) {
    std::replace(text.begin(), text.end(), '_', ' ');
    return text;
}

std::string ToolStartSummary(const std::string& name) {
    if (name == "cortex_processes") return "Searching processes";
    if (name == "cortex_attach") return "Attaching target";
    if (name == "cortex_detach") return "Detaching target";
    if (name == "cortex_targets") return "Listing attached targets";
    if (name == "modules") return "Listing modules";
    if (name == "memory_read") return "Reading memory";
    if (name.rfind("memory_write", 0) == 0 || name.rfind("patch", 0) == 0) return "Writing memory";
    if (name.find("scan") != std::string::npos) return "Scanning memory";
    if (name == "disasm" || name.find("disassembl") != std::string::npos) return "Disassembling";
    if (name.rfind("debug_", 0) == 0) return "Debugger: " + HumanizeToolSuffix(name.substr(6));
    if (name.find("symbol") != std::string::npos) return "Resolving symbols";
    return "Running " + HumanizeToolSuffix(name);
}

json ActivityResponsePayload(const json& response) {
    if (!response.is_object()) return response;
    if (response.contains("result") && response.at("result").is_object()) {
        const auto& result = response.at("result");
        if (result.contains("structuredContent")) return result.at("structuredContent");
    }
    if (response.contains("error")) return response.at("error");
    return response;
}

bool ActivityResponseFailed(const json& response) {
    if (!response.is_object()) return false;
    if (response.contains("error")) return true;
    if (response.contains("result") && response.at("result").is_object()) {
        const auto& result = response.at("result");
        return result.value("isError", false);
    }
    return false;
}

std::string ActivityFailureText(const json& response, const std::string& transportError) {
    if (!transportError.empty()) return transportError;
    const json payload = ActivityResponsePayload(response);
    if (payload.is_object()) {
        if (payload.contains("message") && payload.at("message").is_string()) return payload.at("message").get<std::string>();
        if (payload.contains("error")) {
            const auto& error = payload.at("error");
            if (error.is_string()) return error.get<std::string>();
            if (error.is_object() && error.contains("message") && error.at("message").is_string())
                return error.at("message").get<std::string>();
        }
    }
    return "Tool call failed";
}

std::string ToolCompletionSummary(const std::string& name, const json& response, bool failed,
                                  const std::string& transportError) {
    if (failed) return "Failed: " + ActivityFailureText(response, transportError);
    const json payload = ActivityResponsePayload(response);
    if (payload.is_object()) {
        if (payload.contains("status") && payload.at("status").is_string())
            return HumanizeToolSuffix(payload.at("status").get<std::string>());
        if (payload.contains("count") && payload.at("count").is_number_integer())
            return std::to_string(payload.at("count").get<std::int64_t>()) + " result(s)";
        for (const char* key : {"processes", "targets", "addresses", "instructions", "modules", "threads"}) {
            if (payload.contains(key) && payload.at(key).is_array())
                return std::to_string(payload.at(key).size()) + " " + std::string(key);
        }
        if (name == "memory_read" && payload.contains("value")) {
            const std::string value = ActivityScalarText(payload.at("value"));
            if (!value.empty()) return "Read " + value;
        }
    }
    return "Completed";
}

void PublishToolActivity(const std::shared_ptr<RunState>& state,
                         const json& requestId,
                         const std::string& name,
                         const std::string& target,
                         const std::string& phase,
                         const std::string& summary,
                         const std::string& details,
                         std::optional<std::uint64_t> durationMs = std::nullopt) {
    json event = {
        {"kind", "tool"}, {"phase", phase}, {"request_id", requestId},
        {"tool", name}, {"target", target}, {"summary", summary}
    };
    if (!details.empty()) event["details"] = details;
    if (durationMs) event["duration_ms"] = *durationMs;
    PublishActivity(state, std::move(event));
}

json LocalTools(bool requireDetachTarget = false) {
    json tools = json::array({
        {{"name", "cortex_processes"},
         {"description", "List local processes Cortex can discover before or after attaching. Optionally filter by process name, path, window title, or target id."},
         {"inputSchema", {{"type", "object"}, {"properties", {
             {"query", {{"type", "string"}, {"description", "Optional case-insensitive filter."}}},
             {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 2048}, {"default", 256}}}
         }}, {"additionalProperties", false}}},
         {"_cortex", {{"host_control", true}, {"read_only", true}}}},
        {{"name", "cortex_attach"},
         {"description", "Attach a newly selected local process to this existing Cortex MCP connection. Provide exactly one of pid or process."},
         {"inputSchema", {{"type", "object"}, {"properties", {
             {"pid", {{"type", "integer"}, {"minimum", 1}}},
             {"process", {{"type", "string"}, {"minLength", 1}, {"description", "Unique process name; exact matches are preferred over partial matches."}}}
         }}, {"oneOf", json::array({
             {{"required", json::array({"pid"})}, {"not", {{"required", json::array({"process"})}}}},
             {{"required", json::array({"process"})}, {"not", {{"required", json::array({"pid"})}}}}
         })}, {"additionalProperties", false}}},
         {"_cortex", {{"host_control", true}, {"dynamic_target", true}}}},
        {{"name", "cortex_detach"},
         {"description", "Detach one process from this Cortex MCP connection. If exactly one target is attached, _cortex_target may be omitted."},
         {"inputSchema", {{"type", "object"}, {"properties", {
             {"_cortex_target", {{"oneOf", json::array({{{"type", "integer"}}, {{"type", "string"}}})},
                                  {"description", "Attached target PID, target id, or unique process name."}}}
         }}, {"additionalProperties", false}}},
         {"_cortex", {{"host_control", true}, {"dynamic_target", true}}}},
        {{"name", "cortex_targets"},
         {"description", "List the processes currently attached to this Cortex MCP server and the selectors accepted by _cortex_target."},
         {"inputSchema", {{"type", "object"}, {"properties", json::object()}, {"additionalProperties", false}}},
         {"_cortex", {{"host_control", true}, {"multi_target_router", true}, {"read_only", true}}}}
    });
    if (requireDetachTarget)
        tools[2]["inputSchema"]["required"] = json::array({"_cortex_target"});
    return tools;
}

void PatchDynamicHandshake(json& response, std::size_t attachedCount) {
    if (!response.is_object() || !response.contains("result") || !response["result"].is_object()) return;
    auto& result = response["result"];
    if (!result.contains("capabilities") || !result["capabilities"].is_object()) result["capabilities"] = json::object();
    if (!result["capabilities"].contains("tools") || !result["capabilities"]["tools"].is_object()) result["capabilities"]["tools"] = json::object();
    result["capabilities"]["tools"]["listChanged"] = true;
    if (result.contains("instructions") && result["instructions"].is_string()) {
        std::string instructions = result["instructions"].get<std::string>();
        if (!instructions.empty()) instructions += " ";
        instructions += "This MCP connection supports dynamic Cortex targets. Use cortex_processes, cortex_attach, cortex_detach and cortex_targets; tools/list changes after attach/detach.";
        if (attachedCount == 0)
            instructions += " No target is attached yet.";
        else if (attachedCount > 1)
            instructions += " Multiple Cortex targets are attached; pass _cortex_target on normal runtime tool calls.";
        result["instructions"] = std::move(instructions);
    }
}

bool HandleLocalProtocol(const std::shared_ptr<RunState>& state, const json& message, json& response, bool& hasResponse) {
    api::mcp_protocol::Handler handler;
    handler.profile = state->toolProfile == "compact" ? api::mcp_protocol::ToolProfile::Compact : api::mcp_protocol::ToolProfile::All;
    handler.listTools = [](api::mcp_protocol::ToolProfile) { return LocalTools(); };
    const auto result = api::mcp_protocol::Handle(message, handler);
    response = result.response;
    hasResponse = result.hasResponse;
    if (hasResponse && message.is_object()) {
        const std::string method = message.value("method", std::string());
        if (method == "initialize" || method == "server/discover")
            PatchDynamicHandshake(response, RuntimeSnapshot(state).size());
    }
    return true;
}
void RememberInitialize(const std::shared_ptr<RunState>& state, const json& message) {
    std::string clientName;
    std::string clientVersion;
    if (message.is_object() && message.contains("params") && message.at("params").is_object()) {
        const auto& params = message.at("params");
        if (params.contains("clientInfo") && params.at("clientInfo").is_object()) {
            const auto& clientInfo = params.at("clientInfo");
            clientName = clientInfo.value("name", std::string());
            clientVersion = clientInfo.value("version", std::string());
        }
    }
    std::lock_guard<std::mutex> lock(state->protocolMutex);
    state->initializeMessage = message;
    state->initializedNotificationSeen = false;
    if (!clientName.empty()) state->activityClientName = clientName;
    state->activityClientVersion = clientVersion;
}

void RememberInitializedNotification(const std::shared_ptr<RunState>& state) {
    std::lock_guard<std::mutex> lock(state->protocolMutex);
    state->initializedNotificationSeen = true;
}

bool PrimeRuntimeProtocol(const std::shared_ptr<RunState>& state,
                          const TargetRuntimePtr& runtime,
                          std::string& error) {
    error.clear();
    if (!runtime || !runtime->payload) {
        error = "target runtime payload is unavailable";
        return false;
    }

    std::optional<json> initialize;
    bool initialized = false;
    {
        std::lock_guard<std::mutex> lock(state->protocolMutex);
        initialize = state->initializeMessage;
        initialized = state->initializedNotificationSeen;
    }
    if (!initialize) return true;

    json ignoredResponse;
    bool ignoredHasResponse = false;
    if (!runtime->payload->ForwardMcp(*initialize, state->toolProfile, ignoredResponse, ignoredHasResponse, &error))
        return false;

    if (initialized) {
        const json notification = {
            {"jsonrpc", "2.0"},
            {"method", "notifications/initialized"}
        };
        ignoredResponse = json();
        ignoredHasResponse = false;
        if (!runtime->payload->ForwardMcp(notification, state->toolProfile, ignoredResponse, ignoredHasResponse, &error))
            return false;
    }
    return true;
}

void PrimeExistingRuntimes(const std::shared_ptr<RunState>& state,
                           const json& initializeMessage) {
    const auto runtimes = RuntimeSnapshot(state);
    for (const auto& runtime : runtimes) {
        if (!runtime || !runtime->payload) continue;
        json ignoredResponse;
        bool ignoredHasResponse = false;
        std::string ignoredError;
        runtime->payload->ForwardMcp(initializeMessage, state->toolProfile, ignoredResponse, ignoredHasResponse, &ignoredError);
    }
}
std::string TargetSummaryText(const RuntimeList& runtimes) {
    std::string result;
    for (std::size_t index = 0; index < runtimes.size(); ++index) {
        if (index != 0) result += ", ";
        result += runtimes[index]->target.name + " (PID " + std::to_string(runtimes[index]->target.processId) + ")";
    }
    return result;
}

json TargetListResult(const RuntimeList& runtimes) {
    json targets = json::array();
    for (const auto& runtime : runtimes) {
        const bool alive = runtime && runtime->sessions && runtime->sessions->Active() && runtime->sessions->Active()->Alive();
        if (runtime) targets.push_back(TargetDescriptorJson(runtime->target, true, alive));
    }
    return {{"ok", true}, {"count", targets.size()}, {"targets", std::move(targets)}};
}

TargetRuntimePtr ResolveRuntime(const RuntimeList& runtimes,
                                const json& arguments,
                                std::string& errorCode,
                                std::string& errorMessage) {
    errorCode.clear();
    errorMessage.clear();
    if (runtimes.empty()) {
        errorCode = "no_attached_targets";
        errorMessage = "No Cortex targets are attached";
        return nullptr;
    }

    if (!arguments.contains("_cortex_target")) {
        if (runtimes.size() == 1) return runtimes.front();
        errorCode = "cortex_target_required";
        errorMessage = "Multiple Cortex targets are attached; set _cortex_target to a PID, target id, or unique process name. Available: " + TargetSummaryText(runtimes);
        return nullptr;
    }

    const json& selector = arguments.at("_cortex_target");
    if (selector.is_number_unsigned()) {
        const std::uint64_t pid = selector.get<std::uint64_t>();
        if (pid == 0) {
            errorCode = "invalid_cortex_target";
            errorMessage = "_cortex_target PID must be positive";
            return nullptr;
        }
        for (const auto& runtime : runtimes) {
            if (runtime->target.processId == pid) return runtime;
        }
    } else if (selector.is_number_integer()) {
        const std::int64_t signedPid = selector.get<std::int64_t>();
        if (signedPid <= 0) {
            errorCode = "invalid_cortex_target";
            errorMessage = "_cortex_target PID must be positive";
            return nullptr;
        }
        const auto pid = static_cast<std::uint64_t>(signedPid);
        for (const auto& runtime : runtimes) {
            if (runtime->target.processId == pid) return runtime;
        }
    } else if (selector.is_string()) {
        const std::string wanted = selector.get<std::string>();
        for (const auto& runtime : runtimes) {
            if (runtime->target.id == wanted) return runtime;
        }

        try {
            std::size_t consumed = 0;
            const auto pid = std::stoull(wanted, &consumed, 10);
            if (consumed == wanted.size()) {
                for (const auto& runtime : runtimes) {
                    if (runtime->target.processId == pid) return runtime;
                }
            }
        } catch (...) {
        }

        const std::string lowered = LowerAscii(wanted);
        TargetRuntimePtr exact;
        for (const auto& runtime : runtimes) {
            if (LowerAscii(runtime->target.name) != lowered) continue;
            if (exact) {
                errorCode = "cortex_target_ambiguous";
                errorMessage = "Process name matches more than one attached target; use PID or target id";
                return nullptr;
            }
            exact = runtime;
        }
        if (exact) return exact;
    } else {
        errorCode = "invalid_cortex_target";
        errorMessage = "_cortex_target must be a PID integer or a target id/name string";
        return nullptr;
    }

    errorCode = "cortex_target_not_found";
    errorMessage = "Requested Cortex target is not attached. Available: " + TargetSummaryText(runtimes);
    return nullptr;
}

json ProcessListResult(const std::shared_ptr<RunState>& state, const json& arguments, std::string& errorCode, std::string& errorMessage) {
    errorCode.clear();
    errorMessage.clear();
    if (!state->catalog) {
        errorCode = "target_catalog_unavailable";
        errorMessage = "Local target catalog is unavailable";
        return {};
    }

    std::string query;
    if (arguments.contains("query")) {
        if (!arguments["query"].is_string()) {
            errorCode = "invalid_query";
            errorMessage = "query must be a string";
            return {};
        }
        query = LowerAscii(arguments["query"].get<std::string>());
    }

    std::size_t limit = 256;
    if (arguments.contains("limit")) {
        if (!arguments["limit"].is_number_integer()) {
            errorCode = "invalid_limit";
            errorMessage = "limit must be an integer between 1 and 2048";
            return {};
        }
        const auto requested = arguments["limit"].get<std::int64_t>();
        if (requested < 1 || requested > 2048) {
            errorCode = "invalid_limit";
            errorMessage = "limit must be an integer between 1 and 2048";
            return {};
        }
        limit = static_cast<std::size_t>(requested);
    }

    const auto attached = RuntimeSnapshot(state);
    const auto discovered = state->catalog->Targets();
    json processes = json::array();
    std::size_t totalMatches = 0;
    for (const auto& target : discovered) {
        if (!query.empty()) {
            const std::string searchable = LowerAscii(target.name + "\n" + target.executablePath + "\n" + target.windowTitle + "\n" + target.id);
            if (searchable.find(query) == std::string::npos) continue;
        }
        ++totalMatches;
        if (processes.size() >= limit) continue;
        const bool isAttached = std::any_of(attached.begin(), attached.end(), [&](const TargetRuntimePtr& runtime) {
            return runtime && runtime->target.id == target.id;
        });
        processes.push_back(TargetDescriptorJson(target, isAttached));
    }
    return {
        {"ok", true}, {"count", processes.size()}, {"total_matches", totalMatches},
        {"truncated", totalMatches > processes.size()}, {"processes", std::move(processes)}
    };
}

TargetRuntimePtr CreateRuntime(cortex::target::Catalog& catalog,
                               const TargetDescriptor& target,
                               const std::string& runtimeDirectory,
                               std::string& errorCode,
                               std::string& errorMessage) {
    errorCode.clear();
    errorMessage.clear();
    auto runtime = std::make_shared<TargetRuntime>();
    runtime->target = target;
    runtime->sessions = std::make_unique<cortex::target::SessionManager>(catalog);
    std::string error;
    if (!runtime->sessions->Attach(target, &error)) {
        errorCode = "target_attach_failed";
        errorMessage = error.empty() ? "Target attach failed" : error;
        return {};
    }
    runtime->payload = std::make_unique<cortex::services::PayloadClient>(*runtime->sessions, runtimeDirectory);
    if (!runtime->payload->EnsureReady(&error)) {
        errorCode = "target_runtime_unavailable";
        errorMessage = error.empty() ? "Target runtime is unavailable" : error;
        return {};
    }
    return runtime;
}

bool HandleLocalTool(const std::shared_ptr<RunState>& state,
                     const json& message,
                     const std::string& name,
                     const json& arguments,
                     json& response) {
    if (name != "cortex_processes" && name != "cortex_attach" &&
        name != "cortex_detach" && name != "cortex_targets") return false;

    if (PruneDeadRuntimes(state)) EmitToolsListChanged(state);

    if (name == "cortex_targets") {
        response = LocalToolResponse(MessageId(message), TargetListResult(RuntimeSnapshot(state)));
        return true;
    }

    if (name == "cortex_processes") {
        std::string errorCode;
        std::string errorMessage;
        const json result = ProcessListResult(state, arguments, errorCode, errorMessage);
        if (!errorCode.empty()) response = LocalToolResponse(MessageId(message), LocalToolFailure(errorCode, errorMessage), true);
        else response = LocalToolResponse(MessageId(message), result);
        return true;
    }

    std::lock_guard<std::mutex> mutationLock(state->targetMutationMutex);
    if (name == "cortex_attach") {
        const bool hasPid = arguments.contains("pid");
        const bool hasProcess = arguments.contains("process");
        if (hasPid == hasProcess) {
            response = LocalToolResponse(MessageId(message), LocalToolFailure("invalid_attach_selector", "Provide exactly one of pid or process"), true);
            return true;
        }

        TargetSelector selector;
        if (hasPid) {
            if (!arguments["pid"].is_number_integer()) {
                response = LocalToolResponse(MessageId(message), LocalToolFailure("invalid_pid", "pid must be a positive integer"), true);
                return true;
            }
            const auto pid = arguments["pid"].get<std::int64_t>();
            if (pid <= 0) {
                response = LocalToolResponse(MessageId(message), LocalToolFailure("invalid_pid", "pid must be a positive integer"), true);
                return true;
            }
            selector.pid = static_cast<std::uint64_t>(pid);
        } else {
            if (!arguments["process"].is_string() || arguments["process"].get<std::string>().empty()) {
                response = LocalToolResponse(MessageId(message), LocalToolFailure("invalid_process", "process must be a non-empty string"), true);
                return true;
            }
            selector.process = arguments["process"].get<std::string>();
        }

        if (!state->catalog) {
            response = LocalToolResponse(MessageId(message), LocalToolFailure("target_catalog_unavailable", "Local target catalog is unavailable"), true);
            return true;
        }
        std::string resolveError;
        const auto target = ResolveUniqueTarget(selector, state->catalog->Targets(), resolveError);
        if (!target) {
            response = LocalToolResponse(MessageId(message), LocalToolFailure("target_not_found", resolveError), true);
            return true;
        }

        {
            const auto current = RuntimeSnapshot(state);
            const auto existing = std::find_if(current.begin(), current.end(), [&](const TargetRuntimePtr& runtime) {
                return runtime && runtime->target.id == target->id;
            });
            if (existing != current.end()) {
                response = LocalToolResponse(MessageId(message), {
                    {"ok", true}, {"status", "already_attached"},
                    {"target", TargetDescriptorJson((*existing)->target, true)}, {"count", current.size()}
                });
                return true;
            }
        }

        std::string errorCode;
        std::string errorMessage;
        auto runtime = CreateRuntime(*state->catalog, *target, state->runtimeDirectory, errorCode, errorMessage);
        if (!runtime) {
            response = LocalToolResponse(MessageId(message), LocalToolFailure(errorCode, errorMessage), true);
            return true;
        }
        if (!PrimeRuntimeProtocol(state, runtime, errorMessage)) {
            response = LocalToolResponse(MessageId(message),
                                         LocalToolFailure("target_protocol_init_failed",
                                                          errorMessage.empty() ? "Could not initialize the attached runtime MCP session"
                                                                               : errorMessage),
                                         true);
            return true;
        }
        std::size_t count = 0;
        {
            std::lock_guard<std::mutex> lock(state->runtimeMutex);
            state->runtimes.push_back(runtime);
            count = state->runtimes.size();
        }
        response = LocalToolResponse(MessageId(message), {
            {"ok", true}, {"status", "attached"}, {"target", TargetDescriptorJson(runtime->target, true)}, {"count", count}
        });
        EmitToolsListChanged(state);
        return true;
    }

    const auto current = RuntimeSnapshot(state);
    std::string errorCode;
    std::string errorMessage;
    auto runtime = ResolveRuntime(current, arguments, errorCode, errorMessage);
    if (!runtime) {
        response = LocalToolResponse(MessageId(message), LocalToolFailure(errorCode, errorMessage), true);
        return true;
    }
    std::size_t count = 0;
    {
        std::lock_guard<std::mutex> lock(state->runtimeMutex);
        state->runtimes.erase(std::remove_if(state->runtimes.begin(), state->runtimes.end(), [&](const TargetRuntimePtr& candidate) {
            return candidate && candidate->target.id == runtime->target.id;
        }), state->runtimes.end());
        count = state->runtimes.size();
    }
    response = LocalToolResponse(MessageId(message), {
        {"ok", true}, {"status", "detached"}, {"target", TargetDescriptorJson(runtime->target, false)}, {"count", count}
    });
    EmitToolsListChanged(state);
    return true;
}
void AugmentToolsList(json& response, const RuntimeList& runtimes) {
    if (!response.is_object() || !response.contains("result") || !response["result"].is_object()) return;
    auto& result = response["result"];
    if (!result.contains("tools") || !result["tools"].is_array()) return;

    const bool requireTarget = runtimes.size() > 1;
    const std::string description =
        "Select the attached Cortex target for this call by PID, target id, or unique process name. Available: " +
        TargetSummaryText(runtimes);

    json augmented = LocalTools(requireTarget);

    for (auto tool : result["tools"]) {
        if (!tool.is_object()) {
            augmented.push_back(std::move(tool));
            continue;
        }
        if (!tool.contains("inputSchema") || !tool["inputSchema"].is_object())
            tool["inputSchema"] = {{"type", "object"}, {"properties", json::object()}};
        auto& schema = tool["inputSchema"];
        if (!schema.contains("properties") || !schema["properties"].is_object()) schema["properties"] = json::object();
        schema["properties"]["_cortex_target"] = {
            {"oneOf", json::array({{{"type", "integer"}}, {{"type", "string"}}})},
            {"description", description}
        };
        if (requireTarget) {
            if (!schema.contains("required") || !schema["required"].is_array()) schema["required"] = json::array();
            bool present = false;
            for (const auto& entry : schema["required"]) {
                if (entry.is_string() && entry.get<std::string>() == "_cortex_target") {
                    present = true;
                    break;
                }
            }
            if (!present) schema["required"].push_back("_cortex_target");
        }
        if (!tool.contains("_cortex") || !tool["_cortex"].is_object()) tool["_cortex"] = json::object();
        tool["_cortex"]["target_routed"] = true;
        augmented.push_back(std::move(tool));
    }
    result["tools"] = std::move(augmented);
}

bool ForwardOne(const std::shared_ptr<RunState>& state,
                const json& message,
                json& response,
                bool& hasResponse,
                std::string* error) {
    response = json();
    hasResponse = false;
    if (error) error->clear();

    if (PruneDeadRuntimes(state)) EmitToolsListChanged(state);
    const RuntimeList runtimes = RuntimeSnapshot(state);

    if (!message.is_object()) {
        if (runtimes.empty()) return HandleLocalProtocol(state, message, response, hasResponse);
        return runtimes.front()->payload->ForwardMcp(message, state->toolProfile, response, hasResponse, error);
    }

    if (IsNotification(message)) {
        const std::string notificationMethod = message.value("method", std::string());
        std::unique_lock<std::mutex> lifecycleLock;
        RuntimeList notificationRuntimes = runtimes;
        if (notificationMethod == "notifications/initialized") {
            // Serialize lifecycle changes with attach/detach. An attach that
            // completes after this point will replay both initialize and the
            // initialized notification before becoming routable.
            lifecycleLock = std::unique_lock<std::mutex>(state->targetMutationMutex);
            RememberInitializedNotification(state);
            notificationRuntimes = RuntimeSnapshot(state);
        }
        if (notificationRuntimes.empty()) return HandleLocalProtocol(state, message, response, hasResponse);
        std::string firstError;
        for (const auto& runtime : notificationRuntimes) {
            json notificationResponse;
            bool notificationHasResponse = false;
            std::string notificationError;
            if (!runtime->payload->ForwardMcp(message, state->toolProfile, notificationResponse,
                                              notificationHasResponse, &notificationError)) {
                if (firstError.empty()) firstError = notificationError;
                continue;
            }
            if (!hasResponse && notificationHasResponse) {
                response = std::move(notificationResponse);
                hasResponse = true;
            }
        }
        if (!firstError.empty() && !hasResponse) {
            if (error) *error = firstError;
            return false;
        }
        return true;
    }

    const std::string method = message.value("method", std::string());
    if (method == "initialize") {
        // Keep host handshake semantics local while bringing every currently
        // attached payload to the same legacy MCP lifecycle state. The same
        // mutex is used by dynamic attach/detach, eliminating the window where
        // a target could become routable without receiving initialize.
        std::lock_guard<std::mutex> lifecycleLock(state->targetMutationMutex);
        RememberInitialize(state, message);
        PrimeExistingRuntimes(state, message);
        return HandleLocalProtocol(state, message, response, hasResponse);
    }
    if (method == "server/discover" || method == "ping")
        return HandleLocalProtocol(state, message, response, hasResponse);

    if (method == "tools/list") {
        if (runtimes.empty()) return HandleLocalProtocol(state, message, response, hasResponse);

        std::string firstError;
        for (const auto& runtime : runtimes) {
            std::string runtimeError;
            if (runtime->payload->ForwardMcp(message, state->toolProfile, response, hasResponse, &runtimeError)) {
                if (hasResponse) AugmentToolsList(response, runtimes);
                return true;
            }
            if (firstError.empty()) firstError = runtimeError;
        }

        // Keep host-control tools usable even when an attached payload is
        // temporarily unreachable. A later cortex_detach/cortex_attach can
        // recover the connection without restarting the MCP client.
        HandleLocalProtocol(state, message, response, hasResponse);
        if (hasResponse && response.is_object() && response.contains("result") && response["result"].is_object()) {
            response["result"]["_cortex"] = {
                {"runtime_catalog_available", false},
                {"runtime_catalog_error", firstError.empty() ? "target_runtime_unreachable" : firstError}
            };
        }
        return true;
    }

    if (method == "tools/call") {
        const json params = message.value("params", json::object());
        const std::string name = params.is_object() ? params.value("name", std::string()) : std::string();
        const json arguments = params.is_object() ? params.value("arguments", json::object()) : json::object();

        if (arguments.is_object() && HandleLocalTool(state, message, name, arguments, response)) {
            hasResponse = true;
            return true;
        }
        if (!arguments.is_object()) {
            response = TransportError(MessageId(message), "invalid_arguments", "tools/call arguments must be an object");
            hasResponse = true;
            return true;
        }

        const RuntimeList current = RuntimeSnapshot(state);
        std::string targetError;
        std::string targetMessage;
        auto runtime = ResolveRuntime(current, arguments, targetError, targetMessage);
        if (!runtime) {
            if (targetError == "no_attached_targets")
                targetMessage += ". Use cortex_processes and cortex_attach on this same MCP connection.";
            response = TransportError(MessageId(message), targetError, targetMessage);
            hasResponse = true;
            return true;
        }

        json routed = message;
        if (routed.contains("params") && routed["params"].is_object() &&
            routed["params"].contains("arguments") && routed["params"]["arguments"].is_object()) {
            routed["params"]["arguments"].erase("_cortex_target");
        }
        if (!runtime->payload->ForwardMcp(routed, state->toolProfile, response, hasResponse, error)) return false;
        if (hasResponse && response.is_object() && response.contains("result") && response["result"].is_object() &&
            response["result"].contains("structuredContent") && response["result"]["structuredContent"].is_object()) {
            response["result"]["structuredContent"]["_cortex_target"] = runtime->target.id;
        }
        return true;
    }

    if (runtimes.empty()) return HandleLocalProtocol(state, message, response, hasResponse);
    return runtimes.front()->payload->ForwardMcp(message, state->toolProfile, response, hasResponse, error);
}

bool ForwardOneObserved(const std::shared_ptr<RunState>& state,
                        const json& message,
                        json& response,
                        bool& hasResponse,
                        std::string* error) {
    if (!message.is_object() || message.value("method", std::string()) != "tools/call")
        return ForwardOne(state, message, response, hasResponse, error);

    const json params = message.value("params", json::object());
    const std::string name = params.is_object() ? params.value("name", std::string()) : std::string();
    if (name.empty()) return ForwardOne(state, message, response, hasResponse, error);

    const json arguments = params.is_object() ? params.value("arguments", json::object()) : json::object();
    const std::string target = ActivityTargetText(state, arguments);
    const std::string startDetails = CompactActivityDetails(arguments);
    const json requestId = MessageId(message);
    PublishToolActivity(state, requestId, name, target, "started", ToolStartSummary(name), startDetails);

    const auto startedAt = std::chrono::steady_clock::now();
    std::string localError;
    std::string* forwardedError = error ? error : &localError;
    const bool ok = ForwardOne(state, message, response, hasResponse, forwardedError);
    const auto duration = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startedAt).count());
    const std::string transportError = error ? *error : localError;
    const bool failed = !ok || (hasResponse && ActivityResponseFailed(response));
    const json detailPayload = hasResponse ? ActivityResponsePayload(response) : json::object();
    const std::string details = hasResponse ? CompactActivityDetails(detailPayload)
                                            : (transportError.empty() ? std::string() : transportError);
    PublishToolActivity(state, requestId, name, target, failed ? "failed" : "completed",
                        ToolCompletionSummary(name, response, failed, transportError), details, duration);
    return ok;
}

bool RouteMessage(const std::shared_ptr<RunState>& state,
                  const json& message,
                  json& response,
                  bool& hasResponse,
                  std::string* error) {
    if (!message.is_array() || message.empty())
        return ForwardOneObserved(state, message, response, hasResponse, error);

    json responses = json::array();
    for (const auto& item : message) {
        json itemResponse;
        bool itemHasResponse = false;
        std::string itemError;
        if (!ForwardOneObserved(state, item, itemResponse, itemHasResponse, &itemError)) {
            responses.push_back(TransportError(MessageId(item), "cortex_unreachable",
                                               itemError.empty() ? "Cortex target runtime is unreachable" : itemError));
            continue;
        }
        if (itemHasResponse) responses.push_back(std::move(itemResponse));
    }
    response = std::move(responses);
    hasResponse = !response.empty();
    return true;
}

void ForwardRequest(const json& message, const std::shared_ptr<RunState>& state) {
    json response;
    bool hasResponse = false;
    std::string error;
    if (!RouteMessage(state, message, response, hasResponse, &error)) {
        WriteOutput(state, TransportError(MessageId(message), "cortex_unreachable",
                                          error.empty() ? "Cortex target runtime is unreachable" : error));
        return;
    }
    if (hasResponse) WriteOutput(state, response);
}
} // namespace

int RunMcpMode(int argc, char** argv, const std::string& runtimeDirectory) {
    Options options;
    std::string error;
    if (!ParseOptions(argc, argv, options, error)) {
        std::cerr << "cortex mcp: " << error << '\n';
        PrintUsage(std::cerr);
        return 2;
    }
    if (options.help) {
        PrintUsage(std::cout);
        return 0;
    }

    cortex::target::Catalog catalog;
    if (!catalog.AddBackend(std::make_shared<cortex::target::LocalBackend>())) {
        std::cerr << "cortex mcp: local target backend unavailable\n";
        return 3;
    }

    auto state = std::make_shared<RunState>();
    state->catalog = &catalog;
    state->runtimeDirectory = runtimeDirectory;
    state->toolProfile = options.toolProfile;
    state->activitySessionId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();

    const auto availableTargets = catalog.Targets();
    for (const auto& selector : options.targets) {
        const auto target = ResolveUniqueTarget(selector, availableTargets, error);
        if (!target) {
            std::cerr << "cortex mcp: " << error << '\n';
            return 3;
        }
        const auto current = RuntimeSnapshot(state);
        const bool duplicate = std::any_of(current.begin(), current.end(), [&](const TargetRuntimePtr& runtime) {
            return runtime && runtime->target.id == target->id;
        });
        if (duplicate) {
            std::cerr << "cortex mcp: target requested more than once: PID " << target->processId << '\n';
            return 3;
        }

        std::string errorCode;
        std::string errorMessage;
        auto runtime = CreateRuntime(catalog, *target, runtimeDirectory, errorCode, errorMessage);
        if (!runtime) {
            std::cerr << "cortex mcp: target setup failed for PID " << target->processId << ": "
                      << (errorMessage.empty() ? errorCode : errorMessage) << '\n';
            return errorCode == "target_attach_failed" ? 4 : 5;
        }
        std::lock_guard<std::mutex> lock(state->runtimeMutex);
        state->runtimes.push_back(std::move(runtime));
    }

    PublishSessionActivity(state, "started", "AI/MCP session started",
                           CompactActivityDetails(json{{"tool_profile", options.toolProfile},
                                                       {"startup_targets", RuntimeSnapshot(state).size()}}));

    // stdout is MCP protocol data only from this point onward.
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        if (line.size() > kMaxStdioMessageBytes) {
            WriteOutput(state, TransportError(nullptr, "message_too_large",
                                              "MCP stdio message exceeds the 4 MiB limit"));
            continue;
        }

        json message;
        try {
            message = json::parse(line);
        } catch (const std::exception& exception) {
            WriteOutput(state, {
                {"jsonrpc", "2.0"},
                {"id", nullptr},
                {"error", {{"code", -32700}, {"message", exception.what()}}}
            });
            continue;
        }

        // Notifications, especially notifications/cancelled, bypass the worker
        // limit. In multi-target mode they are broadcast so cancellation and
        // lifecycle notifications reach whichever target owns the request.
        if (IsNotification(message)) {
            json response;
            bool hasResponse = false;
            std::string notificationError;
            if (!RouteMessage(state, message, response, hasResponse, &notificationError)) {
                std::cerr << "cortex mcp: notification forwarding failed: " << notificationError << '\n';
            } else if (hasResponse) {
                // Malformed no-id messages may legitimately be rejected by the
                // runtime as invalid JSON-RPC requests; preserve one response.
                WriteOutput(state, response);
            }
            continue;
        }

        if (!ReserveWorker(state)) {
            WriteOutput(state, TransportError(MessageId(message), "too_many_requests",
                                              "Cortex MCP concurrency limit reached"));
            continue;
        }

        try {
            std::thread([message, state] {
                ForwardRequest(message, state);
                ReleaseWorker(state);
            }).detach();
        } catch (const std::exception& exception) {
            ReleaseWorker(state);
            WriteOutput(state, TransportError(MessageId(message), "worker_start_failed", exception.what()));
        }
    }

    WaitForWorkers(state);
    PublishSessionActivity(state, "ended", "AI/MCP session ended");
    return 0;
}
