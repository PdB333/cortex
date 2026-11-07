#include "setup.h"

#include "../services/user_data.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>

namespace cortex::setup {
namespace {

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool StartsWithNoCase(const std::string& text, const std::string& prefix) {
    return Lower(text).rfind(Lower(prefix), 0) == 0;
}

bool IsFile(const fs::path& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

bool IsDir(const fs::path& path) {
    std::error_code ec;
    return fs::is_directory(path, ec);
}

std::string ReadText(const fs::path& path, bool& ok) {
    std::ifstream file(path, std::ios::binary);
    ok = static_cast<bool>(file);
    if (!ok) return {};
    std::ostringstream out;
    out << file.rdbuf();
    return out.str();
}

std::string Utf8(const fs::path& path) { return path.u8string(); }

// Immediate subdirectories, sorted by name, newest-looking last; bounded.
std::vector<fs::path> Children(const fs::path& directory, size_t limit = 400) {
    std::vector<fs::path> result;
    std::error_code ec;
    if (!IsDir(directory)) return result;
    for (fs::directory_iterator it(directory, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end && result.size() < limit; it.increment(ec)) {
        std::error_code typeError;
        if (it->is_directory(typeError) && !it->is_symlink(typeError)) result.push_back(it->path());
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<fs::path> FilesIn(const fs::path& directory, size_t limit = 400) {
    std::vector<fs::path> result;
    std::error_code ec;
    if (!IsDir(directory)) return result;
    for (fs::directory_iterator it(directory, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end && result.size() < limit; it.increment(ec)) {
        std::error_code typeError;
        if (it->is_regular_file(typeError)) result.push_back(it->path());
    }
    std::sort(result.begin(), result.end());
    return result;
}

#if defined(_WIN32)
const char* kExe = ".exe";
#else
const char* kExe = "";
#endif

// ------------------------------------------------------------------ engine checks

std::string PropertyValue(const std::string& text, const std::string& key) {
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind(key, 0) != 0) continue;
        std::string value = line.substr(key.size());
        while (!value.empty() && (value.front() == ' ' || value.front() == '=' || value.front() == '"')) value.erase(0, 1);
        while (!value.empty() && (value.back() == ' ' || value.back() == '"')) value.pop_back();
        return value;
    }
    return {};
}

bool CheckGhidra(const fs::path& folder, std::string& version) {
    const bool launcher = IsFile(folder / "support" / "analyzeHeadless.bat") || IsFile(folder / "support" / "analyzeHeadless");
    const fs::path properties = folder / "Ghidra" / "application.properties";
    if (!launcher || !IsFile(properties)) return false;
    bool ok = false;
    version = PropertyValue(ReadText(properties, ok), "application.version");
    return true;
}

bool CheckJdk(const fs::path& folder, std::string& version) {
    if (!IsFile(folder / "bin" / (std::string("java") + kExe))) return false;
    // A JDK (not a JRE) also has javac; Ghidra and Vineflower need 17 or 21+.
    bool ok = false;
    version = PropertyValue(ReadText(folder / "release", ok), "JAVA_VERSION");
    return true;
}

bool CheckFileNamed(const fs::path& file, const std::string& stem) {
    return IsFile(file) && StartsWithNoCase(file.filename().u8string(), stem);
}

bool CheckVineflower(const fs::path& jar, std::string& version) {
    const std::string name = Lower(jar.filename().u8string());
    if (!IsFile(jar) || name.rfind("vineflower", 0) != 0 || name.size() < 4 || name.substr(name.size() - 4) != ".jar")
        return false;
    // vineflower-1.11.1.jar
    const size_t dash = name.find('-');
    if (dash != std::string::npos) version = name.substr(dash + 1, name.size() - dash - 5);
    return true;
}

}  // namespace

bool ValidateEngine(const std::string& id, const fs::path& path, std::string& version) {
    version.clear();
    if (id == "ghidra") return CheckGhidra(path, version);
    if (id == "java") return CheckJdk(path, version);
    if (id == "vineflower") return CheckVineflower(path, version);
    if (id == "ilspycmd") return CheckFileNamed(path, "ilspycmd");
    if (id == "dotnet") return CheckFileNamed(path, "dotnet");
    if (id == "netcoredbg") return CheckFileNamed(path, "netcoredbg");
    return false;
}

json EngineReport::ToJson() const {
    json rows = json::array();
    for (const auto& item : found)
        rows.push_back({{"path", Utf8(item.path)}, {"version", item.version}, {"source", item.source}});
    return {{"id", id}, {"name", name}, {"purpose", purpose}, {"download", download}, {"found", rows}};
}

Environment CurrentEnvironment(const fs::path& cortexExe) {
    Environment environment;
    environment.cortexExe = cortexExe;
    auto variable = [](const char* name) -> std::string {
#if defined(_WIN32)
        std::wstring wide(name, name + std::strlen(name));
        const wchar_t* value = _wgetenv(wide.c_str());
        return value ? fs::path(value).u8string() : std::string();
#else
        const char* value = std::getenv(name);
        return value ? value : "";
#endif
    };
#if defined(_WIN32)
    environment.home = fs::u8path(variable("USERPROFILE"));
    environment.appData = fs::u8path(variable("APPDATA"));
    environment.localAppData = fs::u8path(variable("LOCALAPPDATA"));
    const char separator = ';';
#else
    environment.home = fs::u8path(variable("HOME"));
    environment.appData = environment.home / ".config";
    environment.localAppData = environment.home / ".local" / "share";
    const char separator = ':';
#endif
    for (const char* name : {"JAVA_HOME", "GHIDRA_INSTALL_DIR", "CORTEX_ILSPYCMD", "CORTEX_VINEFLOWER",
                             "CORTEX_NETCOREDBG", "DOTNET_HOST_PATH", "ProgramFiles", "ProgramFiles(x86)", "SystemDrive"}) {
        const std::string value = variable(name);
        if (!value.empty()) environment.variables[name] = value;
    }
    std::stringstream path(variable("PATH"));
    std::string entry;
    while (std::getline(path, entry, separator))
        if (!entry.empty()) environment.pathDirs.push_back(fs::u8path(entry));

    auto addRoot = [&environment](const fs::path& root) {
        if (!root.empty()) environment.searchRoots.push_back(root);
    };
    addRoot(environment.home / "Downloads");
    addRoot(environment.home / "Desktop");
    addRoot(environment.home / "tools");
    addRoot(environment.home);
    addRoot(environment.localAppData / "Programs");
    if (environment.variables.count("ProgramFiles")) addRoot(fs::u8path(environment.variables["ProgramFiles"]));
    if (environment.variables.count("ProgramFiles(x86)")) addRoot(fs::u8path(environment.variables["ProgramFiles(x86)"]));
    const std::string drive = environment.variables.count("SystemDrive") ? environment.variables["SystemDrive"] : "";
    if (!drive.empty()) {
        addRoot(fs::u8path(drive + "\\"));
        addRoot(fs::u8path(drive + "\\tools"));
    }
    return environment;
}

std::vector<EngineReport> DetectEngines(const Environment& environment) {
    std::vector<EngineReport> reports;
    auto variable = [&environment](const char* name) {
        const auto it = environment.variables.find(name);
        return it == environment.variables.end() ? std::string() : it->second;
    };
    auto add = [](EngineReport& report, const fs::path& path, const std::string& version, const std::string& source) {
        for (const auto& item : report.found) {
            std::error_code ec;
            if (item.path == path || fs::equivalent(item.path, path, ec)) return;
        }
        report.found.push_back({path, version, source});
    };
    auto onPath = [&](EngineReport& report, const std::string& file) {
        for (const auto& dir : environment.pathDirs) {
            std::string version;
            const fs::path candidate = dir / (file + kExe);
            if (ValidateEngine(report.id, candidate, version)) add(report, candidate, version, "PATH");
        }
    };
    // Folders one or two levels under each root whose name starts with `prefix`.
    auto searchFolders = [&](const std::string& prefix, const std::function<void(const fs::path&)>& visit) {
        for (const auto& root : environment.searchRoots) {
            for (const auto& child : Children(root)) {
                const std::string name = child.filename().u8string();
                if (!StartsWithNoCase(name, prefix)) continue;
                visit(child);
                for (const auto& grandchild : Children(child, 50)) visit(grandchild);
            }
        }
    };

    {
        EngineReport report{"ghidra", "Ghidra", "Native decompiler for PE and ELF (Engines tab, cortex_analysis_*)",
                            "https://github.com/NationalSecurityAgency/ghidra/releases", {}};
        std::string version;
        if (const std::string env = variable("GHIDRA_INSTALL_DIR"); !env.empty() && CheckGhidra(fs::u8path(env), version))
            add(report, fs::u8path(env), version, "GHIDRA_INSTALL_DIR");
        searchFolders("ghidra", [&](const fs::path& folder) {
            std::string found;
            if (CheckGhidra(folder, found)) add(report, folder, found, "search");
        });
        reports.push_back(std::move(report));
    }
    {
        EngineReport report{"java", "JDK 21", "Runs Ghidra and Vineflower", "https://adoptium.net/temurin/releases/?version=21", {}};
        std::string version;
        if (const std::string env = variable("JAVA_HOME"); !env.empty() && CheckJdk(fs::u8path(env), version))
            add(report, fs::u8path(env), version, "JAVA_HOME");
        for (const auto& dir : environment.pathDirs) {
            if (!IsFile(dir / (std::string("java") + kExe))) continue;
            const fs::path home = dir.parent_path();
            if (CheckJdk(home, version)) add(report, home, version, "PATH");
        }
        for (const auto& root : environment.searchRoots) {
            for (const char* vendor : {"Java", "Eclipse Adoptium", "Microsoft", "Zulu", "BellSoft", "Amazon Corretto", "OpenJDK"}) {
                for (const auto& jdk : Children(root / vendor, 50)) {
                    std::string found;
                    if (CheckJdk(jdk, found)) add(report, jdk, found, "search");
                }
            }
        }
        searchFolders("jdk", [&](const fs::path& folder) {
            std::string found;
            if (CheckJdk(folder, found)) add(report, folder, found, "search");
        });
        // Newest first: Ghidra 11 needs 21 or later.
        std::stable_sort(report.found.begin(), report.found.end(), [](const EngineFound& a, const EngineFound& b) {
            return std::atoi(a.version.c_str()) > std::atoi(b.version.c_str());
        });
        reports.push_back(std::move(report));
    }
    {
        EngineReport report{"ilspycmd", "ILSpy (ilspycmd)", "Decompiles .NET assemblies to C#",
                            "dotnet tool install -g ilspycmd", {}};
        std::string version;
        if (const std::string env = variable("CORTEX_ILSPYCMD"); !env.empty() && IsFile(fs::u8path(env)))
            add(report, fs::u8path(env), "", "CORTEX_ILSPYCMD");
        onPath(report, "ilspycmd");
        const fs::path tool = environment.home / ".dotnet" / "tools" / (std::string("ilspycmd") + kExe);
        if (ValidateEngine("ilspycmd", tool, version)) add(report, tool, "", "dotnet tool");
        reports.push_back(std::move(report));
    }
    {
        EngineReport report{"dotnet", ".NET SDK", "Installs and runs ilspycmd", "https://dotnet.microsoft.com/download", {}};
        if (const std::string env = variable("DOTNET_HOST_PATH"); !env.empty() && IsFile(fs::u8path(env)))
            add(report, fs::u8path(env), "", "DOTNET_HOST_PATH");
        onPath(report, "dotnet");
        for (const auto& root : environment.searchRoots) {
            std::string version;
            const fs::path candidate = root / "dotnet" / (std::string("dotnet") + kExe);
            if (ValidateEngine("dotnet", candidate, version)) add(report, candidate, "", "search");
        }
        reports.push_back(std::move(report));
    }
    {
        EngineReport report{"vineflower", "Vineflower", "Decompiles Java .class and .jar files",
                            "https://github.com/Vineflower/vineflower/releases", {}};
        std::string version;
        if (const std::string env = variable("CORTEX_VINEFLOWER"); !env.empty() && CheckVineflower(fs::u8path(env), version))
            add(report, fs::u8path(env), version, "CORTEX_VINEFLOWER");
        for (const auto& root : environment.searchRoots) {
            for (const auto& file : FilesIn(root)) {
                std::string found;
                if (CheckVineflower(file, found)) add(report, file, found, "search");
            }
        }
        searchFolders("vineflower", [&](const fs::path& folder) {
            for (const auto& file : FilesIn(folder, 50)) {
                std::string found;
                if (CheckVineflower(file, found)) add(report, file, found, "search");
            }
        });
        reports.push_back(std::move(report));
    }
    {
        EngineReport report{"netcoredbg", "netcoredbg", ".NET live debugging (log breakpoints)",
                            "https://github.com/Samsung/netcoredbg/releases", {}};
        std::string version;
        if (const std::string env = variable("CORTEX_NETCOREDBG"); !env.empty() && IsFile(fs::u8path(env)))
            add(report, fs::u8path(env), "", "CORTEX_NETCOREDBG");
        onPath(report, "netcoredbg");
        searchFolders("netcoredbg", [&](const fs::path& folder) {
            const fs::path candidate = folder / (std::string("netcoredbg") + kExe);
            if (ValidateEngine("netcoredbg", candidate, version)) add(report, candidate, "", "search");
        });
        reports.push_back(std::move(report));
    }
    return reports;
}

// ------------------------------------------------------------------ AI clients

namespace {

enum class Format { McpServers, VsCodeServers, CodexToml };

struct ClientSpec {
    const char* id;
    const char* name;
    Format format;
    std::function<fs::path(const Environment&)> folder;  // exists when the client is installed
    std::function<fs::path(const Environment&)> config;
};

const std::vector<ClientSpec>& Specs() {
    static const std::vector<ClientSpec> specs = {
        {"claude-desktop", "Claude Desktop", Format::McpServers,
         [](const Environment& e) { return e.appData / "Claude"; },
         [](const Environment& e) { return e.appData / "Claude" / "claude_desktop_config.json"; }},
        {"claude-code", "Claude Code", Format::McpServers,
         [](const Environment& e) { return e.home / ".claude"; },
         [](const Environment& e) { return e.home / ".claude.json"; }},
        {"cursor", "Cursor", Format::McpServers,
         [](const Environment& e) { return e.home / ".cursor"; },
         [](const Environment& e) { return e.home / ".cursor" / "mcp.json"; }},
        {"windsurf", "Windsurf", Format::McpServers,
         [](const Environment& e) { return e.home / ".codeium" / "windsurf"; },
         [](const Environment& e) { return e.home / ".codeium" / "windsurf" / "mcp_config.json"; }},
        {"vscode", "VS Code (Copilot)", Format::VsCodeServers,
         [](const Environment& e) { return e.appData / "Code" / "User"; },
         [](const Environment& e) { return e.appData / "Code" / "User" / "mcp.json"; }},
        {"gemini", "Gemini CLI", Format::McpServers,
         [](const Environment& e) { return e.home / ".gemini"; },
         [](const Environment& e) { return e.home / ".gemini" / "settings.json"; }},
        {"codex", "Codex", Format::CodexToml,
         [](const Environment& e) { return e.home / ".codex"; },
         [](const Environment& e) { return e.home / ".codex" / "config.toml"; }},
        {"lmstudio", "LM Studio", Format::McpServers,
         [](const Environment& e) { return e.home / ".lmstudio"; },
         [](const Environment& e) { return e.home / ".lmstudio" / "mcp.json"; }},
    };
    return specs;
}

const ClientSpec* FindSpec(const std::string& id) {
    for (const auto& spec : Specs())
        if (id == spec.id) return &spec;
    return nullptr;
}

std::vector<std::string> ServerArgs(const ClientOptions& options) {
    std::vector<std::string> args{"mcp"};
    if (options.allowWrites) args.push_back("--allow-writes");
    static const std::vector<std::pair<const char*, const char*>> flags = {
        {"ghidra", "--ghidra-dir"}, {"ilspycmd", "--ilspycmd"}, {"dotnet", "--dotnet"},
        {"vineflower", "--vineflower"}, {"netcoredbg", "--netcoredbg"}};
    for (const auto& [id, flag] : flags) {
        const auto it = options.engines.find(id);
        if (it != options.engines.end() && !it->second.empty()) {
            args.push_back(flag);
            args.push_back(it->second);
        }
    }
    return args;
}

json ServerEnv(const ClientOptions& options) {
    json env = json::object();
    const auto java = options.engines.find("java");
    if (java != options.engines.end() && !java->second.empty()) env["JAVA_HOME"] = java->second;
    return env;
}

std::string TomlString(const std::string& text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

std::string CodexSection(const Environment& environment, const ClientOptions& options) {
    std::ostringstream out;
    out << "[mcp_servers.cortex]\n";
    out << "command = " << TomlString(Utf8(environment.cortexExe)) << "\n";
    out << "args = [";
    const auto args = ServerArgs(options);
    for (size_t i = 0; i < args.size(); ++i) out << (i ? ", " : "") << TomlString(args[i]);
    out << "]\n";
    const json env = ServerEnv(options);
    if (!env.empty()) {
        out << "\n[mcp_servers.cortex.env]\n";
        for (const auto& [key, value] : env.items()) out << key << " = " << TomlString(value.get<std::string>()) << "\n";
    }
    return out.str();
}

// Splits a TOML text around the cortex server's sections: [before, section, after].
bool SplitCodex(const std::string& text, std::string& before, std::string& section, std::string& after) {
    std::istringstream lines(text);
    std::string line;
    enum { Before, In, After } state = Before;
    bool found = false;
    while (std::getline(lines, line)) {
        std::string trimmed = line;
        while (!trimmed.empty() && (trimmed.back() == '\r' || trimmed.back() == ' ')) trimmed.pop_back();
        size_t first = trimmed.find_first_not_of(" \t");
        trimmed = first == std::string::npos ? std::string() : trimmed.substr(first);
        const bool header = !trimmed.empty() && trimmed[0] == '[';
        const bool ours = header && (trimmed == "[mcp_servers.cortex]" || trimmed.rfind("[mcp_servers.cortex.", 0) == 0);
        if (ours) {
            state = In;
            found = true;
        } else if (header && state == In) {
            state = After;
        }
        std::string& target = state == Before ? before : state == In ? section : after;
        target += line + "\n";
    }
    return found;
}

std::string Timestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char buffer[32] = {};
    std::strftime(buffer, sizeof(buffer), "%Y%m%d-%H%M%S", &local);
    return buffer;
}

struct Loaded {
    bool exists = false;
    bool readable = true;
    std::string text;
    json document = json::object();
};

Loaded Load(const ClientSpec& spec, const fs::path& path) {
    Loaded loaded;
    loaded.exists = IsFile(path);
    if (!loaded.exists) return loaded;
    bool ok = false;
    loaded.text = ReadText(path, ok);
    if (!ok) {
        loaded.readable = false;
        return loaded;
    }
    if (spec.format == Format::CodexToml) return loaded;
    const std::string trimmed = loaded.text.find_first_not_of(" \t\r\n") == std::string::npos ? "" : loaded.text;
    if (trimmed.empty()) return loaded;
    loaded.document = json::parse(loaded.text, nullptr, false);
    if (loaded.document.is_discarded() || !loaded.document.is_object()) loaded.readable = false;
    return loaded;
}

const char* ServersKey(const ClientSpec& spec) { return spec.format == Format::VsCodeServers ? "servers" : "mcpServers"; }

json Entry(const ClientSpec& spec, const Environment& environment, const ClientOptions& options) {
    json entry = ServerEntry(environment, options);
    if (spec.format == Format::VsCodeServers) entry["type"] = "stdio";
    return entry;
}

ChangeResult Write(const fs::path& path, const std::string& text, const Loaded& loaded, bool dryRun, ChangeResult result) {
    result.preview = text;
    if (dryRun) {
        result.ok = true;
        return result;
    }
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (loaded.exists) {
        // A name no earlier backup has (two changes can fall in one second).
        const std::string stamp = Timestamp();
        fs::path backup = path;
        backup += ".cortex-backup-" + stamp;
        for (int i = 2; fs::exists(backup, ec) && i < 1000; ++i) {
            backup = path;
            backup += ".cortex-backup-" + stamp + "-" + std::to_string(i);
        }
        fs::copy_file(path, backup, fs::copy_options::none, ec);
        if (ec) {
            result.message = "could not back up " + Utf8(path) + ": " + ec.message() + "; nothing was changed";
            return result;
        }
        result.backup = backup;
    }
    std::string error;
    if (!services::WriteFileAtomically(path, text, &error)) {
        result.message = "could not write " + Utf8(path) + ": " + error;
        return result;
    }
    result.ok = true;
    return result;
}

}  // namespace

std::vector<std::string> ClientIds() {
    std::vector<std::string> ids;
    for (const auto& spec : Specs()) ids.push_back(spec.id);
    return ids;
}

json ServerEntry(const Environment& environment, const ClientOptions& options) {
    json entry = {{"command", Utf8(environment.cortexExe)}, {"args", ServerArgs(options)}};
    const json env = ServerEnv(options);
    if (!env.empty()) entry["env"] = env;
    return entry;
}

json ClientStatus::ToJson() const {
    return {{"id", id}, {"name", name}, {"config", Utf8(configPath)}, {"installed", installed}, {"state", state}, {"detail", detail}};
}

json ChangeResult::ToJson() const {
    json out = {{"id", id}, {"ok", ok}, {"changed", changed}, {"message", message}};
    if (!backup.empty()) out["backup"] = Utf8(backup);
    return out;
}

std::vector<ClientStatus> DetectClients(const Environment& environment, const ClientOptions& options) {
    std::vector<ClientStatus> clients;
    for (const auto& spec : Specs()) {
        ClientStatus status;
        status.id = spec.id;
        status.name = spec.name;
        status.configPath = spec.config(environment);
        status.installed = IsDir(spec.folder(environment)) || IsFile(status.configPath);
        if (!status.installed) {
            status.state = "not_installed";
            clients.push_back(status);
            continue;
        }
        const Loaded loaded = Load(spec, status.configPath);
        if (!loaded.readable) {
            status.state = "unreadable";
            status.detail = "the file is not plain JSON (comments?) and is left untouched; add the entry by hand";
        } else if (spec.format == Format::CodexToml) {
            std::string before, section, after;
            if (!SplitCodex(loaded.text, before, section, after)) status.state = "not_configured";
            else status.state = section == CodexSection(environment, options) ? "configured" : "outdated";
        } else {
            const char* key = ServersKey(spec);
            const json servers = loaded.document.value(key, json::object());
            if (!servers.is_object() || !servers.contains("cortex")) status.state = "not_configured";
            else status.state = servers["cortex"] == Entry(spec, environment, options) ? "configured" : "outdated";
            if (status.state == "outdated" && servers["cortex"].is_object())
                status.detail = "cortex is registered with other settings (another path or options)";
        }
        clients.push_back(status);
    }
    return clients;
}

ChangeResult ConfigureClient(const Environment& environment, const std::string& id, const ClientOptions& options,
                             bool dryRun) {
    ChangeResult result;
    result.id = id;
    const ClientSpec* spec = FindSpec(id);
    if (!spec) {
        result.message = "unknown client " + id;
        return result;
    }
    const fs::path path = spec->config(environment);
    const Loaded loaded = Load(*spec, path);
    if (!loaded.readable) {
        result.message = Utf8(path) + " could not be parsed as plain JSON and was left untouched; add the entry by hand";
        result.preview = json{{ServersKey(*spec), {{"cortex", Entry(*spec, environment, options)}}}}.dump(2);
        return result;
    }
    std::string text;
    if (spec->format == Format::CodexToml) {
        std::string before, section, after;
        const bool found = SplitCodex(loaded.text, before, section, after);
        const std::string wanted = CodexSection(environment, options);
        if (found && section == wanted) {
            result.ok = true;
            result.message = "already configured";
            return result;
        }
        if (found) {
            text = before + wanted + (after.empty() ? "" : "\n" + after);
        } else {
            text = loaded.text;
            if (!text.empty() && text.back() != '\n') text += "\n";
            if (!text.empty()) text += "\n";
            text += wanted;
        }
    } else {
        json document = loaded.document;
        const char* key = ServersKey(*spec);
        if (!document.contains(key) || !document[key].is_object()) document[key] = json::object();
        const json entry = Entry(*spec, environment, options);
        if (document[key].contains("cortex") && document[key]["cortex"] == entry) {
            result.ok = true;
            result.message = "already configured";
            return result;
        }
        document[key]["cortex"] = entry;
        text = document.dump(2) + "\n";
    }
    result.changed = true;
    result.message = std::string(dryRun ? "would register" : "registered") + " cortex in " + Utf8(path);
    return Write(path, text, loaded, dryRun, result);
}

ChangeResult RemoveClient(const Environment& environment, const std::string& id, bool dryRun, bool onlyThisCopy) {
    ChangeResult result;
    result.id = id;
    const ClientSpec* spec = FindSpec(id);
    if (!spec) {
        result.message = "unknown client " + id;
        return result;
    }
    const fs::path path = spec->config(environment);
    const Loaded loaded = Load(*spec, path);
    if (!loaded.exists) {
        result.ok = true;
        result.message = "not configured";
        return result;
    }
    if (!loaded.readable) {
        result.message = Utf8(path) + " could not be parsed and was left untouched";
        return result;
    }
    std::string text;
    if (spec->format == Format::CodexToml) {
        std::string before, section, after;
        if (!SplitCodex(loaded.text, before, section, after)) {
            result.ok = true;
            result.message = "not configured";
            return result;
        }
        if (onlyThisCopy && section.find(TomlString(Utf8(environment.cortexExe))) == std::string::npos) {
            result.ok = true;
            result.message = "registered for another copy of Cortex; kept";
            return result;
        }
        text = before + after;
    } else {
        json document = loaded.document;
        const char* key = ServersKey(*spec);
        if (!document.contains(key) || !document[key].is_object() || !document[key].contains("cortex")) {
            result.ok = true;
            result.message = "not configured";
            return result;
        }
        const json& existing = document[key]["cortex"];
        if (onlyThisCopy && !(existing.is_object() && existing.value("command", std::string()) == Utf8(environment.cortexExe))) {
            result.ok = true;
            result.message = "registered for another copy of Cortex; kept";
            return result;
        }
        document[key].erase("cortex");
        text = document.dump(2) + "\n";
    }
    result.changed = true;
    result.message = std::string(dryRun ? "would remove" : "removed") + " cortex from " + Utf8(path);
    return Write(path, text, loaded, dryRun, result);
}

}  // namespace cortex::setup
