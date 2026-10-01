#pragma once

// Whole-process suspend and resume, used to freeze a target while the value
// scanner reads it and by the desktop's Pause target command. Header-only and
// free of Cortex dependencies.

#include <cstdint>
#include <cstdio>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <cerrno>
#include <csignal>
#include <cstring>
#include <sys/types.h>
#endif

namespace cortex::process_control {

#if defined(_WIN32)
namespace detail {
using NtProcessCall = LONG(NTAPI*)(HANDLE);

inline NtProcessCall Resolve(const char* name) {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll ? reinterpret_cast<NtProcessCall>(GetProcAddress(ntdll, name)) : nullptr;
}

inline bool Call(uint64_t pid, const char* name, std::string* error) {
    const NtProcessCall call = Resolve(name);
    if (!call) {
        if (error) *error = std::string(name) + "_unavailable";
        return false;
    }
    HANDLE process = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, static_cast<DWORD>(pid));
    if (!process) {
        if (error) *error = "process_open_failed:" + std::to_string(GetLastError());
        return false;
    }
    const LONG status = call(process);
    CloseHandle(process);
    if (status < 0) {
        char text[32] = {};
        std::snprintf(text, sizeof(text), "0x%08lX", static_cast<unsigned long>(status));
        if (error) *error = std::string(name) + "_failed:" + text;
        return false;
    }
    if (error) error->clear();
    return true;
}
} // namespace detail

// Every successful Suspend must be paired with one Resume: Windows counts
// suspensions per thread.
inline bool Suspend(uint64_t pid, std::string* error = nullptr) {
    return detail::Call(pid, "NtSuspendProcess", error);
}

inline bool Resume(uint64_t pid, std::string* error = nullptr) {
    return detail::Call(pid, "NtResumeProcess", error);
}
#elif defined(__linux__)
inline bool Suspend(uint64_t pid, std::string* error = nullptr) {
    if (kill(static_cast<pid_t>(pid), SIGSTOP) == 0) return true;
    if (error) *error = std::string("sigstop_failed:") + std::strerror(errno);
    return false;
}

inline bool Resume(uint64_t pid, std::string* error = nullptr) {
    if (kill(static_cast<pid_t>(pid), SIGCONT) == 0) return true;
    if (error) *error = std::string("sigcont_failed:") + std::strerror(errno);
    return false;
}
#else
inline bool Suspend(uint64_t, std::string* error = nullptr) {
    if (error) *error = "process_suspend_not_supported";
    return false;
}

inline bool Resume(uint64_t, std::string* error = nullptr) {
    if (error) *error = "process_resume_not_supported";
    return false;
}
#endif

// Keeps a process suspended for the lifetime of the guard.
class SuspendGuard {
public:
    SuspendGuard() = default;
    explicit SuspendGuard(uint64_t pid) : pid_(pid) { active_ = Suspend(pid_, &error_); }
    ~SuspendGuard() { Release(); }
    SuspendGuard(const SuspendGuard&) = delete;
    SuspendGuard& operator=(const SuspendGuard&) = delete;

    bool Active() const { return active_; }
    const std::string& Error() const { return error_; }
    void Release() {
        if (active_) Resume(pid_, nullptr);
        active_ = false;
    }

private:
    uint64_t pid_ = 0;
    bool active_ = false;
    std::string error_;
};

} // namespace cortex::process_control
