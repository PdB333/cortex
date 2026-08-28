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
#include <vector>

namespace {

using json = nlohmann::json;
using cortex::target::TargetDescriptor;

constexpr std::size_t kMaxStdioMessageBytes = 4u * 1024u * 1024u;
constexpr std::size_t kMaxConcurrentRequests = 64;

struct Options {
    std::optional<std::uint64_t> pid;
    std::string process;
    std::string toolProfile = "compact";
    bool help = false;
};

struct RunState {
    std::mutex outputMutex;
    std::mutex activeMutex;
    std::condition_variable activeChanged;
    std::size_t active = 0;
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
           << "  cortex.exe mcp --pid <pid> [--tools compact|all]\n"
           << "  cortex.exe mcp --process <name> [--tools compact|all]\n\n"
           << "The same cortex.exe attaches the target, activates its internal payload when\n"
           << "required, and forwards MCP JSON-RPC over the authenticated native transport.\n";
}

bool ParseOptions(int argc, char** argv, Options& options, std::string& error) {
    for (int index = 2; index < argc; ++index) {
        const std::string argument = argv[index] ? argv[index] : "";
        if (argument == "--help" || argument == "-h") {
            options.help = true;
            continue;
        }
        if (argument == "--pid" && index + 1 < argc) {
            if (options.pid || !options.process.empty()) {
                error = "select exactly one target with --pid or --process";
                return false;
            }
            const std::string text = argv[++index] ? argv[index] : "";
            try {
                std::size_t consumed = 0;
                const auto value = std::stoull(text, &consumed, 10);
                if (consumed != text.size() || value == 0) throw std::invalid_argument("pid");
                options.pid = static_cast<std::uint64_t>(value);
            } catch (...) {
                error = "--pid must be a positive integer";
                return false;
            }
            continue;
        }
        if (argument == "--process" && index + 1 < argc) {
            if (options.pid || !options.process.empty()) {
                error = "select exactly one target with --pid or --process";
                return false;
            }
            options.process = argv[++index] ? argv[index] : "";
            if (options.process.empty()) {
                error = "--process requires a non-empty process name";
                return false;
            }
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

    if (!options.help && !options.pid && options.process.empty()) {
        error = "a target is required (--pid or --process)";
        return false;
    }
    return true;
}

std::optional<TargetDescriptor> ResolveUniqueTarget(const Options& options,
                                                     const std::vector<TargetDescriptor>& targets,
                                                     std::string& error) {
    if (options.pid) {
        const auto found = std::find_if(targets.begin(), targets.end(), [&](const TargetDescriptor& target) {
            return target.processId == *options.pid;
        });
        if (found == targets.end()) {
            error = "target pid not found: " + std::to_string(*options.pid);
            return std::nullopt;
        }
        return *found;
    }

    const std::string wanted = LowerAscii(options.process);
    std::vector<TargetDescriptor> exact;
    std::vector<TargetDescriptor> partial;
    for (const auto& target : targets) {
        const std::string name = LowerAscii(target.name);
        if (name == wanted) exact.push_back(target);
        else if (name.find(wanted) != std::string::npos) partial.push_back(target);
    }

    const auto& matches = exact.empty() ? partial : exact;
    if (matches.empty()) {
        error = "process not found: " + options.process;
        return std::nullopt;
    }
    if (matches.size() > 1) {
        error = "process target is ambiguous: " + options.process + " (matching pids";
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

void ForwardRequest(cortex::services::PayloadClient& payload,
                    const std::string& toolProfile,
                    const json& message,
                    const std::shared_ptr<RunState>& state) {
    json response;
    bool hasResponse = false;
    std::string error;
    if (!payload.ForwardMcp(message, toolProfile, response, hasResponse, &error)) {
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

    const auto target = ResolveUniqueTarget(options, catalog.Targets(), error);
    if (!target) {
        std::cerr << "cortex mcp: " << error << '\n';
        return 3;
    }

    cortex::target::SessionManager sessions(catalog);
    if (!sessions.Attach(*target, &error)) {
        std::cerr << "cortex mcp: target attach failed: "
                  << (error.empty() ? "attach_failed" : error) << '\n';
        return 4;
    }

    cortex::services::PayloadClient payload(sessions, runtimeDirectory);
    if (!payload.EnsureReady(&error)) {
        std::cerr << "cortex mcp: target runtime unavailable: "
                  << (error.empty() ? "payload_unavailable" : error) << '\n';
        return 5;
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
        // limit and are forwarded immediately on a separate native pipe. That
        // preserves cancellation responsiveness while another request is busy.
        if (IsNotification(message)) {
            json response;
            bool hasResponse = false;
            std::string notificationError;
            if (!payload.ForwardMcp(message, options.toolProfile, response, hasResponse, &notificationError)) {
                std::cerr << "cortex mcp: notification forwarding failed: " << notificationError << '\n';
            } else if (hasResponse) {
                // Malformed no-id messages may legitimately be rejected by the
                // runtime as invalid JSON-RPC requests; preserve that response.
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
            std::thread([&payload, toolProfile = options.toolProfile, message, state] {
                ForwardRequest(payload, toolProfile, message, state);
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
