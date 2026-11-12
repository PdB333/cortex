// `cortex setup` and `cortex update`: the person's own machine.
//
// setup  registers cortex.exe as an MCP server in the AI clients installed for
//        this user and saves the external engines it finds. It shows the plan
//        first and changes nothing without --yes or a typed "y".
// update checks the published releases, downloads the update for this copy
//        (installer or portable archive), verifies its SHA-256 and applies it
//        after confirmation.

#include "../core/setup/setup.h"
#include "../core/setup/updater.h"
#include "../core/services/user_data.h"
#include "../core/version.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <cstring>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace cortex::setup;

namespace {

fs::path SelfPath() {
#if defined(_WIN32)
    std::wstring buffer(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(size);
    return fs::path(buffer);
#else
    return fs::canonical("/proc/self/exe");
#endif
}

// cortex.exe is a windowed program: from a console, output and the question
// need the parent console.
void UseParentConsole() {
#if defined(_WIN32)
    if (GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) != FILE_TYPE_UNKNOWN) return;
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
    FILE* stream = nullptr;
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
    freopen_s(&stream, "CONIN$", "r", stdin);
    std::fputs("\n", stdout);
#endif
}

bool InteractiveInput() {
#if defined(_WIN32)
    DWORD mode = 0;
    return GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode) != 0;
#else
    return false;
#endif
}

bool Confirm(const char* question) {
    std::printf("%s [y/N] ", question);
    std::fflush(stdout);
    std::string answer;
    if (!std::getline(std::cin, answer)) return false;
    return !answer.empty() && (answer[0] == 'y' || answer[0] == 'Y' || answer[0] == 'o' || answer[0] == 'O');
}

const char* StateText(const std::string& state) {
    if (state == "configured") return "configured";
    if (state == "outdated") return "registered with other settings";
    if (state == "not_configured") return "not configured";
    if (state == "unreadable") return "config not plain JSON: left untouched";
    return "not installed";
}

// The engines file the desktop reads (Artifacts > Engines).
fs::path EnginesFile(const fs::path& exe) {
    return cortex::services::UserDataDirectory(exe.parent_path()) / "analysis-engines.json";
}

bool SaveEngines(const fs::path& exe, const std::map<std::string, std::string>& chosen, std::string& error) {
    json saved = json::object();
    {
        std::ifstream file(EnginesFile(exe));
        if (file) {
            saved = json::parse(file, nullptr, false);
            if (!saved.is_object()) saved = json::object();
        }
    }
    static const std::vector<std::pair<const char*, const char*>> keys = {
        {"ghidra", "ghidra_dir"}, {"ilspycmd", "ilspycmd"}, {"dotnet", "dotnet"}, {"vineflower", "vineflower"},
        {"java", "java_home"}, {"netcoredbg", "netcoredbg"}};
    for (const auto& [id, key] : keys) {
        const auto it = chosen.find(id);
        if (it != chosen.end() && !it->second.empty() && saved.value(key, std::string()).empty()) saved[key] = it->second;
    }
    std::error_code ec;
    fs::create_directories(EnginesFile(exe).parent_path(), ec);
    return cortex::services::WriteFileAtomically(EnginesFile(exe), saved.dump(2), &error);
}

void SetupUsage() {
    std::fputs(
        "cortex setup: register Cortex in your AI clients and find the analysis engines\n\n"
        "  cortex setup                  show what is installed and what would change, then ask\n"
        "  cortex setup --yes            apply without asking\n"
        "  cortex setup --dry-run        show the changes (the exact text) and write nothing\n"
        "  cortex setup --client ID      only this client (repeatable): claude-desktop, claude-code, cursor,\n"
        "                                windsurf, vscode, gemini, codex, lmstudio\n"
        "  cortex setup --allow-writes   let agents change targets (each call still asks mutation_permission)\n"
        "  cortex setup --no-engines     do not pass engine paths to the MCP server or save them\n"
        "  cortex setup --remove         remove Cortex from the clients (with --client: only those)\n"
        "  cortex setup --remove --only-this-copy   only entries that start this cortex.exe (uninstaller)\n"
        "  cortex setup --json           machine-readable report\n\n"
        "Every changed file is backed up beside itself (*.cortex-backup-<date>). A file that is not plain\n"
        "JSON (for example with comments) is never rewritten; the entry to add by hand is printed.\n",
        stdout);
}

