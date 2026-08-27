// cortex_mcp_bridge -- stdio transport adapter for Cortex MCP clients.
//
// Native mode is the default: stdio JSON-RPC is framed onto an authenticated
// Windows named pipe exposed by the injected Cortex runtime. HTTP remains an
// explicit compatibility/debug transport via --transport http.
//
// Usage in claude_desktop_config.json:
//   { "mcpServers": { "cortex": {
//       "command": "cortex_host.exe",
//       "args": ["mcp", "--process", "app.exe"]
//   } } }
// Optional args: --token <hex>, --token-file <file>,
//                --tools compact|all (default compact),
//                --transport native|http (default native),
//                --process <name> | --pid <pid>, --dll <cortex_core.dll>.
// HTTP-only args: --port 6969, --host 127.0.0.1.

#include "api/mcp_pipe_protocol.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <windows.h>
#include <io.h>
#include <cstdio>
#include <cstdint>
#include <condition_variable>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

int CortexInjectMain(int argc, char** argv);

namespace {

using json = nlohmann::json;

std::string ReadTokenFile(const std::string& path) {
    std::ifstream file(path);
    std::string token;
    file >> token;
    return token;
}

std::string ExecutableDir() {
    char self[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, self, MAX_PATH);
    std::string dir = self;
    const size_t slash = dir.find_last_of("\\/");
    if (slash != std::string::npos) dir.resize(slash);
    return dir;
}

std::string DirectoryOf(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string MakeSessionId() {
    std::ostringstream out;
    out << std::hex << GetCurrentProcessId() << '-' << GetTickCount64();
    return out.str();
}

int RunInjectorQuietly(const std::string& target, const std::string& dllPath) {
    std::vector<std::string> storage = {"cortex_host inject", target};
    if (!dllPath.empty()) storage.push_back(dllPath);
    std::vector<char*> args;
    for (auto& item : storage) args.push_back(item.data());
    args.push_back(nullptr);

    // stdout is reserved for MCP protocol data during this invocation.
    std::fflush(stdout);
    const int savedStdout = _dup(_fileno(stdout));
    if (savedStdout >= 0) _dup2(_fileno(stderr), _fileno(stdout));
    const int result = CortexInjectMain(static_cast<int>(storage.size()), args.data());
    std::fflush(stdout);
    if (savedStdout >= 0) {
        _dup2(savedStdout, _fileno(stdout));
        _close(savedStdout);
    }
    return result;
}

std::string WaitForToken(const std::string& path) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        const std::string token = ReadTokenFile(path);
        if (!token.empty()) return token;
        Sleep(50);
    }
    return {};
}

bool ReadExact(HANDLE pipe, void* destination, std::uint32_t size) {
    auto* cursor = static_cast<unsigned char*>(destination);
    std::uint32_t remaining = size;
    while (remaining > 0) {
        DWORD read = 0;
        if (!ReadFile(pipe, cursor, remaining, &read, nullptr) || read == 0) return false;
        cursor += read;
        remaining -= read;
    }
    return true;
}

bool WriteExact(HANDLE pipe, const void* source, std::uint32_t size) {
    const auto* cursor = static_cast<const unsigned char*>(source);
    std::uint32_t remaining = size;
    while (remaining > 0) {
        DWORD written = 0;
        if (!WriteFile(pipe, cursor, remaining, &written, nullptr) || written == 0) return false;
        cursor += written;
        remaining -= written;
    }
    return true;
}

bool ReadFrame(HANDLE pipe, std::string& payload) {
    std::uint32_t size = 0;
    if (!ReadExact(pipe, &size, sizeof(size))) return false;
    if (size > api::mcp_pipe_protocol::kMaxFrameBytes) return false;
    payload.resize(size);
    return size == 0 || ReadExact(pipe, payload.data(), size);
}

bool WriteFrame(HANDLE pipe, const std::string& payload) {
    if (payload.size() > api::mcp_pipe_protocol::kMaxFrameBytes) return false;
    const auto size = static_cast<std::uint32_t>(payload.size());
    if (!WriteExact(pipe, &size, sizeof(size))) return false;
    return size == 0 || WriteExact(pipe, payload.data(), size);
}

HANDLE OpenPipe(const std::string& pipeName, int attempts = 100) {
    for (int attempt = 0; attempt < attempts; ++attempt) {
        HANDLE pipe = CreateFileA(pipeName.c_str(),
                                  GENERIC_READ | GENERIC_WRITE,
                                  0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) return pipe;

        if (GetLastError() == ERROR_PIPE_BUSY)
            WaitNamedPipeA(pipeName.c_str(), 50);
        else
            Sleep(50);
    }
    return INVALID_HANDLE_VALUE;
}

