#pragma once

// module+RVA between the live process and static analysis (step 7): which
// loaded module contains an address, and which loaded module is the file an
// artifact SHA-256 names. A module matches an artifact only when its file on
// disk has the same SHA-256; a name alone is never enough.

#include "ui_context.h"

#include "../../core/artifacts/artifact.h"
#include "../../core/services/module_service.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace cortex::ui {

struct LiveModule {
    std::string name;
    std::string path;
    uint64_t base = 0;
    uint64_t size = 0;
};

// SHA-256 of a module file, cached by path, size and modification time.
inline std::string ModuleFileSha256(const std::string& path) {
    struct Entry {
        uintmax_t size = 0;
        std::filesystem::file_time_type time{};
        std::string sha256;
    };
    static std::map<std::string, Entry> cache;
    std::error_code error;
    const auto file = std::filesystem::u8path(path);
    const uintmax_t size = std::filesystem::file_size(file, error);
    if (error) return {};
    const auto time = std::filesystem::last_write_time(file, error);
    if (error) return {};
    auto& entry = cache[path];
    if (entry.sha256.empty() || entry.size != size || entry.time != time) {
        const auto source = cortex::artifacts::OpenFile(path);
        entry = {size, time, source ? cortex::artifacts::Sha256Of(*source) : std::string()};
    }
    return entry.sha256;
}

inline std::vector<LiveModule> LiveModules(UiContext& context) {
    std::vector<LiveModule> modules;
    if (!context.modules || !context.sessions || !context.sessions->ActiveTarget()) return modules;
    for (const auto& module : context.modules->List()) modules.push_back({module.name, module.path, module.base, module.size});
    return modules;
}

// The loaded module containing `address`, with a file on disk.
inline std::optional<LiveModule> ModuleAt(UiContext& context, uint64_t address) {
    for (const auto& module : LiveModules(context))
        if (address >= module.base && address - module.base < module.size && !module.path.empty()) return module;
    return std::nullopt;
}

// The loaded module whose file has this SHA-256. `nameHint` (a file name)
// narrows which files are hashed first.
inline std::optional<LiveModule> ModuleForSha256(UiContext& context, const std::string& sha256, const std::string& nameHint = {}) {
    if (sha256.size() != 64) return std::nullopt;
    auto modules = LiveModules(context);
    std::stable_partition(modules.begin(), modules.end(), [&](const LiveModule& module) {
        return !nameHint.empty() && std::filesystem::u8path(module.path).filename().u8string() == nameHint;
    });
    for (const auto& module : modules)
        if (!module.path.empty() && ModuleFileSha256(module.path) == sha256) return module;
    return std::nullopt;
}

} // namespace cortex::ui
