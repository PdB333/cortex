// Project persistence recovery (core/project/project.cpp). project::Init runs
// once per process, so each scenario is its own CTest entry:
//
//   cortex_project_store_tests backup   damaged primary, valid .bak
//   cortex_project_store_tests damaged  damaged primary and .bak
//   cortex_project_store_tests fresh    no project file at all
//   cortex_project_store_tests locked   primary unreadable and uncopyable
//
// The project file is named after this executable, in a fresh directory.

#include "project/project.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

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

std::string ProjectName() {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    return fs::path(path).stem().string();
}

std::vector<fs::path> DamagedCopies(const fs::path& directory) {
    std::vector<fs::path> copies;
    for (const auto& entry : fs::directory_iterator(directory))
        if (entry.path().filename().string().find(".damaged-") != std::string::npos) copies.push_back(entry.path());
    return copies;
}

const char* kDamaged = "{\"schema_version\": 3, \"addresses\": {\"lost\": ";

} // namespace

int main(int argc, char** argv) {
    const std::string scenario = argc > 1 ? argv[1] : "backup";
    const fs::path directory = fs::temp_directory_path() /
        ("cortex-project-store-" + scenario + "-" + std::to_string(GetCurrentProcessId()));
    fs::remove_all(directory);
    fs::create_directories(directory);
    const fs::path primary = directory / (ProjectName() + ".json");
    const fs::path backup = directory / (ProjectName() + ".json.bak");

    const std::string goodBackup =
        json{{"schema_version", 3},
             {"addresses", {{"health", {{"address", "0x1234"}, {"type", "i32"}, {"notes", "from backup"}}}}}}
            .dump();

    if (scenario == "backup") {
        WriteAll(primary, kDamaged);
        WriteAll(backup, goodBackup);
    } else if (scenario == "damaged") {
        WriteAll(primary, kDamaged);
        WriteAll(backup, "not json either");
    } else if (scenario == "locked") {
        WriteAll(primary, goodBackup);
    } else if (scenario != "fresh") {
        std::cerr << "unknown scenario " << scenario << std::endl;
        return 2;
    }

    // "locked": another process holds the primary open with no sharing, so it
    // can be neither read nor copied.
    HANDLE lock = INVALID_HANDLE_VALUE;
    if (scenario == "locked") {
        lock = CreateFileW(primary.wstring().c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(lock != INVALID_HANDLE_VALUE);
    }

    project::Init(directory.string());
    const json status = project::LoadStatus();
    const json all = project::GetAll();
    CHECK(status.value("path", std::string()) == primary.string());
    CHECK(all.value("schema_version", 0) == 3);

    if (scenario == "backup") {
        CHECK(status.value("source", std::string()) == "backup");
        CHECK(all["addresses"].contains("health"));
        const auto copies = DamagedCopies(directory);
        CHECK(copies.size() == 1);
        CHECK(status["quarantined"].size() == 1);
        if (copies.size() == 1) CHECK(ReadAll(copies[0]) == kDamaged);

        // Two saves: the damaged bytes must not rotate into the backup, and
        // the quarantined copy must survive.
        CHECK(project::SetAddress("ammo", 0x5678, "i32", "first save"));
        CHECK(project::SetAddress("armor", 0x9abc, "i32", "second save"));
        const json saved = json::parse(ReadAll(primary));
        CHECK(saved["addresses"].contains("health") && saved["addresses"].contains("armor"));
        const json rotated = json::parse(ReadAll(backup), nullptr, false);
        CHECK(!rotated.is_discarded() && rotated.is_object());
        CHECK(DamagedCopies(directory).size() == 1);
    } else if (scenario == "damaged") {
        CHECK(status.value("source", std::string()) == "defaults_after_damage");
        CHECK(all["addresses"].empty());
        const auto copies = DamagedCopies(directory);
        CHECK(copies.size() == 2);
        CHECK(status["quarantined"].size() == 2);
        bool primaryKept = false, backupKept = false;
        for (const auto& copy : copies) {
            const std::string bytes = ReadAll(copy);
            primaryKept = primaryKept || bytes == kDamaged;
            backupKept = backupKept || bytes == "not json either";
        }
        CHECK(primaryKept && backupKept);

        CHECK(project::SetAddress("ammo", 0x5678, "i32", "first save"));
        CHECK(project::SetAddress("armor", 0x9abc, "i32", "second save"));
        CHECK(DamagedCopies(directory).size() == 2);
        CHECK(json::parse(ReadAll(primary))["addresses"].contains("armor"));
    } else if (scenario == "locked") {
        CHECK(status.value("source", std::string()) == "defaults_after_damage");
        CHECK(status.value("saves_blocked", false));
        CHECK(status["quarantined"].empty());
        CHECK(!project::SetAddress("ammo", 0x5678, "i32", "must not replace the locked file"));
        CHECK(!project::GetAll()["addresses"].contains("ammo"));
        CloseHandle(lock);
        lock = INVALID_HANDLE_VALUE;
        CHECK(ReadAll(primary) == goodBackup);
        CHECK(!project::SetAddress("ammo", 0x5678, "i32", "still blocked for this session"));
        CHECK(ReadAll(primary) == goodBackup);
    } else {
        CHECK(!status.value("saves_blocked", true));
        CHECK(status.value("source", std::string()) == "new");
        CHECK(status["quarantined"].empty());
        CHECK(project::SetAddress("ammo", 0x5678, "i32", "first save"));
        CHECK(json::parse(ReadAll(primary))["addresses"].contains("ammo"));
        CHECK(DamagedCopies(directory).empty());
    }

    if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
    std::error_code ignored;
    fs::remove_all(directory, ignored);
    if (failures) {
        std::cerr << failures << " project store check(s) failed in scenario " << scenario << std::endl;
        return 1;
    }
    std::cout << "project store scenario '" << scenario << "' passed" << std::endl;
    return 0;
}
