#include "payload_client.h"

#include "api/mcp_pipe_protocol.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace cortex::services {
namespace {

using json = nlohmann::json;

void SetError(std::string* error, std::string value) {
    if (error) *error = std::move(value);
}

std::string ReadTokenFile(const std::filesystem::path& path) {
    std::ifstream file(path);
    std::string token;
    file >> token;
    return token.size() >= 32 ? token : std::string();
}

std::string MakeClientSessionId() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    std::ostringstream out;
#if defined(_WIN32)
    out << "ui-" << GetCurrentProcessId() << '-';
#else
    out << "ui-";
#endif
    out << std::hex << std::chrono::duration_cast<std::chrono::microseconds>(now).count();
    return out.str();
}

std::string ExtractToolError(const json& value) {
    if (!value.is_object()) return "payload_tool_failed";
    if (value.contains("error")) {
        if (value["error"].is_string()) return value["error"].get<std::string>();
        if (value["error"].is_object())
            return value["error"].value("message", value["error"].value("code", std::string("payload_tool_failed")));
    }
    if (value.contains("result") && value["result"].is_object()) {
        const auto& nested = value["result"];
        if (nested.contains("error") && nested["error"].is_string()) return nested["error"].get<std::string>();
    }
    return "payload_tool_failed";
}

const char* ArchitectureAssetName(target::Architecture architecture) {
    switch (architecture) {
        case target::Architecture::X86: return "x86";
        case target::Architecture::X64: return "x64";
        case target::Architecture::Arm64: return "arm64";
        default: return nullptr;
    }
}

std::filesystem::path RuntimeAssetDirectory(const std::string& runtimeDirectory,
                                            target::Architecture architecture) {
    const auto root = std::filesystem::u8path(runtimeDirectory);
    const char* architectureName = ArchitectureAssetName(architecture);
    if (!architectureName) return root;

    const auto candidate = root / "runtime" / architectureName;
    std::error_code error;
    if (std::filesystem::is_directory(candidate, error)) return candidate;
    return root; // compatibility with early unified previews and v0.6 layouts
}

#if defined(_WIN32)

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

HANDLE OpenPipe(const std::string& name, int attempts) {
    for (int attempt = 0; attempt < attempts; ++attempt) {
        HANDLE pipe = CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE,
                                  0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) return pipe;
        const DWORD code = GetLastError();
        if (code == ERROR_PIPE_BUSY) WaitNamedPipeA(name.c_str(), 50);
        else std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return INVALID_HANDLE_VALUE;
}

std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(towlower(character));
    });
    return value;
}