void UpdateUsage() {
    std::fputs(
        "cortex update: install the latest Cortex release\n\n"
        "  cortex update --check         say whether a newer release exists (exit code 10 when it does)\n"
        "  cortex update                 download, verify (SHA-256) and install it, after asking\n"
        "  cortex update --yes           without asking\n"
        "  cortex update --json          machine-readable report\n"
        "  cortex update --feed <url|file>  another release feed (a mirror, or a local file for tests)\n\n"
        "An installed copy runs the release's installer; a portable copy is unpacked into a new folder\n"
        "beside the current one, which is kept. Settings live in %LOCALAPPDATA%\\Cortex and carry over.\n",
        stdout);
}

}  // namespace

int CortexSetupMain(int argc, char** argv) {
    UseParentConsole();
    bool yes = false, dryRun = false, asJson = false, remove = false, engines = true, onlyThisCopy = false;
    ClientOptions options;
    std::set<std::string> only;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i] ? argv[i] : "";
        if (a == "--yes" || a == "-y") yes = true;
        else if (a == "--dry-run") dryRun = true;
        else if (a == "--json") asJson = true;
        else if (a == "--remove") remove = true;
        else if (a == "--only-this-copy") onlyThisCopy = true;
        else if (a == "--no-engines") engines = false;
        else if (a == "--allow-writes") options.allowWrites = true;
        else if (a == "--client" && i + 1 < argc) only.insert(argv[++i]);
        else if (a == "--help" || a == "-h") {
            SetupUsage();
            return 0;
        } else {
            std::fprintf(stderr, "cortex setup: unknown argument %s\n", a.c_str());
            SetupUsage();
            return 2;
        }
    }
    for (const auto& id : only) {
        const auto ids = ClientIds();
        if (std::find(ids.begin(), ids.end(), id) == ids.end()) {
            std::fprintf(stderr, "cortex setup: unknown client %s\n", id.c_str());
            return 2;
        }
    }

    const fs::path exe = SelfPath();
    const Environment environment = CurrentEnvironment(exe);
    std::map<std::string, std::string> chosen;
    std::vector<EngineReport> reports;
    if (engines && !remove) {
        reports = DetectEngines(environment);
        for (const auto& report : reports)
            if (!report.found.empty()) chosen[report.id] = report.found.front().path.u8string();
        // ilspycmd on PATH needs no flag; neither does a JDK found through JAVA_HOME.
        options.engines = chosen;
    }
    const auto clients = DetectClients(environment, options);
    std::vector<std::string> targets;
    for (const auto& client : clients) {
        if (!only.empty() && !only.count(client.id)) continue;
        if (!client.installed) continue;
        if (remove ? client.state != "not_configured" : client.state != "configured") targets.push_back(client.id);
    }

    json report = {{"cortex", exe.u8string()}, {"version", cortex::kVersion}, {"allow_writes", options.allowWrites},
                   {"clients", json::array()}, {"engines", json::array()}, {"planned", targets}};
    for (const auto& client : clients) report["clients"].push_back(client.ToJson());
    for (const auto& engine : reports) report["engines"].push_back(engine.ToJson());

    if (!asJson) {
        std::printf("Cortex %s at %s\n\nAI clients\n", cortex::kVersion, exe.u8string().c_str());
        for (const auto& client : clients)
            std::printf("  %-18s %s%s\n", client.name.c_str(), StateText(client.state),
                        client.detail.empty() ? "" : (" (" + client.detail + ")").c_str());
        if (!remove && engines) {
            std::printf("\nAnalysis engines\n");
            for (const auto& engine : reports) {
                if (engine.found.empty()) {
                    std::printf("  %-18s not found  (%s)\n", engine.name.c_str(), engine.download.c_str());
                } else {
                    const auto& best = engine.found.front();
                    std::printf("  %-18s %s%s%s\n", engine.name.c_str(), best.path.u8string().c_str(),
                                best.version.empty() ? "" : "  ", best.version.c_str());
                }
            }
        }
        std::printf("\n");
        if (options.allowWrites)
            std::printf("--allow-writes: agents will be able to change targets when they ask with mutation_permission.\n\n");
    }

    if (targets.empty() && (remove || chosen.empty() || !engines)) {
        if (asJson) std::printf("%s\n", report.dump(2).c_str());
        else std::printf("Nothing to change.\n");
        return 0;
    }

    if (!asJson) {
        std::printf("Planned:\n");
        for (const auto& id : targets)
            std::printf("  %s cortex %s %s\n", remove ? "remove" : "register", remove ? "from" : "in", id.c_str());
        if (!remove && engines && !chosen.empty())
            std::printf("  save the engines found for the desktop (Artifacts > Engines), keeping paths you set\n");
        std::printf("\n");
    }

    if (!dryRun && !yes) {
        if (!InteractiveInput()) {
            if (asJson) std::printf("%s\n", report.dump(2).c_str());
            else std::printf("Run again with --yes to apply, or use the desktop: Help > Set up AI clients and engines.\n");
            return 0;
        }
        if (!Confirm("Apply these changes?")) {
            std::printf("Nothing was changed.\n");
            return 0;
        }
    }

    int failures = 0;
    json changes = json::array();
    for (const auto& id : targets) {
        const ChangeResult result = remove ? RemoveClient(environment, id, dryRun, onlyThisCopy) : ConfigureClient(environment, id, options, dryRun);
        changes.push_back(result.ToJson());
        if (!result.ok) ++failures;
        if (!asJson) {
            std::printf("%s %s\n", result.ok ? "ok  " : "FAIL", result.message.c_str());
            if (!result.backup.empty()) std::printf("     backup: %s\n", result.backup.u8string().c_str());
            if ((dryRun || !result.ok) && !result.preview.empty()) std::printf("%s\n", result.preview.c_str());
        }
    }
    if (!remove && engines && !chosen.empty() && !dryRun) {
        std::string error;
        const bool saved = SaveEngines(exe, chosen, error);
        if (!saved) ++failures;
        if (!asJson) std::printf("%s engines saved to %s%s\n", saved ? "ok  " : "FAIL", EnginesFile(exe).u8string().c_str(),
                                 saved ? "" : (": " + error).c_str());
    }
    report["changes"] = changes;
    report["dry_run"] = dryRun;
    if (asJson) std::printf("%s\n", report.dump(2).c_str());
    else if (!dryRun && failures == 0 && !targets.empty())
        std::printf("\nRestart the AI clients to load Cortex.\n");
    return failures ? 1 : 0;
}

