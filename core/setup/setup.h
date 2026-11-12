#pragma once

// Setup of the person's own machine: registers cortex.exe as an MCP server in
// the AI clients installed for this user, and finds the external engines
// (Ghidra, a JDK, ILSpy, Vineflower, netcoredbg) where they usually live.
//
// Nothing here runs on an agent's request: `cortex.exe setup` and the
// desktop's Setup dialog call it for the person, show what would change, and
// write only after the person confirms. Every file that is changed is backed
// up first. A configuration file that cannot be parsed is never rewritten.
//
// The code is plain std::filesystem with the folders passed in, so tests run
// it on a fake home directory.

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace cortex::setup {

namespace fs = std::filesystem;
using json = nlohmann::json;

// Where to look. CurrentEnvironment() fills it from the real user profile.
struct Environment {
    fs::path home;          // %USERPROFILE%
    fs::path appData;       // %APPDATA%
    fs::path localAppData;  // %LOCALAPPDATA%
    fs::path cortexExe;     // the cortex.exe that clients will start
    // Engine search: PATH directories and folders where people unpack tools.
    std::vector<fs::path> pathDirs;
    std::vector<fs::path> searchRoots;
    std::map<std::string, std::string> variables;  // JAVA_HOME, GHIDRA_INSTALL_DIR, ...
};

Environment CurrentEnvironment(const fs::path& cortexExe);

// ------------------------------------------------------------------ engines

struct EngineFound {
    fs::path path;        // the folder (Ghidra, JDK) or the file (ilspycmd, jar, netcoredbg)
    std::string version;  // as read from the installation, or empty
    std::string source;   // "GHIDRA_INSTALL_DIR", "PATH", "search", ...
};

struct EngineReport {
    std::string id;    // ghidra, java, ilspycmd, dotnet, vineflower, netcoredbg
    std::string name;  // shown to the person
    std::string purpose;
    std::string download;  // where to get it (official page)
    std::vector<EngineFound> found;  // best first
    json ToJson() const;
};

std::vector<EngineReport> DetectEngines(const Environment& environment);

// Checks a path the person typed: true with the version when it is that engine.
bool ValidateEngine(const std::string& id, const fs::path& path, std::string& version);

// ------------------------------------------------------------------ AI clients

struct ClientOptions {
    bool allowWrites = false;  // adds --allow-writes: the person's decision
    // Engine paths passed to `cortex.exe mcp` (id -> path), JAVA_HOME as env.
    std::map<std::string, std::string> engines;
};

struct ClientStatus {
    std::string id;     // claude-desktop, claude-code, cursor, windsurf, vscode, gemini, codex, lmstudio
    std::string name;
    fs::path configPath;
    bool installed = false;  // the client's folder exists for this user
    // not_installed | not_configured | configured | outdated | unreadable
    std::string state;
    std::string detail;
    json ToJson() const;
};

std::vector<std::string> ClientIds();
std::vector<ClientStatus> DetectClients(const Environment& environment, const ClientOptions& options);

struct ChangeResult {
    std::string id;
    bool ok = false;
    bool changed = false;
    std::string message;
    fs::path backup;   // the copy made before writing, when there was a file
    std::string preview;  // the configuration text written (or that would be)
    json ToJson() const;
};

// Adds or updates the "cortex" server of one client. dryRun computes the
// change without writing.
ChangeResult ConfigureClient(const Environment& environment, const std::string& id,
                             const ClientOptions& options, bool dryRun);
// Removes the "cortex" server of one client. With onlyThisCopy, an entry
// that starts another cortex.exe (a portable copy elsewhere) is kept: the
// uninstaller uses it.
ChangeResult RemoveClient(const Environment& environment, const std::string& id, bool dryRun,
                          bool onlyThisCopy = false);

// The server entry as JSON (the "cortex" value of mcpServers), for display.
json ServerEntry(const Environment& environment, const ClientOptions& options);

}  // namespace cortex::setup
