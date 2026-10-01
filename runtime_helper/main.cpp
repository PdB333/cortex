#include <windows.h>
#include <tlhelp32.h>

#include "../core/process/remote_loader.h"

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

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
    std::string error;
    if (cortex::remote_loader::InjectSameBitness(pid, dllPath, 15000, &error)) return true;
    std::wcerr << L"cortex runtime helper: payload load failed: "
               << std::wstring(error.begin(), error.end()) << L'\n';
    return false;
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

    // This helper is the 32-bit loader: both the target and the payload must
    // be x86, or LoadLibraryW fails with an unhelpful error.
    using cortex::remote_loader::Machine;
    const Machine payloadMachine = cortex::remote_loader::FileMachine(dllPath);
    if (payloadMachine != Machine::Unknown && payloadMachine != Machine::X86) {
        std::wcerr << L"cortex runtime helper: payload is not a 32-bit DLL\n";
        return 5;
    }
    if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, *pid)) {
        const Machine targetMachine = cortex::remote_loader::ProcessMachine(process);
        CloseHandle(process);
        if (targetMachine != Machine::Unknown && targetMachine != Machine::X86) {
            std::wcerr << L"cortex runtime helper: target is not a 32-bit process\n";
            return 5;
        }
    }

    return InjectLibrary(*pid, dllPath) ? 0 : 4;
}
