#include "thread_provider.h"

#include <algorithm>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#include <tlhelp32.h>
#elif defined(__linux__)
#include <dirent.h>
#include <cstdlib>
#endif

namespace cortex::target {
namespace {

void SetError(std::string* error, const char* value) {
    if (error) *error = value ? value : "";
}

#if defined(_WIN32)

class SuspendedThread final {
public:
    explicit SuspendedThread(HANDLE thread) : thread_(thread) {
        suspended_ = thread_ && SuspendThread(thread_) != static_cast<DWORD>(-1);
    }
    ~SuspendedThread() {
        if (suspended_) ResumeThread(thread_);
    }
    bool Ok() const { return suspended_; }

private:
    HANDLE thread_ = nullptr;
    bool suspended_ = false;
};

void Push(std::vector<RegisterValue>& values, const char* name, uint64_t value) {
    values.push_back(RegisterValue{name, value});
}

#endif

} // namespace

std::vector<uint64_t> ListTargetThreads(const TargetDescriptor& target, std::string* error) {
    if (error) error->clear();
    std::vector<uint64_t> result;
    if (target.processId == 0) {
        SetError(error, "invalid_target");
        return result;
    }

#if defined(_WIN32)
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        if (error) *error = "thread_snapshot_failed:" + std::to_string(GetLastError());
        return result;
    }

    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID == static_cast<DWORD>(target.processId))
                result.push_back(static_cast<uint64_t>(entry.th32ThreadID));
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
#elif defined(__linux__)
    const std::string path = "/proc/" + std::to_string(target.processId) + "/task";
    DIR* directory = opendir(path.c_str());
    if (!directory) {
        SetError(error, "thread_list_failed");
        return result;
    }
    while (auto* entry = readdir(directory)) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
        char* end = nullptr;
        const auto id = std::strtoull(entry->d_name, &end, 10);
        if (end && *end == '\0' && id != 0) result.push_back(id);
    }
    closedir(directory);
#else
    SetError(error, "thread_list_not_supported");
#endif

    std::sort(result.begin(), result.end());
    return result;
}

bool ReadTargetThreadRegisters(const TargetDescriptor& target,
                               uint64_t threadId,
                               ThreadRegisterSnapshot& snapshot,
                               std::string* error) {
    snapshot = {};
    if (error) error->clear();
    if (target.processId == 0 || threadId == 0) {
        SetError(error, "invalid_thread");
        return false;
    }

#if defined(_WIN32)
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION,
                               FALSE, static_cast<DWORD>(threadId));
    if (!thread) {
        if (error) *error = "thread_open_failed:" + std::to_string(GetLastError());
        return false;
    }

    SuspendedThread suspended(thread);
    if (!suspended.Ok()) {
        if (error) *error = "thread_suspend_failed:" + std::to_string(GetLastError());
        CloseHandle(thread);
        return false;
    }

    snapshot.threadId = threadId;
    bool ok = false;

#if defined(_WIN64)
    if (target.architecture == Architecture::X86) {
        WOW64_CONTEXT context{};
        context.ContextFlags = WOW64_CONTEXT_FULL;
        if (Wow64GetThreadContext(thread, &context)) {
            snapshot.instructionPointer = context.Eip;
            Push(snapshot.registers, "EAX", context.Eax);
            Push(snapshot.registers, "EBX", context.Ebx);
            Push(snapshot.registers, "ECX", context.Ecx);
            Push(snapshot.registers, "EDX", context.Edx);
            Push(snapshot.registers, "ESI", context.Esi);
            Push(snapshot.registers, "EDI", context.Edi);
            Push(snapshot.registers, "EBP", context.Ebp);
            Push(snapshot.registers, "ESP", context.Esp);
            Push(snapshot.registers, "EIP", context.Eip);
            Push(snapshot.registers, "EFLAGS", context.EFlags);
            ok = true;
        }
    } else if (target.architecture == Architecture::X64) {
        CONTEXT context{};
        context.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(thread, &context)) {
            snapshot.instructionPointer = context.Rip;
            Push(snapshot.registers, "RAX", context.Rax);
            Push(snapshot.registers, "RBX", context.Rbx);
            Push(snapshot.registers, "RCX", context.Rcx);
            Push(snapshot.registers, "RDX", context.Rdx);
            Push(snapshot.registers, "RSI", context.Rsi);
            Push(snapshot.registers, "RDI", context.Rdi);
            Push(snapshot.registers, "RBP", context.Rbp);
            Push(snapshot.registers, "RSP", context.Rsp);
            Push(snapshot.registers, "R8", context.R8);
            Push(snapshot.registers, "R9", context.R9);
            Push(snapshot.registers, "R10", context.R10);
            Push(snapshot.registers, "R11", context.R11);
            Push(snapshot.registers, "R12", context.R12);
            Push(snapshot.registers, "R13", context.R13);
            Push(snapshot.registers, "R14", context.R14);
            Push(snapshot.registers, "R15", context.R15);
            Push(snapshot.registers, "RIP", context.Rip);
            Push(snapshot.registers, "EFLAGS", context.EFlags);
            ok = true;
        }
    } else {
        SetError(error, "register_architecture_not_supported");
    }
#else
    if (target.architecture == Architecture::X86) {
        CONTEXT context{};
        context.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(thread, &context)) {
            snapshot.instructionPointer = context.Eip;
            Push(snapshot.registers, "EAX", context.Eax);
            Push(snapshot.registers, "EBX", context.Ebx);
            Push(snapshot.registers, "ECX", context.Ecx);
            Push(snapshot.registers, "EDX", context.Edx);
            Push(snapshot.registers, "ESI", context.Esi);
            Push(snapshot.registers, "EDI", context.Edi);
            Push(snapshot.registers, "EBP", context.Ebp);
            Push(snapshot.registers, "ESP", context.Esp);
            Push(snapshot.registers, "EIP", context.Eip);
            Push(snapshot.registers, "EFLAGS", context.EFlags);
            ok = true;
        }
    } else {
        SetError(error, "register_bitness_not_supported");
    }
#endif

    if (!ok && error && error->empty())
        *error = "get_thread_context_failed:" + std::to_string(GetLastError());
    CloseHandle(thread);
    return ok;
#elif defined(__linux__)
    SetError(error, "register_read_requires_debug_backend");
    return false;
#else
    SetError(error, "register_read_not_supported");
    return false;
#endif
}

} // namespace cortex::target
