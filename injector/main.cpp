// `cortex inject`: loads the Cortex runtime into a running process, matched
// by PID or name substring. The target's architecture decides which runtime is
// used (runtime\x64 or runtime\x86 next to the executable); a 32-bit target
// seen from a 64-bit Cortex is loaded through the private x86 helper, because
// only a 32-bit process can call LoadLibraryW inside another 32-bit process.
#include <windows.h>
#include "../host/cli_entry_points.h"
#include "../core/process/remote_loader.h"
#include <tlhelp32.h>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>

namespace {

struct ProcEntry {
    DWORD pid;
    std::string name;
};

std::vector<ProcEntry> ListProcesses() {
    std::vector<ProcEntry> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;

    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) {
        do {
            out.push_back({pe.th32ProcessID, pe.szExeFile});
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return out;
}

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::wstring Widen(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_ACP, 0, text.c_str(), -1, nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text.c_str(), -1, result.data(), size);
    result.resize(static_cast<size_t>(size - 1));
    return result;
}

std::string Narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_ACP, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_ACP, 0, text.c_str(), -1, result.data(), size, nullptr, nullptr);
    result.resize(static_cast<size_t>(size - 1));
    return result;
}

std::wstring AbsolutePath(const std::wstring& path) {
    const DWORD required = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (required == 0) return path;
    std::wstring result(required, L'\0');
    const DWORD written = GetFullPathNameW(path.c_str(), required, result.data(), nullptr);
    if (written == 0 || written >= required) return path;
    result.resize(written);
    return result;
}

std::wstring ExecutableDirectory() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) return L".";
        if (length < path.size()) { path.resize(length); break; }
        path.resize(path.size() * 2);
    }
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

bool FileExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring FirstExisting(const std::vector<std::wstring>& candidates) {
    for (const auto& candidate : candidates)
        if (FileExists(candidate)) return candidate;
    return {};
}

int RunX86Helper(const std::wstring& helper, DWORD pid, const std::wstring& dll) {
    std::wstring command = L"\"" + helper + L"\" --pid " + std::to_wstring(pid) +
                           L" --dll \"" + dll + L"\"";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    // Hand the helper our standard handles so its error messages reach the
    // caller, without opening a console window of its own.
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(helper.c_str(), mutableCommand.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        std::cerr << "Could not start the x86 helper (error " << GetLastError() << ").\n";
        return 1;
    }
    const DWORD wait = WaitForSingleObject(process.hProcess, 30000);
    DWORD exitCode = 1;
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 124);
        std::cerr << "The x86 helper timed out.\n";
    } else {
        GetExitCodeProcess(process.hProcess, &exitCode);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return wait == WAIT_OBJECT_0 && exitCode == 0 ? 0 : 1;
}

} // namespace

