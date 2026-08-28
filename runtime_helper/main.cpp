#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
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

std::wstring AbsolutePath(const std::wstring& path) {
    DWORD required = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (required == 0) return path;
    std::wstring result(required, L'\0');
    const DWORD written = GetFullPathNameW(path.c_str(), required, result.data(), nullptr);
    if (written == 0 || written >= required) return path;
    result.resize(written);
    return result;
}

bool InjectLibrary(DWORD pid, const std::wstring& dllPath) {
    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                 PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                                 FALSE, pid);
    if (!process) {
        std::wcerr << L"cortex runtime helper: OpenProcess failed: " << GetLastError() << L'\n';
        return false;
    }

    const SIZE_T byteSize = (dllPath.size() + 1) * sizeof(wchar_t);
    LPVOID remotePath = VirtualAllocEx(process, nullptr, byteSize,
                                       MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath) {
        std::wcerr << L"cortex runtime helper: VirtualAllocEx failed: " << GetLastError() << L'\n';
        CloseHandle(process);
        return false;
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remotePath, dllPath.c_str(), byteSize, &written) || written != byteSize) {
        std::wcerr << L"cortex runtime helper: WriteProcessMemory failed: " << GetLastError() << L'\n';
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    HMODULE localKernel = GetModuleHandleW(L"kernel32.dll");
    FARPROC localLoadLibrary = localKernel ? GetProcAddress(localKernel, "LoadLibraryW") : nullptr;
    const uintptr_t remoteKernel = RemoteModuleBase(pid, L"kernel32.dll");
    if (!localKernel || !localLoadLibrary || remoteKernel == 0) {
        std::wcerr << L"cortex runtime helper: LoadLibraryW resolution failed\n";
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    const uintptr_t loadLibraryRva = reinterpret_cast<uintptr_t>(localLoadLibrary) -
                                     reinterpret_cast<uintptr_t>(localKernel);
    const auto remoteLoadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteKernel + loadLibraryRva);
    HANDLE thread = CreateRemoteThread(process, nullptr, 0, remoteLoadLibrary, remotePath, 0, nullptr);
    if (!thread) {
        std::wcerr << L"cortex runtime helper: CreateRemoteThread failed: " << GetLastError() << L'\n';
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    const DWORD wait = WaitForSingleObject(thread, 15000);
    DWORD exitCode = 0;
    const bool loaded = wait == WAIT_OBJECT_0 &&
                        GetExitCodeThread(thread, &exitCode) &&
                        exitCode != 0;
    if (!loaded) {
        std::wcerr << L"cortex runtime helper: payload load failed";
        if (wait == WAIT_TIMEOUT) std::wcerr << L" (timeout)";
        std::wcerr << L'\n';
    }

    CloseHandle(thread);
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    CloseHandle(process);
    return loaded;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    std::optional<DWORD> pid;
    std::wstring dllPath;

    for (int index = 1; index < argc; ++index) {
        const std::wstring argument = argv[index] ? argv[index] : L"";
        if (argument == L"--pid" && index + 1 < argc) {
            try {
                const auto parsed = std::stoull(argv[++index]);
                if (parsed == 0 || parsed > 0xffffffffull) throw std::out_of_range("pid");
                pid = static_cast<DWORD>(parsed);
            } catch (...) {
                std::wcerr << L"cortex runtime helper: invalid --pid\n";
                return 2;
            }
        } else if (argument == L"--dll" && index + 1 < argc) {
            dllPath = argv[++index] ? argv[index] : L"";
        } else {
            std::wcerr << L"cortex runtime helper: unknown or incomplete argument\n";
            return 2;
        }
    }

    if (!pid || dllPath.empty()) {
        std::wcerr << L"cortex runtime helper: --pid and --dll are required\n";
        return 2;
    }

    dllPath = AbsolutePath(dllPath);
    const DWORD attributes = GetFileAttributesW(dllPath.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        std::wcerr << L"cortex runtime helper: payload not found\n";
        return 3;
    }

    return InjectLibrary(*pid, dllPath) ? 0 : 4;
}