uintptr_t RemoteModuleBase(DWORD pid, const wchar_t* wanted) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    uintptr_t result = 0;
    const std::wstring wantedLower = Lower(wanted ? wanted : L"");
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (Lower(entry.szModule) == wantedLower) {
                result = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

bool HostMatchesTargetArchitecture(target::Architecture architecture) {
#if defined(_WIN64)
    return architecture == target::Architecture::X64;
#else
    return architecture == target::Architecture::X86;
#endif
}

bool InjectLibrary(DWORD pid, const std::filesystem::path& path, std::string* error) {
    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                 PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                                 FALSE, pid);
    if (!process) {
        SetError(error, "payload_open_process_failed:" + std::to_string(GetLastError()));
        return false;
    }

    const std::wstring widePath = path.wstring();
    const SIZE_T byteSize = (widePath.size() + 1) * sizeof(wchar_t);
    LPVOID remotePath = VirtualAllocEx(process, nullptr, byteSize,
                                       MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath) {
        SetError(error, "payload_remote_alloc_failed:" + std::to_string(GetLastError()));
        CloseHandle(process);
        return false;
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remotePath, widePath.c_str(), byteSize, &written) || written != byteSize) {
        SetError(error, "payload_remote_write_failed:" + std::to_string(GetLastError()));
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    HMODULE localKernel = GetModuleHandleW(L"kernel32.dll");
    FARPROC localLoadLibrary = localKernel ? GetProcAddress(localKernel, "LoadLibraryW") : nullptr;
    const uintptr_t remoteKernel = RemoteModuleBase(pid, L"kernel32.dll");
    if (!localKernel || !localLoadLibrary || remoteKernel == 0) {
        SetError(error, "payload_loadlibrary_resolution_failed");
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    const uintptr_t loadLibraryRva = reinterpret_cast<uintptr_t>(localLoadLibrary) -
                                     reinterpret_cast<uintptr_t>(localKernel);
    const auto remoteLoadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteKernel + loadLibraryRva);
    HANDLE thread = CreateRemoteThread(process, nullptr, 0, remoteLoadLibrary, remotePath, 0, nullptr);
    if (!thread) {
        SetError(error, "payload_remote_thread_failed:" + std::to_string(GetLastError()));
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    const DWORD wait = WaitForSingleObject(thread, 10000);
    DWORD exitCode = 0;
    const bool finished = wait == WAIT_OBJECT_0 && GetExitCodeThread(thread, &exitCode) && exitCode != 0;
    if (!finished)
        SetError(error, wait == WAIT_TIMEOUT ? "payload_load_timeout" : "payload_load_failed");

    CloseHandle(thread);
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    CloseHandle(process);
    return finished;
}

bool RunBootstrapHelper(DWORD pid,
                        const std::filesystem::path& helperPath,
                        const std::filesystem::path& payloadPath,
                        std::string* error) {
    if (!std::filesystem::exists(helperPath)) {
        SetError(error, "payload_cross_bitness_helper_missing");
        return false;
    }

    std::wstring command = L"\"" + helperPath.wstring() + L"\" --pid " +
                           std::to_wstring(static_cast<unsigned long long>(pid)) +
                           L" --dll \"" + payloadPath.wstring() + L"\"";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const std::wstring workingDirectory = helperPath.parent_path().wstring();
    if (!CreateProcessW(helperPath.wstring().c_str(), mutableCommand.data(),
                        nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
                        &startup, &process)) {
        SetError(error, "payload_helper_start_failed:" + std::to_string(GetLastError()));
        return false;
    }

    const DWORD wait = WaitForSingleObject(process.hProcess, 20000);
    DWORD exitCode = 0xffffffffu;
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 124);
        SetError(error, "payload_helper_timeout");
    } else if (wait != WAIT_OBJECT_0 || !GetExitCodeProcess(process.hProcess, &exitCode) || exitCode != 0) {
        SetError(error, "payload_helper_failed:" + std::to_string(exitCode));
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return wait == WAIT_OBJECT_0 && exitCode == 0;
}

#endif

} // namespace

PayloadClient::PayloadClient(target::SessionManager& sessions, std::string runtimeDirectory)
    : sessions_(sessions), runtimeDirectory_(std::move(runtimeDirectory)), mcpSessionId_(MakeClientSessionId()) {}

void PayloadClient::SetRuntimeDirectory(std::string runtimeDirectory) {
    std::lock_guard<std::mutex> lock(mutex_);
    runtimeDirectory_ = std::move(runtimeDirectory);
    token_.clear();
    pipeName_.clear();
    verifiedProcessId_ = 0;
}

void PayloadClient::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    token_.clear();
    pipeName_.clear();
    verifiedProcessId_ = 0;
    mcpSessionId_ = MakeClientSessionId();
}

bool PayloadClient::Ready() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return verifiedProcessId_ != 0 && !token_.empty() && !pipeName_.empty();
}

uint64_t PayloadClient::TargetProcessId() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return verifiedProcessId_;
}

