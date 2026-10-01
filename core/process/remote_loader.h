#pragma once

// Loads a DLL into another process of the same bitness. Shared by
// `cortex inject`, the desktop runtime loader and the private x86 helper,
// which are also built as standalone executables: keep this header free of
// Cortex dependencies (Win32 only).

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <string>

namespace cortex::remote_loader {

enum class Machine { Unknown, X86, X64 };

inline const char* MachineName(Machine machine) {
    switch (machine) {
        case Machine::X86: return "x86";
        case Machine::X64: return "x64";
        default: return "unknown";
    }
}

inline Machine CurrentMachine() {
#if defined(_WIN64)
    return Machine::X64;
#else
    return Machine::X86;
#endif
}

inline Machine FromImageMachine(USHORT machine) {
    if (machine == IMAGE_FILE_MACHINE_I386) return Machine::X86;
    if (machine == IMAGE_FILE_MACHINE_AMD64) return Machine::X64;
    return Machine::Unknown;
}

// Architecture of a running process.
inline Machine ProcessMachine(HANDLE process) {
    using IsWow64Process2Fn = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
    const auto isWow64Process2 = reinterpret_cast<IsWow64Process2Fn>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"));
    if (isWow64Process2) {
        USHORT processMachine = IMAGE_FILE_MACHINE_UNKNOWN;
        USHORT nativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;
        if (isWow64Process2(process, &processMachine, &nativeMachine)) {
            // IMAGE_FILE_MACHINE_UNKNOWN means "not under WOW64": native.
            return FromImageMachine(processMachine != IMAGE_FILE_MACHINE_UNKNOWN
                                        ? processMachine : nativeMachine);
        }
    }

    BOOL wow64 = FALSE;
    if (!IsWow64Process(process, &wow64)) return Machine::Unknown;
    if (wow64) return Machine::X86;
    SYSTEM_INFO info{};
    GetNativeSystemInfo(&info);
    if (info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64) return Machine::X64;
    if (info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL) return Machine::X86;
    return Machine::Unknown;
}

// Architecture a PE file (exe or dll) was built for.
inline Machine FileMachine(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return Machine::Unknown;

    Machine result = Machine::Unknown;
    IMAGE_DOS_HEADER dos{};
    DWORD read = 0;
    if (ReadFile(file, &dos, sizeof(dos), &read, nullptr) && read == sizeof(dos) &&
        dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0 &&
        SetFilePointer(file, dos.e_lfanew, nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER) {
        DWORD signature = 0;
        IMAGE_FILE_HEADER header{};
        if (ReadFile(file, &signature, sizeof(signature), &read, nullptr) && read == sizeof(signature) &&
            signature == IMAGE_NT_SIGNATURE &&
            ReadFile(file, &header, sizeof(header), &read, nullptr) && read == sizeof(header)) {
            result = FromImageMachine(header.Machine);
        }
    }
    CloseHandle(file);
    return result;
}

inline std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return value;
}

inline uintptr_t ModuleBase(DWORD pid, const wchar_t* name) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    uintptr_t result = 0;
    const std::wstring wanted = Lower(name ? name : L"");
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (Lower(entry.szModule) == wanted) {
                result = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

inline void SetError(std::string* error, const std::string& value) {
    if (error) *error = value;
}

// Calls LoadLibraryW(path) in the target. The target must have this
// process's bitness: LoadLibraryW is found at the same offset from the
// target's kernel32 base as from ours.
inline bool LoadInto(HANDLE process, DWORD pid, const std::wstring& path,
                     DWORD timeoutMs, std::string* error) {
    const SIZE_T byteSize = (path.size() + 1) * sizeof(wchar_t);
    LPVOID remotePath = VirtualAllocEx(process, nullptr, byteSize,
                                       MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath) {
        SetError(error, "payload_remote_alloc_failed:" + std::to_string(GetLastError()));
        return false;
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remotePath, path.c_str(), byteSize, &written) ||
        written != byteSize) {
        SetError(error, "payload_remote_write_failed:" + std::to_string(GetLastError()));
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        return false;
    }

    HMODULE localKernel = GetModuleHandleW(L"kernel32.dll");
    FARPROC localLoadLibrary = localKernel ? GetProcAddress(localKernel, "LoadLibraryW") : nullptr;
    const uintptr_t remoteKernel = ModuleBase(pid, L"kernel32.dll");
    if (!localKernel || !localLoadLibrary || remoteKernel == 0) {
        SetError(error, "payload_loadlibrary_resolution_failed");
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        return false;
    }

    const uintptr_t loadLibraryRva = reinterpret_cast<uintptr_t>(localLoadLibrary) -
                                     reinterpret_cast<uintptr_t>(localKernel);
    const auto remoteLoadLibrary =
        reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteKernel + loadLibraryRva);
    HANDLE thread = CreateRemoteThread(process, nullptr, 0, remoteLoadLibrary, remotePath, 0, nullptr);
    if (!thread) {
        SetError(error, "payload_remote_thread_failed:" + std::to_string(GetLastError()));
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        return false;
    }

    const DWORD wait = WaitForSingleObject(thread, timeoutMs);
    DWORD exitCode = 0;
    const bool loaded = wait == WAIT_OBJECT_0 && GetExitCodeThread(thread, &exitCode) && exitCode != 0;
    if (!loaded) SetError(error, wait == WAIT_TIMEOUT ? "payload_load_timeout" : "payload_load_failed");

    CloseHandle(thread);
    // A timed-out LoadLibraryW may still be reading the path: leak it.
    if (wait == WAIT_OBJECT_0) VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    return loaded;
}

// cortex_core.dll imports these by name, and Windows looks for a DLL in the
// target's own folder before System32. A game folder can hold a copy of the
// wrong architecture or a proxy (ReShade's opengl32.dll, a d3d8 wrapper, an
// old dbghelp.dll), which breaks or subverts the runtime. Loading the System32
// copy first, when the target has not loaded that DLL itself, makes the
// runtime's imports resolve to it.
inline void PreloadSystemDependencies(HANDLE process, DWORD pid) {
    wchar_t systemDirectory[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return;

    static const wchar_t* const kDependencies[] = {
        L"dbghelp.dll",
        L"opengl32.dll",
#if !defined(_WIN64)
        L"d3d8.dll",
#endif
    };
    for (const wchar_t* name : kDependencies) {
        if (ModuleBase(pid, name) != 0) continue;
        const std::wstring path = std::wstring(systemDirectory) + L"\\" + name;
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        LoadInto(process, pid, path, 5000, nullptr);
    }
}

// Opens the target and loads `path` into it after the System32 dependencies.
inline bool InjectSameBitness(DWORD pid, const std::wstring& path, DWORD timeoutMs,
                              std::string* error) {
    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                 PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                                 FALSE, pid);
    if (!process) {
        SetError(error, "payload_open_process_failed:" + std::to_string(GetLastError()));
        return false;
    }
    PreloadSystemDependencies(process, pid);
    const bool loaded = LoadInto(process, pid, path, timeoutMs, error);
    CloseHandle(process);
    return loaded;
}

}  // namespace cortex::remote_loader