bool NativeRoundTrip(const std::string& pipeName,
                     const std::string& token,
                     const std::string& toolProfile,
                     const std::string& session,
                     const json& message,
                     std::string& response) {
    HANDLE pipe = OpenPipe(pipeName, 20);
    if (pipe == INVALID_HANDLE_VALUE) return false;

    const json envelope = {
        {"token", token},
        {"tools", toolProfile},
        {"session", session},
        {"message", message}
    };
    const std::string payload = envelope.dump();
    const bool ok = WriteFrame(pipe, payload) && ReadFrame(pipe, response);
    CloseHandle(pipe);
    return ok;
}

bool WaitForHttpRuntime(const std::string& host, int port) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        httplib::Client client(host.c_str(), port);
        client.set_connection_timeout(0, 100000);
        client.set_read_timeout(0, 250000);
        const auto response = client.Get("/health");
        if (response && response->status >= 200 && response->status < 500) return true;
        Sleep(50);
    }
    return false;
}

bool HttpRoundTrip(const std::string& host,
                   int port,
                   const std::string& token,
                   const std::string& toolProfile,
                   const std::string& session,
                   const json& message,
                   std::string& response) {
    httplib::Client client(host.c_str(), port);
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(60, 0);
    const httplib::Headers headers = {
        {"X-Cortex-Token", token},
        {"Host", host},
        {"X-Cortex-MCP-Tools", toolProfile},
        {"X-Cortex-MCP-Session", session}
    };
    const auto result = client.Post("/mcp", headers, message.dump(), "application/json");
    if (!result) return false;
    if (result->status == 202 || result->body.empty()) response.clear();
    else response = result->body;
    return true;
}

json BridgeError(const json& id, const std::string& code, const std::string& message) {
    return {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"error", {{"code", -32000}, {"message", message}, {"data", {{"code", code}}}}}
    };
}

json MessageId(const json& message) {
    return message.is_object() && message.contains("id") ? message.at("id") : json(nullptr);
}

bool IsCancellationNotification(const json& message) {
    return message.is_object() && !message.contains("id") &&
           message.value("method", std::string()) == "notifications/cancelled";
}

struct RunState {
    std::mutex outputMutex;
    std::mutex activeMutex;
    std::condition_variable activeChanged;
    size_t active = 0;
};

void WriteOutput(const std::shared_ptr<RunState>& state, const std::string& response) {
    if (response.empty()) return;
    std::lock_guard<std::mutex> lock(state->outputMutex);
    std::cout << response << '\n';
}

bool ReserveWorker(const std::shared_ptr<RunState>& state, size_t limit) {
    std::lock_guard<std::mutex> lock(state->activeMutex);
    if (state->active >= limit) return false;
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

int RunNative(const std::string& token, const std::string& toolProfile) {
    const std::string pipeName = api::mcp_pipe_protocol::PipeNameForToken(token);
    const std::string session = MakeSessionId();
    HANDLE ready = OpenPipe(pipeName, 100);
    if (ready == INVALID_HANDLE_VALUE) {
        std::cerr << "cortex_host mcp: native Cortex pipe did not become reachable\n";
        return 3;
    }
    CloseHandle(ready);

    std::setvbuf(stdout, nullptr, _IONBF, 0);
    auto state = std::make_shared<RunState>();
    constexpr size_t kMaxConcurrentRequests = 64;

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;

        json message;
        try {
            message = json::parse(line);
        } catch (const std::exception& error) {
            WriteOutput(state, json({
                {"jsonrpc", "2.0"},
                {"id", nullptr},
                {"error", {{"code", -32700}, {"message", error.what()}}}
            }).dump());
            continue;
        }

        // Cancellation bypasses the worker limit and is dispatched on the
        // reader thread, so it can reach the runtime while another pipe is
        // executing a long primitive.
        if (IsCancellationNotification(message)) {
            std::string ignored;
            NativeRoundTrip(pipeName, token, toolProfile, session, message, ignored);
            continue;
        }

        if (!ReserveWorker(state, kMaxConcurrentRequests)) {
            WriteOutput(state, BridgeError(MessageId(message), "too_many_requests",
                                           "Cortex MCP bridge concurrency limit reached").dump());
            continue;
        }

        try {
            std::thread([state, pipeName, token, toolProfile, session, message] {
                std::string response;
                if (!NativeRoundTrip(pipeName, token, toolProfile, session, message, response)) {
                    response = BridgeError(MessageId(message), "cortex_unreachable",
                                           "Cortex native MCP transport is unreachable").dump();
                }
                WriteOutput(state, response);
                ReleaseWorker(state);
            }).detach();
        } catch (const std::exception& error) {
            ReleaseWorker(state);
            WriteOutput(state, BridgeError(MessageId(message), "worker_start_failed", error.what()).dump());
        }
    }

    WaitForWorkers(state);
    return 0;
}

