#pragma once

// Memory management in another process for the Auto Assembler and the
// memory tools: allocate (near an address, for rel32 jumps), free, change
// protection, write code, start a thread. Header-only and free of Cortex
// dependencies; Windows only, other platforms report unsupported.

#include <cstdint>
#include <cstdio>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace cortex::remote_memory {

#if defined(_WIN32)
namespace detail {

class Process {
public:
    Process(uint64_t pid, DWORD access) : handle_(OpenProcess(access, FALSE, static_cast<DWORD>(pid))) {}
    ~Process() {
        if (handle_) CloseHandle(handle_);
    }
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    HANDLE Get() const { return handle_; }

private:
    HANDLE handle_;
};

inline bool Fail(std::string* error, const char* what) {
    if (error) *error = std::string(what) + ":" + std::to_string(GetLastError());
    return false;
}

inline LPVOID Pointer(uint64_t address) {
    return reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address));
}

} // namespace detail

// Reserves and commits `size` bytes of read/write/execute memory. With
// `near`, the block is placed within +-2 GB of it when possible, so code
// there can jump to `near` with a 5-byte jmp.
inline bool Allocate(uint64_t pid, size_t size, uint64_t nearAddress, uint64_t& address, std::string* error = nullptr) {
    detail::Process process(pid, PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION);
    if (!process.Get()) return detail::Fail(error, "process_open_failed");
    if (size == 0) size = 0x1000;
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const uint64_t granularity = info.dwAllocationGranularity ? info.dwAllocationGranularity : 0x10000;
    if (nearAddress) {
        const uint64_t window = 0x7FF00000ull;
        const uint64_t minimum = reinterpret_cast<uintptr_t>(info.lpMinimumApplicationAddress);
        const uint64_t maximum = reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress);
        const uint64_t low = nearAddress > minimum + window ? nearAddress - window : minimum;
        const uint64_t high = nearAddress + window < maximum ? nearAddress + window : maximum;
        // Walk the free regions from `near` outwards, above then below.
        for (int direction = 0; direction < 2; ++direction) {
            uint64_t cursor = nearAddress;
            for (int steps = 0; steps < 4096; ++steps) {
                MEMORY_BASIC_INFORMATION region{};
                if (!VirtualQueryEx(process.Get(), detail::Pointer(cursor), &region, sizeof(region))) break;
                const uint64_t base = reinterpret_cast<uintptr_t>(region.BaseAddress);
                const uint64_t end = base + region.RegionSize;
                if (region.State == MEM_FREE) {
                    uint64_t candidate = (base + granularity - 1) / granularity * granularity;
                    if (direction == 1 && end >= size) {
                        const uint64_t top = (end - size) / granularity * granularity;
                        if (top >= candidate) candidate = top;
                    }
                    if (candidate >= low && candidate + size <= high && candidate + size <= end) {
                        LPVOID block = VirtualAllocEx(process.Get(), detail::Pointer(candidate), size,
                                                      MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
                        if (block) {
                            address = reinterpret_cast<uintptr_t>(block);
                            if (error) error->clear();
                            return true;
                        }
                    }
                }
                if (direction == 0) {
                    if (end <= cursor || end >= high) break;
                    cursor = end;
                } else {
                    if (base <= low || base == 0) break;
                    cursor = base - 1;
                }
            }
        }
    }
    LPVOID block = VirtualAllocEx(process.Get(), nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!block) return detail::Fail(error, "virtual_alloc_failed");
    address = reinterpret_cast<uintptr_t>(block);
    if (error) error->clear();
    return true;
}

inline bool Free(uint64_t pid, uint64_t address, std::string* error = nullptr) {
    detail::Process process(pid, PROCESS_VM_OPERATION);
    if (!process.Get()) return detail::Fail(error, "process_open_failed");
    if (!VirtualFreeEx(process.Get(), detail::Pointer(address), 0, MEM_RELEASE)) return detail::Fail(error, "virtual_free_failed");
    if (error) error->clear();
    return true;
}

// Makes a range readable, writable and executable (Cheat Engine's
// fullaccess).
inline bool FullAccess(uint64_t pid, uint64_t address, size_t size, std::string* error = nullptr) {
    detail::Process process(pid, PROCESS_VM_OPERATION);
    if (!process.Get()) return detail::Fail(error, "process_open_failed");
    DWORD old = 0;
    if (!VirtualProtectEx(process.Get(), detail::Pointer(address), size, PAGE_EXECUTE_READWRITE, &old))
        return detail::Fail(error, "virtual_protect_failed");
    if (error) error->clear();
    return true;
}

// Writes code or data whatever the page protection, then restores it and
// flushes the instruction cache.
inline bool WriteCode(uint64_t pid, uint64_t address, const void* data, size_t size, std::string* error = nullptr) {
    detail::Process process(pid, PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ | PROCESS_QUERY_INFORMATION);
    if (!process.Get()) return detail::Fail(error, "process_open_failed");
    DWORD old = 0;
    const bool unprotected = VirtualProtectEx(process.Get(), detail::Pointer(address), size, PAGE_EXECUTE_READWRITE, &old) != FALSE;
    SIZE_T written = 0;
    const bool ok = WriteProcessMemory(process.Get(), detail::Pointer(address), data, size, &written) && written == size;
    const DWORD writeError = GetLastError();
    if (unprotected) {
        DWORD ignored = 0;
        VirtualProtectEx(process.Get(), detail::Pointer(address), size, old, &ignored);
    }
    FlushInstructionCache(process.Get(), detail::Pointer(address), size);
    if (!ok) {
        if (error) *error = "write_failed:" + std::to_string(writeError);
        return false;
    }
    if (error) error->clear();
    return true;
}

inline bool CreateThread(uint64_t pid, uint64_t address, uint64_t parameter, std::string* error = nullptr) {
    detail::Process process(pid, PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                                     PROCESS_VM_WRITE | PROCESS_VM_READ);
    if (!process.Get()) return detail::Fail(error, "process_open_failed");
    HANDLE thread = CreateRemoteThread(process.Get(), nullptr, 0,
                                       reinterpret_cast<LPTHREAD_START_ROUTINE>(static_cast<uintptr_t>(address)),
                                       detail::Pointer(parameter), 0, nullptr);
    if (!thread) return detail::Fail(error, "create_thread_failed");
    CloseHandle(thread);
    if (error) error->clear();
    return true;
}

#else

inline bool Unsupported(std::string* error) {
    if (error) *error = "remote_memory_unsupported_on_this_platform";
    return false;
}
inline bool Allocate(uint64_t, size_t, uint64_t, uint64_t&, std::string* error = nullptr) { return Unsupported(error); }
inline bool Free(uint64_t, uint64_t, std::string* error = nullptr) { return Unsupported(error); }
inline bool FullAccess(uint64_t, uint64_t, size_t, std::string* error = nullptr) { return Unsupported(error); }
inline bool WriteCode(uint64_t, uint64_t, const void*, size_t, std::string* error = nullptr) { return Unsupported(error); }
inline bool CreateThread(uint64_t, uint64_t, uint64_t, std::string* error = nullptr) { return Unsupported(error); }

#endif

} // namespace cortex::remote_memory