int CortexUpdateMain(int argc, char** argv) {
    UseParentConsole();
    bool check = false, yes = false, asJson = false;
    std::string feed;
    if (const char* env = std::getenv("CORTEX_UPDATE_FEED"); env && *env) feed = env;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i] ? argv[i] : "";
        if (a == "--check") check = true;
        else if (a == "--yes" || a == "-y") yes = true;
        else if (a == "--json") asJson = true;
        else if (a == "--feed" && i + 1 < argc) feed = argv[++i];
        else if (a == "--help" || a == "-h") {
            UpdateUsage();
            return 0;
        } else {
            std::fprintf(stderr, "cortex update: unknown argument %s\n", a.c_str());
            UpdateUsage();
            return 2;
        }
    }
    const fs::path exe = SelfPath();
    const fs::path folder = exe.parent_path();
    const InstallKind kind = DetectInstallKind(folder);
    const UpdateCheck result = CheckForUpdate(feed, cortex::kVersion);
    json report = result.ToJson();
    report["install"] = InstallKindName(kind);
    if (!result.ok) {
        if (asJson) std::printf("%s\n", report.dump(2).c_str());
        else std::fprintf(stderr, "cortex update: the check failed: %s\n", result.error.c_str());
        return 1;
    }
    if (!asJson) {
        std::printf("Cortex %s (%s), latest release %s\n", cortex::kVersion, InstallKindName(kind), result.latest.version.c_str());
        if (!result.available) std::printf("Cortex is up to date.\n");
        else std::printf("Cortex %s is available: %s\n", result.latest.version.c_str(), result.latest.page.c_str());
    }
    if (!result.available || check) {
        if (asJson) std::printf("%s\n", report.dump(2).c_str());
        return result.available ? 10 : 0;
    }
    if (!yes) {
        if (!InteractiveInput()) {
            if (!asJson) std::printf("Run again with --yes to install it, or use the desktop: Help > Check for updates.\n");
            else std::printf("%s\n", report.dump(2).c_str());
            return 10;
        }
        if (!Confirm(kind == InstallKind::Installed ? "Download, verify and run the installer?"
                                                    : "Download, verify and unpack it beside this folder?")) {
            std::printf("Nothing was changed.\n");
            return 10;
        }
    }
    const fs::path work = cortex::services::UserDataDirectory(folder) / "updates";
    const PreparedUpdate prepared = PrepareUpdate(result.latest, kind, work, [asJson](uint64_t done, uint64_t total) {
        if (asJson || total == 0) return;
        std::printf("\r  %llu / %llu KiB", static_cast<unsigned long long>(done / 1024),
                    static_cast<unsigned long long>(total / 1024));
        std::fflush(stdout);
    });
    report["prepared"] = prepared.ToJson();
    if (!asJson) std::printf("\n");
    if (!prepared.ok) {
        if (asJson) std::printf("%s\n", report.dump(2).c_str());
        else std::fprintf(stderr, "cortex update: %s\n", prepared.error.c_str());
        return 1;
    }
    if (!asJson) std::printf("Verified: SHA-256 %s\n", prepared.sha256.c_str());
    if (kind == InstallKind::Installed) {
        const auto blocking = FindBlockingProcesses(folder);
        if (!blocking.empty()) {
            json rows = json::array();
            for (const auto& item : blocking) rows.push_back({{"pid", item.pid}, {"name", item.name}, {"reason", item.reason}});
            report["blocking"] = rows;
            if (asJson) {
                std::printf("%s\n", report.dump(2).c_str());
            } else {
                std::fprintf(stderr, "cortex update: close these first (the installer cannot replace files in use):\n");
                for (const auto& item : blocking)
                    std::fprintf(stderr, "  %u %s: %s\n", item.pid, item.name.c_str(), item.reason.c_str());
                std::fprintf(stderr, "The verified installer is kept at %s\n", prepared.file.u8string().c_str());
            }
            return 1;
        }
    }
    const ApplyResult applied = ApplyUpdate(prepared, folder, result.latest.version);
    report["applied"] = {{"ok", applied.ok}, {"error", applied.error}, {"new_folder", applied.newFolder.u8string()}};
    if (asJson) std::printf("%s\n", report.dump(2).c_str());
    else if (!applied.ok) std::fprintf(stderr, "cortex update: %s\n", applied.error.c_str());
    else if (kind == InstallKind::Installed) std::printf("The installer is running.\n");
    else std::printf("Cortex %s is in %s (this folder is unchanged).\n", result.latest.version.c_str(),
                     applied.newFolder.u8string().c_str());
    return applied.ok ? 0 : 1;
}