int RunHttp(const std::string& host,
            int port,
            const std::string& token,
            const std::string& toolProfile) {
    if (!WaitForHttpRuntime(host, port)) {
        std::cerr << "cortex_host mcp: Cortex runtime did not become reachable on "
                  << host << ':' << port << '\n';
        return 3;
    }

    const std::string session = MakeSessionId();
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    auto state = std::make_shared<RunState>();
    constexpr size_t kMaxConcurrentRequests = 64;

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;

        json message;
        try {
            message = json::parse(line);
        } catch (const std::exception& error) {
            WriteOutput(state, json({
                {"jsonrpc", "2.0"},
                {"id", nullptr},
                {"error", {{"code", -32700}, {"message", error.what()}}}
            }).dump());
            continue;
        }

        // Use an independent connection for cancellation so the fallback
        // transport preserves the same responsive cancellation semantics as
        // native mode instead of blocking behind an active POST /mcp.
        if (IsCancellationNotification(message)) {
            std::string ignored;
            HttpRoundTrip(host, port, token, toolProfile, session, message, ignored);
            continue;
        }

        if (!ReserveWorker(state, kMaxConcurrentRequests)) {
            WriteOutput(state, BridgeError(MessageId(message), "too_many_requests",
                                           "Cortex MCP bridge concurrency limit reached").dump());
            continue;
        }

        try {
            std::thread([state, host, port, token, toolProfile, session, message] {
                std::string response;
                if (!HttpRoundTrip(host, port, token, toolProfile, session, message, response)) {
                    response = BridgeError(MessageId(message), "cortex_unreachable",
                                           "Cortex HTTP MCP transport is unreachable").dump();
                }
                WriteOutput(state, response);
                ReleaseWorker(state);
            }).detach();
        } catch (const std::exception& error) {
            ReleaseWorker(state);
            WriteOutput(state, BridgeError(MessageId(message), "worker_start_failed", error.what()).dump());
        }
    }

    WaitForWorkers(state);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    std::string token;
    std::string tokenFile;
    std::string toolProfile = "compact";
    std::string transport = "native";
    std::string processTarget;
    std::string dllPath;
    int port = 6969;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--port" && i + 1 < argc) port = std::atoi(argv[++i]);
        else if (argument == "--host" && i + 1 < argc) host = argv[++i];
        else if (argument == "--token" && i + 1 < argc) token = argv[++i];
        else if (argument == "--token-file" && i + 1 < argc) tokenFile = argv[++i];
        else if (argument == "--tools" && i + 1 < argc) toolProfile = argv[++i];
        else if (argument == "--transport" && i + 1 < argc) transport = argv[++i];
        else if ((argument == "--process" || argument == "--pid") && i + 1 < argc)
            processTarget = argv[++i];
        else if (argument == "--dll" && i + 1 < argc) dllPath = argv[++i];
        else {
            std::cerr << "cortex_host mcp: unknown or incomplete argument: " << argument << '\n';
            return 2;
        }
    }

    if (toolProfile != "compact" && toolProfile != "all") {
        std::cerr << "cortex_host mcp: --tools must be compact or all\n";
        return 2;
    }
    if (transport != "native" && transport != "http") {
        std::cerr << "cortex_host mcp: --transport must be native or http\n";
        return 2;
    }

    if (!processTarget.empty()) {
        if (dllPath.empty()) dllPath = ExecutableDir() + "\\cortex_core.dll";
        if (RunInjectorQuietly(processTarget, dllPath) != 0) return 4;
    }

    if (token.empty()) {
        if (tokenFile.empty()) {
            std::string tokenDir = !dllPath.empty() ? DirectoryOf(dllPath) : ExecutableDir();
            if (tokenDir.empty()) tokenDir = ExecutableDir();
            tokenFile = tokenDir + "\\cortex.token";
        }
        token = processTarget.empty() ? ReadTokenFile(tokenFile) : WaitForToken(tokenFile);
    }
    if (token.empty()) {
        std::cerr << "cortex_host mcp: no token found. Pass --token or --token-file, "
                     "or place cortex.token beside cortex_core.dll.\n";
        return 1;
    }

    return transport == "http"
        ? RunHttp(host, port, token, toolProfile)
        : RunNative(token, toolProfile);
}