bool PayloadClient::ConnectExisting(const target::TargetDescriptor& target, std::string* error) {
#if !defined(_WIN32)
    (void)target;
    SetError(error, "payload_transport_not_supported_on_platform");
    return false;
#else
    std::string runtimeDirectory;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        runtimeDirectory = runtimeDirectory_;
    }
    if (runtimeDirectory.empty()) {
        SetError(error, "payload_runtime_directory_missing");
        return false;
    }

    const auto assetDirectory = RuntimeAssetDirectory(runtimeDirectory, target.architecture);
    const std::string token = ReadTokenFile(assetDirectory / "cortex.token");
    if (token.empty()) {
        SetError(error, "payload_token_unavailable");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        token_ = token;
        pipeName_ = api::mcp_pipe_protocol::PipeNameForToken(token);
        verifiedProcessId_ = 0;
    }
    if (!VerifyTarget(target, error)) {
        std::lock_guard<std::mutex> lock(mutex_);
        verifiedProcessId_ = 0;
        return false;
    }
    return true;
#endif
}

bool PayloadClient::VerifyTarget(const target::TargetDescriptor& target, std::string* error) {
    json response;
    const auto id = requestSequence_.fetch_add(1, std::memory_order_relaxed) + 1;
    const json message = {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"method", "tools/call"},
        {"params", {{"name", "status"}, {"arguments", json::object()}}}
    };
    if (!RoundTrip(message, response, error, 4, "all", "2026-07-28", false)) return false;
    if (!response.is_object() || response.contains("error") || !response.contains("result")) {
        SetError(error, "payload_status_invalid_response");
        return false;
    }

    const json mcpResult = response["result"];
    const json structured = mcpResult.value("structuredContent", json::object());
    const json routeResult = structured.value("result", json::object());
    if (!routeResult.is_object() || !routeResult.contains("pid")) {
        SetError(error, "payload_status_missing_pid");
        return false;
    }

    uint64_t pid = 0;
    try {
        pid = routeResult.at("pid").get<uint64_t>();
    } catch (...) {
        SetError(error, "payload_status_invalid_pid");
        return false;
    }
    if (pid != target.processId) {
        SetError(error, "payload_target_mismatch:" + std::to_string(pid));
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    verifiedProcessId_ = pid;
    return true;
}

bool PayloadClient::InjectPayload(const target::TargetDescriptor& target, std::string* error) {
#if !defined(_WIN32)
    (void)target;
    SetError(error, "payload_injection_not_supported_on_platform");
    return false;
#else
    if (target.platform != target::Platform::Windows) {
        SetError(error, "payload_injection_not_supported_on_target");
        return false;
    }

    std::string runtimeDirectory;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        runtimeDirectory = runtimeDirectory_;
    }
    const auto assetDirectory = RuntimeAssetDirectory(runtimeDirectory, target.architecture);
    const auto payloadPath = assetDirectory / "cortex_core.dll";
    if (!std::filesystem::exists(payloadPath)) {
        SetError(error, "payload_binary_missing");
        return false;
    }

    if (HostMatchesTargetArchitecture(target.architecture))
        return InjectLibrary(static_cast<DWORD>(target.processId), payloadPath, error);

#if defined(_WIN64)
    if (target.architecture == target::Architecture::X86) {
        return RunBootstrapHelper(static_cast<DWORD>(target.processId),
                                  assetDirectory / "cortex_runtime_helper.exe",
                                  payloadPath,
                                  error);
    }
#endif

    SetError(error, "payload_cross_bitness_helper_required");
    return false;
#endif
}

