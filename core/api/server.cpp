#include "server.h"
#include "routes.h"
#include "request_id.h"
#include "request_limits.h"
#include "response_contract.h"
#include "../config.h"
#include "../log.h"
#include "../events/events.h"

#include <windows.h>
#include <bcrypt.h>
#include <httplib.h>
#include <thread>
#include <memory>
#include <atomic>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <mutex>
#include <cctype>

namespace api {

namespace {
    std::unique_ptr<httplib::Server> g_server;
    std::thread g_thread;
    int g_port = 0;
    ULONGLONG g_startTimeMs = 0;
    std::string g_token;
    std::string g_tokenPath;
    std::string g_lastError;
    std::mutex g_stateMutex;
    std::atomic<uint64_t> g_requestSequence{0};

    void SetLastError(std::string error) {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_lastError = std::move(error);
    }

    std::string GenerateToken() {
        unsigned char bytes[32] = {};
        if (BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return {};
        std::ostringstream out;
        out << std::hex << std::setfill('0');
        for (unsigned char b : bytes) out << std::setw(2) << static_cast<unsigned int>(b);
        return out.str();
    }

    std::string NextRequestId() {
        const uint64_t sequence = g_requestSequence.fetch_add(1, std::memory_order_relaxed) + 1;
        return request_id::Format(GetTickCount64(), GetCurrentProcessId(), sequence);
    }

    std::string LoadOrCreateToken(const std::string& configured) {
        g_tokenPath = config::GetModuleDir() + "\\cortex.token";
        if (!configured.empty()) return configured;
        std::ifstream in(g_tokenPath);
        std::string token;
        if (in >> token && token.size() >= 32) return token;
        token = GenerateToken();
        if (!token.empty()) {
            HANDLE file = CreateFileA(g_tokenPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
            if (file == INVALID_HANDLE_VALUE) return {};
            const std::string payload = token + "\r\n";
            DWORD written = 0;
            const bool ok = WriteFile(file, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr) &&
                            written == payload.size() && FlushFileBuffers(file);
            CloseHandle(file);
            if (!ok) return {};
        }
        return token;
    }

    bool IsLocalAuthority(const std::string& authority) {
        // IPv6 loopback authority: [::1] or [::1]:port. Match the bracketed
        // form explicitly -- the naked "::1" case doesn't appear in HTTP Host
        // headers (RFC 3986 requires the brackets for IPv6).
        if (!authority.empty() && authority.front() == '[') {
            const size_t rb = authority.find(']');
            if (rb == std::string::npos) return false;
            const std::string host = authority.substr(1, rb - 1);
            if (host != "::1") return false;
            if (rb + 1 == authority.size()) return true;
            if (authority[rb + 1] != ':') return false;
            const std::string port = authority.substr(rb + 2);
            return !port.empty() && port.find_first_not_of("0123456789") == std::string::npos;
        }
        const size_t colon = authority.find(':');
        const std::string host = authority.substr(0, colon);
        if (host != "127.0.0.1" && host != "localhost") return false;
        if (colon == std::string::npos) return true;
        const std::string port = authority.substr(colon + 1);
        return !port.empty() && port.find_first_not_of("0123456789") == std::string::npos;
    }

    bool IsLocalOrigin(const std::string& value) {
        size_t start = value.rfind("http://", 0) == 0 ? 7 : value.rfind("https://", 0) == 0 ? 8 : std::string::npos;
        if (start == std::string::npos) return false;
        size_t end = value.find('/', start);
        return IsLocalAuthority(value.substr(start, end - start));
    }

    bool SecureTokenEquals(const std::string& supplied) {
        size_t diff = supplied.size() ^ g_token.size();
        const size_t length = (std::max)(supplied.size(), g_token.size());
        for (size_t i = 0; i < length; ++i) {
            const unsigned char a = i < supplied.size() ? static_cast<unsigned char>(supplied[i]) : 0;
            const unsigned char b = i < g_token.size() ? static_cast<unsigned char>(g_token[i]) : 0;
            diff |= a ^ b;
        }
        return diff == 0;
    }

    bool IsPublicPath(const std::string& path) {
        return path == "/status" || path == "/health" || path == "/tools" || path == "/openapi.json";
    }

    bool RequestDeclaresPayload(const httplib::Request& req) {
        // cpp-httplib invokes the pre-routing handler before it has populated
        // req.body. Inspect the transport headers instead so content-type
        // enforcement cannot be bypassed by reaching the route parser first.
        const std::string transferEncoding = req.get_header_value("Transfer-Encoding");
        if (!transferEncoding.empty()) return true;

        const std::string contentLength = req.get_header_value("Content-Length");
        if (contentLength.empty()) return false;
        const auto parsed = request_limits::ParseContentLength(contentLength, (std::numeric_limits<size_t>::max)());
        return !parsed || parsed.length != 0;
    }

    void SetJsonError(httplib::Response& res,
                      int status,
                      const std::string& code,
                      const std::string& requestId,
                      const std::string& message = {}) {
        res.status = status;
        res.set_content(response::Error(code, message, requestId).dump(), "application/json");
    }
}

bool Start(int port, const std::string& configuredToken) {
    if (g_server) return true;
    if (port <= 0 || port > 65535) {
        SetLastError("invalid_port");
        return false;
    }
    g_port = port;
    g_startTimeMs = GetTickCount64();
    g_requestSequence.store(0, std::memory_order_relaxed);
    SetLastError({});
    g_token = LoadOrCreateToken(configuredToken);
    if (g_token.size() < 32) {
        SetLastError("token_generation_failed");
        return false;
    }

    g_server = std::make_unique<httplib::Server>();
    g_server->set_payload_max_length(request_limits::kMaxPayloadBytes);

    g_server->set_pre_routing_handler([](const httplib::Request& req, httplib::Response& res) {
        const std::string requestId = NextRequestId();
        res.set_header("X-Cortex-Request-Id", requestId);
        res.set_header("Cache-Control", "no-store");
        res.set_header("X-Content-Type-Options", "nosniff");

        const std::string host = req.get_header_value("Host");
        if (!host.empty() && !IsLocalAuthority(host)) {
            SetJsonError(res, 403, "invalid_host", requestId);
            return httplib::Server::HandlerResponse::Handled;
        }
        const std::string origin = req.get_header_value("Origin");
        if (!origin.empty() && !IsLocalOrigin(origin)) {
            SetJsonError(res, 403, "untrusted_origin", requestId);
            return httplib::Server::HandlerResponse::Handled;
        }
        if (!IsPublicPath(req.path) && !SecureTokenEquals(req.get_header_value("X-Cortex-Token"))) {
            SetJsonError(res, 401, "invalid_token", requestId);
            return httplib::Server::HandlerResponse::Handled;
        }

        if (RequestDeclaresPayload(req)) {
            const std::string transferEncoding = req.get_header_value("Transfer-Encoding");
            if (!transferEncoding.empty()) {
                SetJsonError(res, 411, "bounded_content_length_required", requestId,
                             "chunked request bodies are not accepted by the local API");
                return httplib::Server::HandlerResponse::Handled;
            }

            const auto contentLength = request_limits::ParseContentLength(req.get_header_value("Content-Length"));
            if (contentLength.error == request_limits::ContentLengthError::Invalid) {
                SetJsonError(res, 400, "invalid_content_length", requestId);
                return httplib::Server::HandlerResponse::Handled;
            }
            if (contentLength.error == request_limits::ContentLengthError::TooLarge) {
                SetJsonError(res, 413, "payload_too_large", requestId,
                             "request body exceeds the 16 MiB API limit");
                return httplib::Server::HandlerResponse::Handled;
            }
        }

        if ((req.method == "POST" || req.method == "PUT" || req.method == "PATCH") &&
            RequestDeclaresPayload(req)) {
            std::string contentType = req.get_header_value("Content-Type");
            std::transform(contentType.begin(), contentType.end(), contentType.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (contentType.rfind("application/json", 0) != 0) {
                SetJsonError(res, 415, "application_json_required", requestId);
                return httplib::Server::HandlerResponse::Handled;
            }
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });

    // Keep the request identifier in the response header. Normal route bodies
    // are intentionally left untouched here: cpp-httplib computes
    // Content-Length before its post-routing hook, so mutating res.body in a
    // post-routing handler can make the declared length stale and truncate the
    // JSON seen by clients. Structured errors above may still include the same
    // request ID because their body is created before response serialization.

    RegisterStatusRoutes(*g_server);
    RegisterModulesRoutes(*g_server);
    RegisterMemoryRoutes(*g_server);
    RegisterScanRoutes(*g_server);
    RegisterDisasmRoutes(*g_server);
    RegisterDebugRoutes(*g_server);
    RegisterSymbolsRoutes(*g_server);
    RegisterProjectRoutes(*g_server);
    RegisterScreenshotRoutes(*g_server);
    RegisterPromptRoutes(*g_server);
    RegisterPatchRoutes(*g_server);
    RegisterInputRoutes(*g_server);
    RegisterFreezeRoutes(*g_server);
    RegisterStructRoutes(*g_server);
    RegisterCallRoutes(*g_server);
    RegisterWatchRoutes(*g_server);
    RegisterAnalysisRoutes(*g_server);
    RegisterDissectRoutes(*g_server);
    RegisterBatchRoutes(*g_server);
    RegisterActionRoutes(*g_server);
    RegisterEventRoutes(*g_server);
    RegisterPointerMapRoutes(*g_server);
    RegisterTraceRoutes(*g_server);
    RegisterGhidraRoutes(*g_server);
    RegisterTimelineRoutes(*g_server);
    RegisterWindowRoutes(*g_server);
    RegisterNetRoutes(*g_server);
    RegisterSessionRoutes(*g_server);
    RegisterMcpRoutes(*g_server);
    RegisterLuaRoutes(*g_server);
    RegisterOcrRoutes(*g_server);

    if (!g_server->bind_to_port("127.0.0.1", port)) {
        SetLastError("bind_failed");
        g_server.reset();
        return false;
    }

    g_thread = std::thread([] {
        // 127.0.0.1 only -- an AI-controllable memory read/write endpoint
        // must never be reachable from the network.
        if (!g_server->listen_after_bind()) SetLastError("listen_failed");
    });
    dbglog::Line("API token file: %s", g_tokenPath.c_str());
    return true;
}

void Stop() {
    if (!g_server) return;
    events::WakeAll();
    g_server->stop();
    if (g_thread.joinable()) g_thread.join();
    g_server.reset();
}

int GetPort() { return g_port; }

unsigned long long GetUptimeMs() {
    return GetTickCount64() - g_startTimeMs;
}

bool IsRunning() { return g_server && g_server->is_running(); }
std::string GetLastError() {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    return g_lastError;
}
std::string GetTokenPath() { return g_tokenPath; }
std::string GetToken() { std::lock_guard<std::mutex> lock(g_stateMutex); return g_token; }

} // namespace api