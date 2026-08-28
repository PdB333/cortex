#include "local_backend.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cwchar>
#include <fstream>
#include <string>
#include <unordered_map>

#if defined(_WIN32)
#include <windows.h>
#include <tlhelp32.h>
#elif defined(__linux__)
#include <dirent.h>
#include <limits.h>
#include <unistd.h>
#endif

namespace cortex::target {
namespace {

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

#if defined(_WIN32)

std::string WideToUtf8(const wchar_t* text, int length = -1) {
    if (!text) return {};
    if (length < 0) length = static_cast<int>(std::wcslen(text));
    if (length <= 0) return {};

    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string result(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, length, result.data(), bytes, nullptr, nullptr);
    return result;
}

Architecture ArchitectureFromMachine(USHORT machine) {
    switch (machine) {
        case IMAGE_FILE_MACHINE_I386: return Architecture::X86;
        case IMAGE_FILE_MACHINE_AMD64: return Architecture::X64;
#ifdef IMAGE_FILE_MACHINE_ARM64
        case IMAGE_FILE_MACHINE_ARM64: return Architecture::Arm64;
#endif
        default: return Architecture::Unknown;
    }
}

Architecture NativeArchitecture() {
    SYSTEM_INFO info{};
    GetNativeSystemInfo(&info);
    switch (info.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_INTEL: return Architecture::X86;
        case PROCESSOR_ARCHITECTURE_AMD64: return Architecture::X64;
#ifdef PROCESSOR_ARCHITECTURE_ARM64
        case PROCESSOR_ARCHITECTURE_ARM64: return Architecture::Arm64;
#endif
        default: return Architecture::Unknown;
    }
}

Architecture ArchitectureForProcess(HANDLE process, Architecture fallback) {
    if (!process) return fallback;

    using IsWow64Process2Fn = BOOL (WINAPI*)(HANDLE, USHORT*, USHORT*);
    static const auto isWow64Process2 = reinterpret_cast<IsWow64Process2Fn>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"));

    if (isWow64Process2) {
        USHORT processMachine = IMAGE_FILE_MACHINE_UNKNOWN;
        USHORT nativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;
        if (isWow64Process2(process, &processMachine, &nativeMachine)) {
            const USHORT machine = processMachine == IMAGE_FILE_MACHINE_UNKNOWN ? nativeMachine : processMachine;
            const auto architecture = ArchitectureFromMachine(machine);
            if (architecture != Architecture::Unknown) return architecture;
        }
    }

    BOOL wow64 = FALSE;
    if (IsWow64Process(process, &wow64) && wow64) return Architecture::X86;
    return fallback;
}

std::string ProcessPath(HANDLE process) {
    if (!process) return {};
    std::wstring buffer(32768, L'\0');
    DWORD size = static_cast<DWORD>(buffer.size());
    if (!QueryFullProcessImageNameW(process, 0, buffer.data(), &size)) return {};
    return WideToUtf8(buffer.data(), static_cast<int>(size));
}

BOOL CALLBACK CollectWindowTitles(HWND hwnd, LPARAM parameter) {
    if (!IsWindowVisible(hwnd)) return TRUE;
    const int length = GetWindowTextLengthW(hwnd);
    if (length <= 0) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return TRUE;

    auto* titles = reinterpret_cast<std::unordered_map<uint64_t, std::string>*>(parameter);
    if (!titles || titles->find(pid) != titles->end()) return TRUE;

    std::wstring title(static_cast<size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(hwnd, title.data(), length + 1);
    if (copied > 0) (*titles)[pid] = WideToUtf8(title.data(), copied);
    return TRUE;
}

std::string ComputerName() {
    wchar_t buffer[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameW(buffer, &size) && size > 0)
        return WideToUtf8(buffer, static_cast<int>(size));
    return "Windows local node";
}

#elif defined(__linux__)

Architecture NativeArchitecture() {
#if defined(__x86_64__)
    return Architecture::X64;
#elif defined(__i386__)
    return Architecture::X86;
#elif defined(__aarch64__)
    return Architecture::Arm64;
#else
    return Architecture::Unknown;
#endif
}

std::string HostName() {
    char buffer[256] = {};
    if (gethostname(buffer, sizeof(buffer) - 1) == 0 && buffer[0] != '\0') return buffer;
    return "Linux local node";
}

#endif

} // namespace

LocalBackend::LocalBackend() {
#if defined(_WIN32)
    node_ = MakeLocalNode("local", ComputerName(), Platform::Windows, NativeArchitecture());
#elif defined(__linux__)
    node_ = MakeLocalNode("local", HostName(), Platform::Linux, NativeArchitecture());
#else
    node_ = MakeLocalNode("local", "Local node", Platform::Unknown, Architecture::Unknown);
#endif
}

NodeDescriptor LocalBackend::Node() const {
    return node_;
}

std::vector<TargetDescriptor> LocalBackend::ListTargets() {
    std::vector<TargetDescriptor> result;

#if defined(_WIN32)
    std::unordered_map<uint64_t, std::string> windowTitles;
    EnumWindows(CollectWindowTitles, reinterpret_cast<LPARAM>(&windowTitles));

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry)) {
            do {
                TargetDescriptor target;
                target.nodeId = node_.id;
                target.platform = Platform::Windows;
                target.kind = TargetKind::Process;
                target.processId = static_cast<uint64_t>(entry.th32ProcessID);
                target.id = MakeProcessTargetId(target.nodeId, target.platform, target.processId);
                target.name = WideToUtf8(entry.szExeFile);
                target.architecture = node_.architecture;
                target.capabilities.Add(Capability::ProcessInfo);

                HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
                if (process) {
                    target.architecture = ArchitectureForProcess(process, target.architecture);
                    target.executablePath = ProcessPath(process);
                    CloseHandle(process);
                }

                const auto title = windowTitles.find(target.processId);
                if (title != windowTitles.end()) target.windowTitle = title->second;
                if (target.Valid()) result.push_back(std::move(target));
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }
#elif defined(__linux__)
    DIR* proc = opendir("/proc");
    if (proc) {
        while (auto* entry = readdir(proc)) {
            if (!std::isdigit(static_cast<unsigned char>(entry->d_name[0]))) continue;
            char* end = nullptr;
            const auto pid = std::strtoull(entry->d_name, &end, 10);
            if (!end || *end != '\0' || pid == 0) continue;

            TargetDescriptor target;
            target.nodeId = node_.id;
            target.platform = Platform::Linux;
            target.architecture = node_.architecture;
            target.kind = TargetKind::Process;
            target.processId = pid;
            target.id = MakeProcessTargetId(target.nodeId, target.platform, target.processId);
            target.capabilities.Add(Capability::ProcessInfo);

            std::ifstream comm("/proc/" + std::to_string(pid) + "/comm");
            std::getline(comm, target.name);
            if (target.name.empty()) target.name = "pid-" + std::to_string(pid);

            char link[PATH_MAX + 1] = {};
            const auto path = "/proc/" + std::to_string(pid) + "/exe";
            const ssize_t length = readlink(path.c_str(), link, PATH_MAX);
            if (length > 0) target.executablePath.assign(link, static_cast<size_t>(length));

            if (target.Valid()) result.push_back(std::move(target));
        }
        closedir(proc);
    }
#endif

    std::sort(result.begin(), result.end(), [](const TargetDescriptor& left, const TargetDescriptor& right) {
        const auto leftName = Lower(left.name);
        const auto rightName = Lower(right.name);
        if (leftName != rightName) return leftName < rightName;
        return left.processId < right.processId;
    });

    return result;
}

} // namespace cortex::target
