#pragma once

#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace cortex::services {

// Name of the marker file that keeps desktop settings beside cortex.exe
// (portable installs on removable media, CI bundles).
inline constexpr const char* kPortableMarker = "cortex.portable";

// Directory for per-user desktop state (UI settings, window layout).
//
// cortex.exe can live in a read-only location such as Program Files and can be
// started from any working directory, so user state goes to
// %LOCALAPPDATA%\Cortex. A `cortex.portable` file beside the executable keeps
// everything in the application directory instead. When the per-user
// directory cannot be created the application directory is used as a last
// resort. Runtime configuration (cortex.ini) is not user state: the injected
// runtime reads it beside its own DLL and it stays there.
inline std::filesystem::path UserDataDirectory(const std::filesystem::path& applicationDirectory) {
    std::error_code error;
    if (std::filesystem::exists(applicationDirectory / kPortableMarker, error))
        return applicationDirectory;

#if defined(_WIN32)
    const wchar_t* localAppData = _wgetenv(L"LOCALAPPDATA");
    if (localAppData && *localAppData) {
        const std::filesystem::path directory = std::filesystem::path(localAppData) / L"Cortex";
        std::filesystem::create_directories(directory, error);
        if (!error) return directory;
    }
#else
    if (const char* home = std::getenv("HOME"); home && *home) {
        const std::filesystem::path directory = std::filesystem::path(home) / ".local" / "share" / "cortex";
        std::filesystem::create_directories(directory, error);
        if (!error) return directory;
    }
#endif
    return applicationDirectory;
}

// Returns `userDirectory / fileName`, first copying a file of that name from
// the application directory if one exists there and the per-user copy does
// not. This carries settings and layouts over from releases that wrote them
// beside cortex.exe.
inline std::filesystem::path UserDataFile(const std::filesystem::path& applicationDirectory,
                                          const std::filesystem::path& userDirectory,
                                          const char* fileName) {
    const std::filesystem::path target = userDirectory / fileName;
    std::error_code error;
    if (userDirectory != applicationDirectory && !std::filesystem::exists(target, error)) {
        const std::filesystem::path legacy = applicationDirectory / fileName;
        if (std::filesystem::is_regular_file(legacy, error))
            std::filesystem::copy_file(legacy, target, error);
    }
    return target;
}

}  // namespace cortex::services