bool PayloadClient::EnsureReady(std::string* error) {
    if (error) error->clear();
    const auto session = sessions_.Active();
    if (!session || !session->Alive()) {
        SetError(error, "no_active_session");
        return false;
    }
    const auto target = session->Target();

    if (ConnectExisting(target, nullptr)) return true;
    Reset();

    if (!InjectPayload(target, error)) return false;

    std::string connectError;
    for (int attempt = 0; attempt < 120; ++attempt) {
        if (ConnectExisting(target, &connectError)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    SetError(error, connectError.empty() ? "payload_start_timeout" : connectError);
    return false;
}

bool PayloadClient::RoundTrip(const json& message,
                              json& response,
                              std::string* error,
                              int attempts,
                              const std::string& toolProfile,
                              const std::string& transportProtocolVersion,
                              bool allowEmptyResponse,
                              bool* hasResponse) const {
    response = json();
    if (hasResponse) *hasResponse = false;
#if !defined(_WIN32)
    (void)message;
    (void)attempts;
    (void)toolProfile;
    (void)transportProtocolVersion;
    (void)allowEmptyResponse;
    SetError(error, "payload_transport_not_supported_on_platform");
    return false;
#else
    std::string token;
    std::string pipeName;
    std::string sessionId;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        token = token_;
        pipeName = pipeName_;
        sessionId = mcpSessionId_;
    }
    if (token.empty() || pipeName.empty()) {
        SetError(error, "payload_not_connected");
        return false;
    }

    HANDLE pipe = OpenPipe(pipeName, std::max(1, attempts));
    if (pipe == INVALID_HANDLE_VALUE) {
        SetError(error, "payload_pipe_unreachable:" + std::to_string(GetLastError()));
        return false;
    }

    json envelope = {
        {"token", token},
        {"tools", toolProfile},
        {"session", sessionId},
        {"message", message}
    };
    if (!transportProtocolVersion.empty())
        envelope["protocolVersion"] = transportProtocolVersion;

    const std::string payload = envelope.dump();
    std::string rawResponse;
    const bool ioOk = WriteFrame(pipe, payload) && ReadFrame(pipe, rawResponse);
    CloseHandle(pipe);
    if (!ioOk) {
        SetError(error, "payload_pipe_roundtrip_failed");
        return false;
    }
    if (rawResponse.empty()) {
        if (allowEmptyResponse) return true;
        SetError(error, "payload_empty_response");
        return false;
    }
    try {
        response = json::parse(rawResponse);
        if (hasResponse) *hasResponse = true;
        return true;
    } catch (const std::exception& exception) {
        SetError(error, std::string("payload_invalid_json:") + exception.what());
        return false;
    }
#endif
}

bool PayloadClient::CallTool(const std::string& name,
                             const json& arguments,
                             json& output,
                             std::string* error) {
    output = json::object();
    if (error) error->clear();
    if (name.empty() || !arguments.is_object()) {
        SetError(error, "invalid_payload_tool_call");
        return false;
    }
    if (!EnsureReady(error)) return false;

    const auto id = requestSequence_.fetch_add(1, std::memory_order_relaxed) + 1;
    const json message = {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"method", "tools/call"},
        {"params", {{"name", name}, {"arguments", arguments}}}
    };

    json response;
    if (!RoundTrip(message, response, error, 100, "all", "2026-07-28", false)) {
        Reset();
        return false;
    }
    if (!response.is_object()) {
        SetError(error, "payload_rpc_invalid_response");
        return false;
    }
    if (response.contains("error")) {
        output = response["error"];
        SetError(error, ExtractToolError(response));
        return false;
    }
    if (!response.contains("result") || !response["result"].is_object()) {
        SetError(error, "payload_rpc_missing_result");
        return false;
    }

    const json& mcpResult = response["result"];
    output = mcpResult.value("structuredContent", json::object());
    if (mcpResult.value("isError", false)) {
        SetError(error, ExtractToolError(output));
        return false;
    }
    return true;
}

bool PayloadClient::ForwardMcp(const json& message,
                               const std::string& toolProfile,
                               json& response,
                               bool& hasResponse,
                               std::string* error) {
    response = json();
    hasResponse = false;
    if (error) error->clear();
    if (toolProfile != "compact" && toolProfile != "all") {
        SetError(error, "invalid_mcp_tool_profile");
        return false;
    }
    if (!Ready() && !EnsureReady(error)) return false;

    if (!RoundTrip(message, response, error, 100, toolProfile, std::string(), true, &hasResponse)) {
        Reset();
        return false;
    }
    return true;
}

} // namespace cortex::services
