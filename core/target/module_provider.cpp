#include "module_provider.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>

#if defined(_WIN32)
#include <windows.h>
#include <tlhelp32.h>
#elif defined(__linux__)
#include <limits.h>
#endif

namespace cortex::target {
namespace {

#if defined(_WIN32)
std::string WideToUtf8(const wchar_t* text) {
    if (!text || !*text) return {};
    const int length = static_cast<int>(wcslen(text));
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string result(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, length, result.data(), bytes, nullptr, nullptr);
    return result;
}
#endif

std::string FileName(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // namespace

std::vector<ModuleInfo> ListTargetModules(const TargetDescriptor& target, std::string* error) {
    if (error) error->clear();
    std::vector<ModuleInfo> modules;
    if (target.processId == 0) {
        if (error) *error = "invalid_target";
        return modules;
    }

#if defined(_WIN32)
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                               static_cast<DWORD>(target.processId));
    if (snapshot == INVALID_HANDLE_VALUE) {
        if (error) *error = "module_snapshot_failed:" + std::to_string(GetLastError());
        return modules;
    }

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry)) {
        do {
            ModuleInfo module;
            module.name = WideToUtf8(entry.szModule);
            module.path = WideToUtf8(entry.szExePath);
            module.base = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(entry.modBaseAddr));
            module.size = static_cast<uint64_t>(entry.modBaseSize);
            modules.push_back(std::move(module));
        } while (Module32NextW(snapshot, &entry));
    } else if (error) {
        *error = "module_enumeration_failed:" + std::to_string(GetLastError());
    }
    CloseHandle(snapshot);
#elif defined(__linux__)
    std::ifstream maps("/proc/" + std::to_string(target.processId) + "/maps");
    if (!maps) {
        if (error) *error = "module_maps_open_failed";
        return modules;
    }

    struct Aggregate { uint64_t begin = UINT64_MAX; uint64_t end = 0; };
    std::map<std::string, Aggregate> grouped;
    std::string line;
    while (std::getline(maps, line)) {
        std::istringstream stream(line);
        std::string range, permissions, offset, device, inode;
        if (!(stream >> range >> permissions >> offset >> device >> inode)) continue;
        std::string path;
        std::getline(stream, path);
        const auto first = path.find_first_not_of(' ');
        if (first == std::string::npos) continue;
        path.erase(0, first);
        if (path.empty() || path.front() == '[') continue;

        const auto dash = range.find('-');
        if (dash == std::string::npos) continue;
        try {
            const uint64_t begin = std::stoull(range.substr(0, dash), nullptr, 16);
            const uint64_t end = std::stoull(range.substr(dash + 1), nullptr, 16);
            if (end <= begin) continue;
            auto& aggregate = grouped[path];
            aggregate.begin = std::min(aggregate.begin, begin);
            aggregate.end = std::max(aggregate.end, end);
        } catch (...) {
        }
    }

    modules.reserve(grouped.size());
    for (const auto& [path, aggregate] : grouped) {
        if (aggregate.begin == UINT64_MAX || aggregate.end <= aggregate.begin) continue;
        modules.push_back({FileName(path), path, aggregate.begin, aggregate.end - aggregate.begin});
    }
#else
    if (error) *error = "module_enumeration_not_supported";
#endif

    std::sort(modules.begin(), modules.end(), [](const ModuleInfo& left, const ModuleInfo& right) {
        return left.base < right.base;
    });
    return modules;
}

} // namespace cortex::target
