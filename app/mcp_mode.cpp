#include "mcp_mode.h"

#include "services/payload_client.h"
#include "target/catalog.h"
#include "target/local_backend.h"
#include "target/session_manager.h"

#include <nlohmann/json.hpp>

#include <algorithm>
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

struct RunState {
    std::mutex outputMutex;
    std::mutex activeMutex;
    std::condition_variable activeChanged;
    std::size_t active = 0;
};

struct TargetRuntime {
    TargetDescriptor target;
    std::unique_ptr<cortex::target::SessionManager> sessions;
    std::unique_ptr<cortex::services::PayloadClient> payload;
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
           << "  cortex.exe mcp --pid <pid> [--pid <pid> ...] [--tools compact|all]\n"
           << "  cortex.exe mcp --process <name> [--process <name> ...] [--tools compact|all]\n"
           << "  cortex.exe mcp --pid <pid> --process <name> [--tools compact|all]\n\n"
           << "One or more targets may be attached by the same MCP server. With multiple\n"
           << "targets, tools/call requests select a target through the _cortex_target\n"
           << "argument exposed in tools/list. Use cortex_targets to list attached targets.\n";
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

    if (!options.help && options.targets.empty()) {
        error = "at least one target is required (--pid or --process)";
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

std::string TargetSummaryText(const std::vector<std::unique_ptr<TargetRuntime>>& runtimes) {
    std::string result;
    for (std::size_t index = 0; index < runtimes.size(); ++index) {
        if (index != 0) result += ", ";
        result += runtimes[index]->target.name + " (PID " + std::to_string(runtimes[index]->target.processId) + ")";
    }
    return result;
}

json TargetListResult(const std::vector<std::unique_ptr<TargetRuntime>>& runtimes) {
    json targets = json::array();
    for (const auto& runtime : runtimes) {
        targets.push_back({
            {"id", runtime->target.id},
            {"name", runtime->target.name},
            {"pid", runtime->target.processId},
            {"selector", std::to_string(runtime->target.processId)},
            {"alive", runtime->sessions && runtime->sessions->Active() && runtime->sessions->Active()->Alive()}
        });
    }
    return {{"ok", true}, {"count", targets.size()}, {"targets", std::move(targets)}};
}

TargetRuntime* ResolveRuntime(std::vector<std::unique_ptr<TargetRuntime>>& runtimes,
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
        if (runtimes.size() == 1) return runtimes.front().get();
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
        for (auto& runtime : runtimes) {
            if (runtime->target.processId == pid) return runtime.get();
        }
    } else if (selector.is_number_integer()) {
        const std::int64_t signedPid = selector.get<std::int64_t>();
        if (signedPid <= 0) {
            errorCode = "invalid_cortex_target";
            errorMessage = "_cortex_target PID must be positive";
            return nullptr;
        }
        const auto pid = static_cast<std::uint64_t>(signedPid);
        for (auto& runtime : runtimes) {
            if (runtime->target.processId == pid) return runtime.get();
        }
    } else if (selector.is_string()) {
        const std::string wanted = selector.get<std::string>();
        for (auto& runtime : runtimes) {
            if (runtime->target.id == wanted) return runtime.get();
        }

        try {
            std::size_t consumed = 0;
            const auto pid = std::stoull(wanted, &consumed, 10);
            if (consumed == wanted.size()) {
                for (auto& runtime : runtimes) {
                    if (runtime->target.processId == pid) return runtime.get();
                }
            }
        } catch (...) {
        }

        const std::string lowered = LowerAscii(wanted);
        TargetRuntime* exact = nullptr;
        for (auto& runtime : runtimes) {
            if (LowerAscii(runtime->target.name) != lowered) continue;
            if (exact) {
                errorCode = "cortex_target_ambiguous";
                errorMessage = "Process name matches more than one attached target; use PID or target id";
                return nullptr;
            }
            exact = runtime.get();
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

void AugmentToolsList(json& response, const std::vector<std::unique_ptr<TargetRuntime>>& runtimes) {
    if (!response.is_object() || !response.contains("result") || !response["result"].is_object()) return;
    auto& result = response["result"];
    if (!result.contains("tools") || !result["tools"].is_array()) return;

    const bool requireTarget = runtimes.size() > 1;
    const std::string description =
        "Select the attached Cortex target for this call by PID, target id, or unique process name. Available: " +
        TargetSummaryText(runtimes);

    json augmented = json::array();
    augmented.push_back({
        {"name", "cortex_targets"},
        {"description", "List the processes attached to this Cortex MCP server and the selectors accepted by _cortex_target."},
        {"inputSchema", {{"type", "object"}, {"properties", json::object()}, {"additionalProperties", false}}},
        {"_cortex", {{"multi_target_router", true}}}
    });

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

bool ForwardOne(std::vector<std::unique_ptr<TargetRuntime>>& runtimes,
                const std::string& toolProfile,
                const json& message,
                json& response,
                bool& hasResponse,
                std::string* error) {
    response = json();
    hasResponse = false;
    if (error) error->clear();
    if (runtimes.empty()) {
        if (error) *error = "no_attached_targets";
        return false;
    }

    if (!message.is_object())
        return runtimes.front()->payload->ForwardMcp(message, toolProfile, response, hasResponse, error);

    if (IsNotification(message)) {
        std::string firstError;
        for (auto& runtime : runtimes) {
            json notificationResponse;
            bool notificationHasResponse = false;
            std::string notificationError;
            if (!runtime->payload->ForwardMcp(message, toolProfile, notificationResponse,
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
    if (method == "tools/list") {
        if (!runtimes.front()->payload->ForwardMcp(message, toolProfile, response, hasResponse, error)) return false;
        if (hasResponse) AugmentToolsList(response, runtimes);
        return true;
    }

    if (method == "tools/call") {
        const json params = message.value("params", json::object());
        if (params.is_object() && params.value("name", std::string()) == "cortex_targets") {
            response = LocalToolResponse(MessageId(message), TargetListResult(runtimes));
            hasResponse = true;
            return true;
        }
        const json arguments = params.is_object() ? params.value("arguments", json::object()) : json::object();
        if (!arguments.is_object())
            return runtimes.front()->payload->ForwardMcp(message, toolProfile, response, hasResponse, error);

        std::string targetError;
        std::string targetMessage;
        TargetRuntime* runtime = ResolveRuntime(runtimes, arguments, targetError, targetMessage);
        if (!runtime) {
            response = TransportError(MessageId(message), targetError, targetMessage);
            hasResponse = true;
            return true;
        }

        json routed = message;
        if (routed.contains("params") && routed["params"].is_object() &&
            routed["params"].contains("arguments") && routed["params"]["arguments"].is_object()) {
            routed["params"]["arguments"].erase("_cortex_target");
        }
        if (!runtime->payload->ForwardMcp(routed, toolProfile, response, hasResponse, error)) return false;
        if (hasResponse && response.is_object() && response.contains("result") && response["result"].is_object() &&
            response["result"].contains("structuredContent") && response["result"]["structuredContent"].is_object()) {
            response["result"]["structuredContent"]["_cortex_target"] = runtime->target.id;
        }
        return true;
    }

    if (!runtimes.front()->payload->ForwardMcp(message, toolProfile, response, hasResponse, error)) return false;
    if (hasResponse && method == "initialize" && runtimes.size() > 1 && response.is_object() &&
        response.contains("result") && response["result"].is_object()) {
        std::string instructions = response["result"].value("instructions", std::string());
        if (!instructions.empty()) instructions += " ";
        instructions += "Multiple Cortex targets are attached. Use cortex_targets and pass _cortex_target on every tools/call request.";
        response["result"]["instructions"] = std::move(instructions);
    }
    return true;
}

bool RouteMessage(std::vector<std::unique_ptr<TargetRuntime>>& runtimes,
                  const std::string& toolProfile,
                  const json& message,
                  json& response,
                  bool& hasResponse,
                  std::string* error) {
    if (!message.is_array() || message.empty())
        return ForwardOne(runtimes, toolProfile, message, response, hasResponse, error);

    json responses = json::array();
    for (const auto& item : message) {
        json itemResponse;
        bool itemHasResponse = false;
        std::string itemError;
        if (!ForwardOne(runtimes, toolProfile, item, itemResponse, itemHasResponse, &itemError)) {
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

void ForwardRequest(std::vector<std::unique_ptr<TargetRuntime>>& runtimes,
                    const std::string& toolProfile,
                    const json& message,
                    const std::shared_ptr<RunState>& state) {
    json response;
    bool hasResponse = false;
    std::string error;
    if (!RouteMessage(runtimes, toolProfile, message, response, hasResponse, &error)) {
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

    const auto availableTargets = catalog.Targets();
    std::vector<std::unique_ptr<TargetRuntime>> runtimes;
    runtimes.reserve(options.targets.size());
    for (const auto& selector : options.targets) {
        const auto target = ResolveUniqueTarget(selector, availableTargets, error);
        if (!target) {
            std::cerr << "cortex mcp: " << error << '\n';
            return 3;
        }
        const bool duplicate = std::any_of(runtimes.begin(), runtimes.end(), [&](const auto& runtime) {
            return runtime->target.id == target->id;
        });
        if (duplicate) {
            std::cerr << "cortex mcp: target requested more than once: PID " << target->processId << '\n';
            return 3;
        }

        auto runtime = std::make_unique<TargetRuntime>();
        runtime->target = *target;
        runtime->sessions = std::make_unique<cortex::target::SessionManager>(catalog);
        if (!runtime->sessions->Attach(*target, &error)) {
            std::cerr << "cortex mcp: target attach failed for PID " << target->processId << ": "
                      << (error.empty() ? "attach_failed" : error) << '\n';
            return 4;
        }
        runtime->payload = std::make_unique<cortex::services::PayloadClient>(*runtime->sessions, runtimeDirectory);
        if (!runtime->payload->EnsureReady(&error)) {
            std::cerr << "cortex mcp: target runtime unavailable for PID " << target->processId << ": "
                      << (error.empty() ? "payload_unavailable" : error) << '\n';
            return 5;
        }
        runtimes.push_back(std::move(runtime));
    }
    // stdout is MCP protocol data only from this point onward.
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);
    auto state = std::make_shared<RunState>();

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
            if (!RouteMessage(runtimes, options.toolProfile, message, response, hasResponse, &notificationError)) {
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
            std::thread([&runtimes, toolProfile = options.toolProfile, message, state] {
                ForwardRequest(runtimes, toolProfile, message, state);
                ReleaseWorker(state);
            }).detach();
        } catch (const std::exception& exception) {
            ReleaseWorker(state);
            WriteOutput(state, TransportError(MessageId(message), "worker_start_failed", exception.what()));
        }
    }

    WaitForWorkers(state);
    return 0;
}
