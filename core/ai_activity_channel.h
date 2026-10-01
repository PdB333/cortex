#pragma once

#include <windows.h>

#include <cctype>
#include <cstdint>
#include <string>

namespace cortex::ai_activity {

constexpr DWORD kMaxMessageBytes = 64 * 1024;

inline std::string SanitizeEndpointPart(std::string value) {
    for (char& ch : value) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (!std::isalnum(c) && ch != '-' && ch != '_') ch = '_';
    }
    if (value.empty()) value = "user";
    return value;
}

inline std::string EndpointName() {
    char user[256] = {};
    const DWORD userSize = GetEnvironmentVariableA(
        "USERNAME", user, static_cast<DWORD>(sizeof(user)));
    const std::string identity =
        userSize > 0 && userSize < sizeof(user)
            ? SanitizeEndpointPart(std::string(user, userSize))
            : std::string("user");

    DWORD sessionId = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &sessionId);

    return "\\\\.\\mailslot\\cortex-ai-activity-v1-" +
           std::to_string(sessionId) + "-" + identity;
}

inline HANDLE OpenInbox() {
    const std::string endpoint = EndpointName();
    return CreateMailslotA(endpoint.c_str(), kMaxMessageBytes, 0, nullptr);
}

inline bool Publish(const std::string& payload) {
    if (payload.empty() || payload.size() > kMaxMessageBytes) return false;

    const std::string endpoint = EndpointName();
    HANDLE out = CreateFileA(
        endpoint.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    const bool ok =
        WriteFile(out, payload.data(), static_cast<DWORD>(payload.size()),
                  &written, nullptr) &&
        written == payload.size();
    CloseHandle(out);
    return ok;
}

} // namespace cortex::ai_activity
