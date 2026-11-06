// Desktop settings persistence: the settings file and the runtime cortex.ini
// are replaced atomically, round-trip, and keep the keys Cortex does not own.
// Needs no window and no target.

#include "application/settings.h"
#include "services/user_data.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

namespace fs = std::filesystem;
using cortex::application::SettingsStore;

int failures = 0;

#define CHECK(condition)                                                              \
    do {                                                                              \
        if (!(condition)) {                                                           \
            ++failures;                                                               \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #condition \
                      << std::endl;                                                   \
        }                                                                             \
    } while (false)

std::string ReadAll(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

void WriteAll(const fs::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

fs::path FreshDirectory(const char* name) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path directory = fs::temp_directory_path() / (std::string(name) + "-" + std::to_string(stamp));
    fs::remove_all(directory);
    fs::create_directories(directory);
    return directory;
}

void TestAtomicWriteHelper() {
    const fs::path directory = FreshDirectory("cortex-atomic-write");
    const fs::path file = directory / "state.json";

    std::string error;
    CHECK(cortex::services::WriteFileAtomically(file, "first", &error));
    CHECK(ReadAll(file) == "first");
    CHECK(cortex::services::WriteFileAtomically(file, "second", &error));
    CHECK(ReadAll(file) == "second");
    CHECK(!fs::exists(directory / "state.json.tmp"));

    // A stale temporary left by a crashed write is replaced, not appended to.
    WriteAll(directory / "state.json.tmp", "garbage from a crashed write that is longer");
    CHECK(cortex::services::WriteFileAtomically(file, "third", &error));
    CHECK(ReadAll(file) == "third");
    CHECK(!fs::exists(directory / "state.json.tmp"));

    // A failed write reports an error and leaves no partial file behind.
    const fs::path missing = directory / "missing" / "state.json";
    CHECK(!cortex::services::WriteFileAtomically(missing, "never", &error));
    CHECK(!error.empty());
    CHECK(!fs::exists(missing));

    // When the destination cannot be replaced the previous content survives.
    const fs::path blocked = directory / "blocked";
    fs::create_directories(blocked / "child");
    error.clear();
    CHECK(!cortex::services::WriteFileAtomically(blocked, "never", &error));
    CHECK(fs::is_directory(blocked / "child"));
    CHECK(!fs::exists(directory / "blocked.tmp"));

    fs::remove_all(directory);
}

void TestSettingsRoundTripAndIniSync() {
    const fs::path directory = FreshDirectory("cortex-settings-store");
    WriteAll(directory / cortex::services::kPortableMarker, "");
    WriteAll(directory / "cortex_core.dll", "");
    WriteAll(directory / "cortex.ini",
             "# operator comment\n"
             "api_token=keep-me\n"
             "http_api_enabled=true\n"
             "HTTP_API_ENABLED=duplicate\n");

    {
        SettingsStore store(directory.u8string());
        CHECK(store.Path() == directory / "cortex-ui-settings.json");
        store.Values().memoryBytesPerRow = 32;
        store.Values().httpApiEnabled = false;
        store.Values().sessionHistoryLimit = 42;
        store.Values().projectDirectory = "D:/projects";
        std::string error;
        CHECK(store.SaveAndSync(&error));
        CHECK(error.empty());
    }

    CHECK(!fs::exists(directory / "cortex-ui-settings.json.tmp"));
    CHECK(!fs::exists(directory / "cortex.ini.tmp"));

    {
        SettingsStore reloaded(directory.u8string());
        CHECK(reloaded.Values().memoryBytesPerRow == 32);
        CHECK(!reloaded.Values().httpApiEnabled);
        CHECK(reloaded.Values().sessionHistoryLimit == 42);
        CHECK(reloaded.Values().projectDirectory == "D:/projects");
    }

    const std::string ini = ReadAll(directory / "cortex.ini");
    CHECK(ini.find("# operator comment") != std::string::npos);
    CHECK(ini.find("api_token=keep-me") != std::string::npos);
    CHECK(ini.find("http_api_enabled=false") != std::string::npos);
    CHECK(ini.find("duplicate") == std::string::npos);
    CHECK(ini.find("session_history_limit=42") != std::string::npos);
    CHECK(ini.find("project_directory=D:/projects") != std::string::npos);

    // A crash between writing the temporary and renaming it leaves the
    // previous settings readable.
    WriteAll(directory / "cortex-ui-settings.json.tmp", "{ truncated");
    {
        SettingsStore afterCrash(directory.u8string());
        CHECK(afterCrash.Values().memoryBytesPerRow == 32);
        std::string error;
        CHECK(afterCrash.Save(&error));
    }
    CHECK(!fs::exists(directory / "cortex-ui-settings.json.tmp"));

    fs::remove_all(directory);
}

} // namespace

int main() {
    TestAtomicWriteHelper();
    TestSettingsRoundTripAndIniSync();
    if (failures) {
        std::cerr << failures << " settings store check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "settings store tests passed" << std::endl;
    return 0;
}
