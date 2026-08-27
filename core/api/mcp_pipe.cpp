#include "mcp_pipe.h"

#include "mcp_pipe_protocol.h"
#include "mcp_protocol.h"
#include "mcp_tools.h"

#include <windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace api::mcp_pipe {
namespace {

using json = nlohmann::json;

struct Worker {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    std::thread thread;
    std::atomic<bool> done{false};
};

std::atomic<bool> g_running{false};
std::thread g_acceptThread;
std::mutex g_stateMutex;
std::mutex g_workersMutex;
std::vector<std::shared_ptr<Worker>> g_workers;
std::string g_token;
std::string g_pipeName;
std::string g_lastError;

void SetLastError(std::string error) {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    g_lastError = std::move(error);
}

bool ConstantTimeEquals(const std::string& supplied, const std::string& expected) {
    size_t diff = supplied.size() ^ expected.size();
    const size_t length = (std::max)(supplied.size(), expected.size());
    for (size_t i = 0; i < length; ++i) {
        const unsigned char a = i < supplied.size() ? static_cast<unsigned char>(supplied[i]) : 0;
        const unsigned char b = i < expected.size() ? static_cast<unsigned char>(expected[i]) : 0;
        diff |= a ^ b;
    }
    return diff == 0;
}

HANDLE CreatePipeInstance(const std::string& pipeName) {
    DWORD mode = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT;
#ifdef PIPE_REJECT_REMOTE_CLIENTS
    mode |= PIPE_REJECT_REMOTE_CLIENTS;
#endif
    return CreateNamedPipeA(
        pipeName.c_str(),
        PIPE_ACCESS_DUPLEX,
        mode,
        PIPE_UNLIMITED_INSTANCES,
        64 * 1024,
        64 * 1024,
        0,
        nullptr);
}

bool ReadExact(HANDLE pipe, void* destination, std::uint32_t size) {
    auto* cursor = static_cast<unsigned char*>(destination);
    std::uint32_t remaining = size;
    while (remaining > 0) {
        DWORD read = 0;
        const DWORD chunk = remaining;
        if (!ReadFile(pipe, cursor, chunk, &read, nullptr) || read == 0) return false;
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
        const DWORD chunk = remaining;
        if (!WriteFile(pipe, cursor, chunk, &written, nullptr) || written == 0) return false;
        cursor += written;
        remaining -= written;
    }
    return true;
}

bool ReadFrame(HANDLE pipe, std::string& payload) {
    std::uint32_t size = 0;
    if (!ReadExact(pipe, &size, sizeof(size))) return false;
    if (size > mcp_pipe_protocol::kMaxFrameBytes) return false;
    payload.resize(size);
    return size == 0 || ReadExact(pipe, payload.data(), size);
}

bool WriteFrame(HANDLE pipe, const std::string& payload) {
    if (payload.size() > mcp_pipe_protocol::kMaxFrameBytes) return false;
    const auto size = static_cast<std::uint32_t>(payload.size());
    if (!WriteExact(pipe, &size, sizeof(size))) return false;
    return size == 0 || WriteExact(pipe, payload.data(), size);
}

json RpcErrorForEnvelope(const json& envelope, int code, const std::string& message) {
    json id = nullptr;
    if (envelope.is_object() && envelope.contains("message") && envelope["message"].is_object() &&
        envelope["message"].contains("id")) {
        id = envelope["message"]["id"];
    }
    return mcp_protocol::Error(id, code, message);
}

void CloseWorkerPipe(const std::shared_ptr<Worker>& worker) {
    std::lock_guard<std::mutex> lock(g_workersMutex);
    if (worker->pipe != INVALID_HANDLE_VALUE) {
        DisconnectNamedPipe(worker->pipe);
        CloseHandle(worker->pipe);
        worker->pipe = INVALID_HANDLE_VALUE;
    }
    worker->done.store(true, std::memory_order_release);
}

void ServeClient(const std::shared_ptr<Worker>& worker, const std::string& expectedToken) {
    std::string frame;
    if (!ReadFrame(worker->pipe, frame) || frame.empty()) {
        CloseWorkerPipe(worker);
        return;
    }

    json envelope;
    std::string responsePayload;
    try {
        envelope = json::parse(frame);
        if (!envelope.is_object()) {
            responsePayload = RpcErrorForEnvelope(envelope, -32600, "invalid_native_envelope").dump();
        } else if (!envelope.contains("token") || !envelope["token"].is_string() ||
                   !ConstantTimeEquals(envelope["token"].get<std::string>(), expectedToken)) {
            responsePayload = RpcErrorForEnvelope(envelope, -32001, "invalid_token").dump();
        } else if (!envelope.contains("message")) {
            responsePayload = RpcErrorForEnvelope(envelope, -32600, "missing_message").dump();
        } else {
            const auto profile = mcp_tools::ParseProfile(
                envelope.value("tools", std::string("compact")),
                mcp_protocol::ToolProfile::Compact);
            const auto result = mcp_tools::Handle(
                envelope["message"],
                profile,
                envelope.value("protocolVersion", std::string()),
                envelope.value("session", std::string()));
            if (result.hasResponse) responsePayload = result.response.dump();
        }
    } catch (const json::parse_error& error) {
        responsePayload = mcp_protocol::Error(nullptr, -32700, error.what()).dump();
    } catch (const std::exception& error) {
        responsePayload = mcp_protocol::Error(nullptr, -32603, error.what()).dump();
    }

    WriteFrame(worker->pipe, responsePayload);
    CloseWorkerPipe(worker);
}

void ReapFinishedWorkers() {
    std::vector<std::shared_ptr<Worker>> finished;
    {
        std::lock_guard<std::mutex> lock(g_workersMutex);
        auto it = g_workers.begin();
        while (it != g_workers.end()) {
            if ((*it)->done.load(std::memory_order_acquire)) {
                finished.push_back(*it);
                it = g_workers.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& worker : finished)
        if (worker->thread.joinable()) worker->thread.join();
}

void AcceptLoop(HANDLE firstPipe, const std::string& pipeName, const std::string& expectedToken) {
    HANDLE pipe = firstPipe;
    while (g_running.load(std::memory_order_acquire)) {
        const BOOL connected = ConnectNamedPipe(pipe, nullptr)
            ? TRUE
            : (GetLastError() == ERROR_PIPE_CONNECTED);

        if (!g_running.load(std::memory_order_acquire)) {
            if (connected) DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            break;
        }

        if (connected) {
            auto worker = std::make_shared<Worker>();
            worker->pipe = pipe;
            {
                std::lock_guard<std::mutex> lock(g_workersMutex);
                g_workers.push_back(worker);
            }
            worker->thread = std::thread([worker, expectedToken] {
                ServeClient(worker, expectedToken);
            });
            ReapFinishedWorkers();
        } else {
            CloseHandle(pipe);
        }

        pipe = CreatePipeInstance(pipeName);
        if (pipe == INVALID_HANDLE_VALUE) {
            SetLastError("create_named_pipe_failed:" + std::to_string(GetLastError()));
            g_running.store(false, std::memory_order_release);
            break;
        }
    }
    ReapFinishedWorkers();
}

} // namespace

bool Start(const std::string& token) {
    if (g_running.load(std::memory_order_acquire)) return true;
    if (token.size() < 32) {
        SetLastError("invalid_token");
        return false;
    }

    const std::string pipeName = mcp_pipe_protocol::PipeNameForToken(token);
    HANDLE firstPipe = CreatePipeInstance(pipeName);
    if (firstPipe == INVALID_HANDLE_VALUE) {
        SetLastError("create_named_pipe_failed:" + std::to_string(GetLastError()));
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_token = token;
        g_pipeName = pipeName;
        g_lastError.clear();
    }
    g_running.store(true, std::memory_order_release);
    g_acceptThread = std::thread([firstPipe, pipeName, token] {
        AcceptLoop(firstPipe, pipeName, token);
    });
    return true;
}

void Stop() {
    if (!g_running.exchange(false, std::memory_order_acq_rel) && !g_acceptThread.joinable()) return;

    std::string pipeName;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        pipeName = g_pipeName;
    }

    // Wake a blocking ConnectNamedPipe call.
    if (!pipeName.empty()) {
        HANDLE wake = CreateFileA(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE,
                                  0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (wake != INVALID_HANDLE_VALUE) CloseHandle(wake);
    }

    if (g_acceptThread.joinable()) g_acceptThread.join();

    std::vector<std::shared_ptr<Worker>> workers;
    {
        std::lock_guard<std::mutex> lock(g_workersMutex);
        workers = g_workers;
        for (const auto& worker : workers) {
            if (worker->pipe != INVALID_HANDLE_VALUE) {
                CancelIoEx(worker->pipe, nullptr);
                DisconnectNamedPipe(worker->pipe);
            }
        }
        g_workers.clear();
    }
    for (auto& worker : workers)
        if (worker->thread.joinable()) worker->thread.join();

    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_token.clear();
        g_pipeName.clear();
    }
}

bool IsRunning() {
    return g_running.load(std::memory_order_acquire);
}

std::string GetLastError() {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    return g_lastError;
}

std::string GetPipeName() {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    return g_pipeName;
}

} // namespace api::mcp_pipe