int CortexInjectMain(int argc, char** argv) {
    using cortex::remote_loader::Machine;
    using cortex::remote_loader::MachineName;

    if (argc < 2) {
        std::cout << "Cortex injector\n\n"
                  << "Usage:\n"
                  << "  cortex inject <process-name-substring-or-pid> [path-to-dll]\n\n"
                  << "  Without a DLL path, the runtime matching the target is used:\n"
                  << "  runtime\\x64\\cortex_core.dll or runtime\\x86\\cortex_core.dll next to\n"
                  << "  this executable. 32-bit targets are loaded through\n"
                  << "  runtime\\x86\\cortex_runtime_helper.exe automatically.\n\n"
                  << "Running processes:\n";
        for (const auto& p : ListProcesses()) {
            std::cout << "  " << p.pid << "\t" << p.name << "\n";
        }
        return 1;
    }

    const std::string target = argv[1];
    DWORD pid = 0;
    std::string matchedName;
    const bool isNumeric = !target.empty() && std::all_of(target.begin(), target.end(), ::isdigit);
    if (isNumeric) {
        pid = static_cast<DWORD>(std::stoul(target));
    } else {
        const std::string needle = ToLower(target);
        for (const auto& p : ListProcesses()) {
            if (ToLower(p.name).find(needle) != std::string::npos) {
                pid = p.pid;
                matchedName = p.name;
                std::cout << "Matched process: " << p.name << " (pid " << pid << ")\n";
                break;
            }
        }
    }
    if (pid == 0) {
        std::cerr << "No matching process found for \"" << target << "\"\n";
        return 1;
    }

    Machine targetMachine = Machine::Unknown;
    if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        targetMachine = cortex::remote_loader::ProcessMachine(process);
        CloseHandle(process);
    } else {
        std::cerr << "Cannot open pid " << pid << " (error " << GetLastError()
                  << "). Run Cortex as administrator if the target is elevated.\n";
        return 1;
    }
    const Machine self = cortex::remote_loader::CurrentMachine();
    if (targetMachine == Machine::Unknown) targetMachine = self;
    const std::wstring root = ExecutableDirectory();
    const std::wstring archDirectory = root + L"\\runtime\\" + Widen(MachineName(targetMachine));

    std::wstring dll;
    if (argc >= 3) {
        dll = AbsolutePath(Widen(argv[2]));
        if (!FileExists(dll)) {
            std::cerr << "DLL not found: " << argv[2] << "\n";
            return 1;
        }
    } else {
        std::vector<std::wstring> candidates{archDirectory + L"\\cortex_core.dll"};
        if (targetMachine == self) candidates.push_back(root + L"\\cortex_core.dll");
        dll = FirstExisting(candidates);
        if (dll.empty()) {
            std::cerr << "No " << MachineName(targetMachine) << " runtime found: expected "
                      << Narrow(candidates.front()) << "\n";
            return 1;
        }
    }

    const Machine dllMachine = cortex::remote_loader::FileMachine(dll);
    if (dllMachine != Machine::Unknown && dllMachine != targetMachine) {
        std::cerr << Narrow(dll) << " is built for " << MachineName(dllMachine) << ", but pid "
                  << pid << " runs as " << MachineName(targetMachine) << ". Leave out the DLL path"
                  << " to use runtime\\" << MachineName(targetMachine) << "\\cortex_core.dll.\n";
        return 1;
    }

    std::cout << "Loading " << Narrow(dll) << " into pid " << pid << " ("
              << MachineName(targetMachine) << ")...\n";

    if (targetMachine == self) {
        std::string error;
        if (!cortex::remote_loader::InjectSameBitness(pid, dll, 15000, &error)) {
            std::cerr << "Injection failed: " << error << "\n";
            return 1;
        }
    } else if (self == Machine::X64 && targetMachine == Machine::X86) {
        const std::wstring dllDirectory = dll.substr(0, dll.find_last_of(L"\\/"));
        const std::wstring helper = FirstExisting({
            root + L"\\runtime\\x86\\cortex_runtime_helper.exe",
            dllDirectory + L"\\cortex_runtime_helper.exe",
            root + L"\\cortex_runtime_helper.exe"});
        if (helper.empty()) {
            std::cerr << "pid " << pid << " is a 32-bit process and the x86 helper is missing:"
                      << " expected runtime\\x86\\cortex_runtime_helper.exe next to Cortex.\n";
            return 1;
        }
        if (RunX86Helper(helper, pid, dll) != 0) {
            std::cerr << "Injection through the x86 helper failed.\n";
            return 1;
        }
    } else {
        std::cerr << "A " << MachineName(self) << " Cortex cannot load a runtime into a "
                  << MachineName(targetMachine) << " process. Use the x64 cortex.exe.\n";
        return 1;
    }

    std::cout << "Injected successfully.\n";
    return 0;
}

#ifdef CORTEX_STANDALONE_TOOL
// Standalone build of this tool (outside cortex.exe / cortex_host.exe).
int main(int argc, char** argv) { return CortexInjectMain(argc, argv); }
#endif
