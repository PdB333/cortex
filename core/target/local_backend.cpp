#include "local_backend.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cwchar>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

#if defined(_WIN32)
#include <windows.h>
#include <tlhelp32.h>
#elif defined(__linux__)
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
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

bool RegionReadable(DWORD protect) {
    if ((protect & PAGE_GUARD) != 0 || (protect & 0xff) == PAGE_NOACCESS) return false;
    switch (protect & 0xff) {
        case PAGE_READONLY:
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
    }
}

bool RegionWritable(DWORD protect) {
    switch (protect & 0xff) {
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
    }
}

bool RegionExecutable(DWORD protect) {
    switch (protect & 0xff) {
        case PAGE_EXECUTE:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
    }
}

class WindowsProcessSession final : public Session {
public:
    WindowsProcessSession(TargetDescriptor target, HANDLE process, bool writable)
        : target_(std::move(target)), process_(process), writable_(writable) {
        capabilities_ = target_.capabilities;
        capabilities_.Add(Capability::MemoryRead).Add(Capability::MemoryScan);
        if (writable_) capabilities_.Add(Capability::MemoryWrite);
        target_.capabilities = capabilities_;
    }

    ~WindowsProcessSession() override {
        if (process_) CloseHandle(process_);
    }

    const TargetDescriptor& Target() const override { return target_; }
    const CapabilitySet& Capabilities() const override { return capabilities_; }

    bool Alive() const override {
        if (!process_) return false;
        DWORD code = 0;
        return GetExitCodeProcess(process_, &code) && code == STILL_ACTIVE;
    }

    bool ReadMemory(uint64_t address, void* buffer, size_t size, size_t* bytesRead) const override {
        if (bytesRead) *bytesRead = 0;
        if (!process_ || !buffer || size == 0) return false;
        SIZE_T read = 0;
        const BOOL ok = ReadProcessMemory(process_, reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(address)), buffer, size, &read);
        if (bytesRead) *bytesRead = static_cast<size_t>(read);
        return ok != FALSE && read == size;
    }

    bool WriteMemory(uint64_t address, const void* buffer, size_t size, size_t* bytesWritten) override {
        if (bytesWritten) *bytesWritten = 0;
        if (!writable_ || !process_ || !buffer || size == 0) return false;
        SIZE_T written = 0;
        const BOOL ok = WriteProcessMemory(process_, reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), buffer, size, &written);
        if (ok && written > 0) FlushInstructionCache(process_, reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(address)), written);
        if (bytesWritten) *bytesWritten = static_cast<size_t>(written);
        return ok != FALSE && written == size;
    }

    std::vector<MemoryRegion> MemoryRegions() const override {
        std::vector<MemoryRegion> regions;
        if (!process_) return regions;

        SYSTEM_INFO systemInfo{};
        GetSystemInfo(&systemInfo);
        uintptr_t address = reinterpret_cast<uintptr_t>(systemInfo.lpMinimumApplicationAddress);
        const uintptr_t maximum = reinterpret_cast<uintptr_t>(systemInfo.lpMaximumApplicationAddress);

        while (address < maximum) {
            MEMORY_BASIC_INFORMATION info{};
            const SIZE_T queried = VirtualQueryEx(process_, reinterpret_cast<LPCVOID>(address), &info, sizeof(info));
            if (queried == 0) break;

            const auto base = reinterpret_cast<uintptr_t>(info.BaseAddress);
            if (info.State == MEM_COMMIT && info.RegionSize > 0) {
                MemoryRegion region;
                region.base = static_cast<uint64_t>(base);
                region.size = static_cast<uint64_t>(info.RegionSize);
                region.readable = RegionReadable(info.Protect);
                region.writable = region.readable && RegionWritable(info.Protect);
                region.executable = region.readable && RegionExecutable(info.Protect);
                regions.push_back(region);
            }

            const uintptr_t next = base + info.RegionSize;
            if (next <= address) break;
            address = next;
        }
        return regions;
    }

private:
    TargetDescriptor target_;
    CapabilitySet capabilities_;
    HANDLE process_ = nullptr;
    bool writable_ = false;
};

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

