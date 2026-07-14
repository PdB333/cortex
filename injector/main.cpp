// Standalone CLI injector: injects cortex_core.dll into a running
// process, matched by name substring or PID. Standard
// CreateRemoteThread + LoadLibraryW technique -- works on any 32-bit process
// that doesn't have anti-cheat/anti-injection protection.
#include <windows.h>
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

std::string GetAbsolutePath(const std::string& relative) {
    char buf[MAX_PATH];
    if (GetFullPathNameA(relative.c_str(), MAX_PATH, buf, nullptr) == 0) return relative;
    return buf;
}

bool InjectDll(DWORD pid, const std::string& dllPath) {
    HANDLE hProcess = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
        PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, pid);
    if (!hProcess) {
        std::cerr << "OpenProcess failed (err " << GetLastError()
                  << "). Try running the injector as administrator.\n";
        return false;
    }

    LPVOID remotePath = VirtualAllocEx(hProcess, nullptr, dllPath.size() + 1,
                                        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath) {
        std::cerr << "VirtualAllocEx failed (err " << GetLastError() << ")\n";
        CloseHandle(hProcess);
        return false;
    }

    if (!WriteProcessMemory(hProcess, remotePath, dllPath.c_str(), dllPath.size() + 1, nullptr)) {
        std::cerr << "WriteProcessMemory failed (err " << GetLastError() << ")\n";
        VirtualFreeEx(hProcess, remotePath, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    auto loadLibraryAddr = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(hKernel32, "LoadLibraryA"));

    HANDLE hThread = CreateRemoteThread(hProcess, nullptr, 0, loadLibraryAddr,
                                         remotePath, 0, nullptr);
    if (!hThread) {
        std::cerr << "CreateRemoteThread failed (err " << GetLastError()
                  << "). If the target is 64-bit, this 32-bit injector can't reach it.\n";
        VirtualFreeEx(hProcess, remotePath, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    WaitForSingleObject(hThread, INFINITE);

    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);

    VirtualFreeEx(hProcess, remotePath, 0, MEM_RELEASE);
    CloseHandle(hThread);
    CloseHandle(hProcess);

    if (exitCode == 0) {
        std::cerr << "LoadLibraryA returned NULL in the target process -- the DLL failed to load "
                     "(missing dependency, or bitness mismatch).\n";
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cout << "Cortex injector\n\n"
                  << "Usage:\n"
                  << "  injector.exe <process-name-substring-or-pid> [path-to-dll]\n\n"
                  << "  If path-to-dll is omitted, cortex_core.dll next to this\n"
                  << "  executable is used.\n\n"
                  << "Running processes:\n";
        for (const auto& p : ListProcesses()) {
            std::cout << "  " << p.pid << "\t" << p.name << "\n";
        }
        return 1;
    }

    std::string target = argv[1];
    std::string dllPath = argc >= 3 ? argv[2] : "cortex_core.dll";
    dllPath = GetAbsolutePath(dllPath);

    if (GetFileAttributesA(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::cerr << "DLL not found: " << dllPath << "\n";
        return 1;
    }

    DWORD pid = 0;
    bool isNumeric = !target.empty() && std::all_of(target.begin(), target.end(), ::isdigit);
    if (isNumeric) {
        pid = static_cast<DWORD>(std::stoul(target));
    } else {
        std::string needle = ToLower(target);
        for (const auto& p : ListProcesses()) {
            if (ToLower(p.name).find(needle) != std::string::npos) {
                pid = p.pid;
                std::cout << "Matched process: " << p.name << " (pid " << pid << ")\n";
                break;
            }
        }
    }

    if (pid == 0) {
        std::cerr << "No matching process found for \"" << target << "\"\n";
        return 1;
    }

    std::cout << "Injecting " << dllPath << " into pid " << pid << "...\n";
    if (!InjectDll(pid, dllPath)) {
        return 1;
    }

    std::cout << "Injected successfully.\n";
    return 0;
}