class LinuxProcessSession final : public Session {
public:
    LinuxProcessSession(TargetDescriptor target, int fd, bool writable)
        : target_(std::move(target)), fd_(fd), writable_(writable) {
        capabilities_ = target_.capabilities;
        capabilities_.Add(Capability::MemoryRead).Add(Capability::MemoryScan);
        if (writable_) capabilities_.Add(Capability::MemoryWrite);
        target_.capabilities = capabilities_;
    }

    ~LinuxProcessSession() override {
        if (fd_ >= 0) close(fd_);
    }

    const TargetDescriptor& Target() const override { return target_; }
    const CapabilitySet& Capabilities() const override { return capabilities_; }
    bool Alive() const override { return target_.processId > 0 && (kill(static_cast<pid_t>(target_.processId), 0) == 0 || errno == EPERM); }

    bool ReadMemory(uint64_t address, void* buffer, size_t size, size_t* bytesRead) const override {
        if (bytesRead) *bytesRead = 0;
        if (fd_ < 0 || !buffer || size == 0) return false;
        const ssize_t result = pread(fd_, buffer, size, static_cast<off_t>(address));
        if (bytesRead && result > 0) *bytesRead = static_cast<size_t>(result);
        return result == static_cast<ssize_t>(size);
    }

    bool WriteMemory(uint64_t address, const void* buffer, size_t size, size_t* bytesWritten) override {
        if (bytesWritten) *bytesWritten = 0;
        if (!writable_ || fd_ < 0 || !buffer || size == 0) return false;
        const ssize_t result = pwrite(fd_, buffer, size, static_cast<off_t>(address));
        if (bytesWritten && result > 0) *bytesWritten = static_cast<size_t>(result);
        return result == static_cast<ssize_t>(size);
    }

    std::vector<MemoryRegion> MemoryRegions() const override {
        std::vector<MemoryRegion> regions;
        std::ifstream maps("/proc/" + std::to_string(target_.processId) + "/maps");
        std::string line;
        while (std::getline(maps, line)) {
            std::istringstream stream(line);
            std::string range;
            std::string permissions;
            if (!(stream >> range >> permissions)) continue;
            const auto dash = range.find('-');
            if (dash == std::string::npos) continue;
            try {
                const uint64_t begin = std::stoull(range.substr(0, dash), nullptr, 16);
                const uint64_t end = std::stoull(range.substr(dash + 1), nullptr, 16);
                if (end <= begin) continue;
                MemoryRegion region;
                region.base = begin;
                region.size = end - begin;
                region.readable = permissions.size() > 0 && permissions[0] == 'r';
                region.writable = permissions.size() > 1 && permissions[1] == 'w';
                region.executable = permissions.size() > 2 && permissions[2] == 'x';
                regions.push_back(region);
            } catch (...) {
            }
        }
        return regions;
    }

private:
    TargetDescriptor target_;
    CapabilitySet capabilities_;
    int fd_ = -1;
    bool writable_ = false;
};

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

SessionPtr LocalBackend::Attach(const TargetDescriptor& target, std::string* error) {
    if (error) error->clear();
    if (target.nodeId != node_.id || target.kind != TargetKind::Process || target.processId == 0) {
        if (error) *error = "invalid_target";
        return {};
    }

#if defined(_WIN32)
    const DWORD fullAccess = PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION;
    HANDLE process = OpenProcess(fullAccess, FALSE, static_cast<DWORD>(target.processId));
    bool writable = process != nullptr;
    if (!process) {
        process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, static_cast<DWORD>(target.processId));
    }
    if (!process) {
        if (error) *error = "process_open_failed:" + std::to_string(GetLastError());
        return {};
    }
    return std::make_shared<WindowsProcessSession>(target, process, writable);
#elif defined(__linux__)
    const std::string path = "/proc/" + std::to_string(target.processId) + "/mem";
    int fd = open(path.c_str(), O_RDWR);
    bool writable = fd >= 0;
    if (fd < 0) fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        if (error) *error = "process_open_failed";
        return {};
    }
    return std::make_shared<LinuxProcessSession>(target, fd, writable);
#else
    if (error) *error = "attach_not_supported";
    return {};
#endif
}

} // namespace cortex::target
