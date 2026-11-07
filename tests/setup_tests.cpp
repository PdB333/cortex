// Setup and updater (core/setup): AI client registration on a fake home
// folder, engine detection on fake installations, and the update flow from a
// local feed (checksums, archives, version order). Nothing touches the real
// profile or the network.
#include "setup/setup.h"
#include "setup/updater.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace cortex::setup;

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", message.c_str());
    if (!condition) ++failures;
}

void WriteFile(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string ReadFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

int CountBackups(const fs::path& folder) {
    int count = 0;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(folder, ec))
        if (entry.path().filename().u8string().find(".cortex-backup-") != std::string::npos) ++count;
    return count;
}

const ClientStatus* Find(const std::vector<ClientStatus>& clients, const std::string& id) {
    for (const auto& client : clients)
        if (client.id == id) return &client;
    return nullptr;
}

#if defined(_WIN32)
const std::string kExe = ".exe";
#else
const std::string kExe = "";
#endif

// A stored (uncompressed) ZIP with the given entries, built by hand.
std::string StoredZip(const std::vector<std::pair<std::string, std::string>>& entries) {
    auto crc32 = [](const std::string& data) {
        uint32_t crc = 0xFFFFFFFFu;
        for (unsigned char c : data) {
            crc ^= c;
            for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
        return ~crc;
    };
    auto u16 = [](std::string& out, uint16_t v) { out += char(v & 0xFF); out += char(v >> 8); };
    auto u32 = [](std::string& out, uint32_t v) { for (int i = 0; i < 4; ++i) out += char((v >> (8 * i)) & 0xFF); };
    std::string body, central;
    for (const auto& [name, data] : entries) {
        const uint32_t offset = static_cast<uint32_t>(body.size());
        const uint32_t crc = crc32(data);
        u32(body, 0x04034b50); u16(body, 20); u16(body, 0); u16(body, 0); u16(body, 0); u16(body, 0);
        u32(body, crc); u32(body, static_cast<uint32_t>(data.size())); u32(body, static_cast<uint32_t>(data.size()));
        u16(body, static_cast<uint16_t>(name.size())); u16(body, 0);
        body += name + data;
        u32(central, 0x02014b50); u16(central, 20); u16(central, 20); u16(central, 0); u16(central, 0); u16(central, 0);
        u16(central, 0); u32(central, crc); u32(central, static_cast<uint32_t>(data.size()));
        u32(central, static_cast<uint32_t>(data.size())); u16(central, static_cast<uint16_t>(name.size()));
        u16(central, 0); u16(central, 0); u16(central, 0); u16(central, 0); u32(central, 0); u32(central, offset);
        central += name;
    }
    std::string out = body + central;
    u32(out, 0x06054b50); u16(out, 0); u16(out, 0);
    u16(out, static_cast<uint16_t>(entries.size())); u16(out, static_cast<uint16_t>(entries.size()));
    u32(out, static_cast<uint32_t>(central.size())); u32(out, static_cast<uint32_t>(body.size())); u16(out, 0);
    return out;
}

}  // namespace

#include "artifacts/sha256.h"

static std::string HexOf(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    const std::string data = out.str();
    cortex::artifacts::Sha256 hash;
    hash.Update(data.data(), data.size());
    return cortex::artifacts::Sha256::Hex(hash.Final());
}

int main() {
    const fs::path base = fs::temp_directory_path() / ("cortex-setup-tests-" + std::to_string(std::rand()));
    fs::remove_all(base);
    Environment environment;
    environment.home = base / "home";
    environment.appData = base / "appdata";
    environment.localAppData = base / "local";
    environment.cortexExe = base / "Cortex" / ("cortex" + kExe);
    environment.searchRoots = {environment.home / "Downloads", base / "ProgramFiles"};
    environment.pathDirs = {base / "bin"};
    fs::create_directories(environment.home);
    WriteFile(environment.cortexExe, "exe");

    // ------------------------------------------------------------ clients
    fs::create_directories(environment.home / ".cursor");
    WriteFile(environment.home / ".codex" / "config.toml",
              "model = \"o3\"\n\n[mcp_servers.other]\ncommand = \"other.exe\"\n\n[profiles.fast]\nmodel = \"x\"\n");
    WriteFile(environment.appData / "Claude" / "claude_desktop_config.json",
              "{\n  // a comment\n  \"mcpServers\": {}\n}\n");
    WriteFile(environment.home / ".gemini" / "settings.json", "{\"theme\": \"dark\", \"mcpServers\": {\"x\": {\"command\": \"x\"}}}");
    fs::create_directories(environment.appData / "Code" / "User");

    ClientOptions options;
    auto clients = DetectClients(environment, options);
    Check(Find(clients, "cursor") && Find(clients, "cursor")->state == "not_configured", "Cursor installed, not configured");
    Check(Find(clients, "windsurf") && Find(clients, "windsurf")->state == "not_installed", "Windsurf not installed");
    Check(Find(clients, "claude-desktop") && Find(clients, "claude-desktop")->state == "unreadable",
          "a config with comments is reported unreadable");
    Check(Find(clients, "codex") && Find(clients, "codex")->state == "not_configured", "Codex not configured");

    // Dry run writes nothing.
    auto dry = ConfigureClient(environment, "cursor", options, true);
    Check(dry.ok && dry.changed && !fs::exists(environment.home / ".cursor" / "mcp.json"), "dry run writes nothing");
    Check(dry.preview.find("\"mcp\"") != std::string::npos, "dry run shows the entry");

    // Cursor: new file, no backup needed.
    auto cursor = ConfigureClient(environment, "cursor", options, false);
    const json cursorConfig = json::parse(ReadFile(environment.home / ".cursor" / "mcp.json"));
    Check(cursor.ok && cursorConfig["mcpServers"]["cortex"]["args"] == json::array({"mcp"}),
          "Cursor gets cortex mcp (read-only)");
    Check(cursorConfig["mcpServers"]["cortex"]["command"] == environment.cortexExe.u8string(), "the command is this cortex.exe");
    Check(ConfigureClient(environment, "cursor", options, false).message == "already configured", "second run changes nothing");

    // Gemini: existing settings kept, backup made.
    auto gemini = ConfigureClient(environment, "gemini", options, false);
    const json geminiConfig = json::parse(ReadFile(environment.home / ".gemini" / "settings.json"));
    Check(gemini.ok && geminiConfig["theme"] == "dark" && geminiConfig["mcpServers"].contains("x") &&
              geminiConfig["mcpServers"].contains("cortex"),
          "Gemini keeps its other settings and servers");
    Check(!gemini.backup.empty() && fs::exists(gemini.backup) && CountBackups(environment.home / ".gemini") == 1,
          "the previous Gemini file is backed up");

    // Claude Desktop with comments: refused, untouched.
    const std::string before = ReadFile(environment.appData / "Claude" / "claude_desktop_config.json");
    auto claude = ConfigureClient(environment, "claude-desktop", options, false);
    Check(!claude.ok && ReadFile(environment.appData / "Claude" / "claude_desktop_config.json") == before,
          "an unparsable config is left untouched");
    Check(claude.preview.find("cortex") != std::string::npos, "and the entry to add by hand is shown");

    // VS Code uses "servers" with a type.
    ConfigureClient(environment, "vscode", options, false);
    const json vscode = json::parse(ReadFile(environment.appData / "Code" / "User" / "mcp.json"));
    Check(vscode["servers"]["cortex"]["type"] == "stdio", "VS Code entry has type stdio under servers");

    // Codex TOML: added, other sections kept, then updated in place.
    auto codex = ConfigureClient(environment, "codex", options, false);
    std::string toml = ReadFile(environment.home / ".codex" / "config.toml");
    Check(codex.ok && toml.find("[mcp_servers.cortex]") != std::string::npos && toml.find("[mcp_servers.other]") != std::string::npos &&
              toml.find("[profiles.fast]") != std::string::npos && toml.find("model = \"o3\"") != std::string::npos,
          "Codex config keeps its sections and gains cortex");
    ClientOptions writes;
    writes.allowWrites = true;
    writes.engines["ghidra"] = (base / "ghidra").u8string();
    writes.engines["java"] = (base / "jdk").u8string();
    clients = DetectClients(environment, writes);
    Check(Find(clients, "codex")->state == "outdated", "other options make the entry outdated");
    const auto codexWrites = ConfigureClient(environment, "codex", writes, false);
    toml = ReadFile(environment.home / ".codex" / "config.toml");
    if (!codexWrites.ok) std::printf("     %s\n", codexWrites.message.c_str());
    size_t sections = 0;
    for (size_t at = toml.find("[mcp_servers.cortex]"); at != std::string::npos; at = toml.find("[mcp_servers.cortex]", at + 1)) ++sections;
    Check(sections == 1 && toml.find("--allow-writes") != std::string::npos && toml.find("--ghidra-dir") != std::string::npos &&
              toml.find("[mcp_servers.cortex.env]") != std::string::npos && toml.find("[profiles.fast]") != std::string::npos,
          "Codex entry replaced in place with writes, Ghidra and JAVA_HOME");
    const json entry = ServerEntry(environment, writes);
    Check(entry["env"]["JAVA_HOME"] == (base / "jdk").u8string(), "JAVA_HOME goes in env");

    // Remove. The uninstaller keeps entries of another copy.
    Environment other = environment;
    other.cortexExe = base / "Elsewhere" / ("cortex" + kExe);
    auto kept = RemoveClient(other, "codex", false, true);
    Check(kept.ok && !kept.changed && ReadFile(environment.home / ".codex" / "config.toml").find("[mcp_servers.cortex]") != std::string::npos,
          "--only-this-copy keeps an entry of another copy");
    auto removed = RemoveClient(environment, "codex", false, true);
    toml = ReadFile(environment.home / ".codex" / "config.toml");
    Check(removed.ok && removed.changed && toml.find("mcp_servers.cortex") == std::string::npos &&
              toml.find("[mcp_servers.other]") != std::string::npos,
          "Codex entry removed, the rest kept");
    Check(RemoveClient(environment, "cursor", false).ok &&
              !json::parse(ReadFile(environment.home / ".cursor" / "mcp.json"))["mcpServers"].contains("cortex"),
          "Cursor entry removed");
    Check(RemoveClient(environment, "windsurf", false).message == "not configured", "removing an absent entry is a no-op");
    Check(!ConfigureClient(environment, "nope", options, false).ok, "unknown client refused");

    // ------------------------------------------------------------ engines
    const fs::path ghidra = environment.home / "Downloads" / "ghidra_11.4.2_PUBLIC_20250826" / "ghidra_11.4.2_PUBLIC";
    WriteFile(ghidra / "support" / "analyzeHeadless.bat", "@echo off");
    WriteFile(ghidra / "Ghidra" / "application.properties", "application.name=Ghidra\napplication.version=11.4.2\n");
    const fs::path jdk21 = base / "ProgramFiles" / "Eclipse Adoptium" / "jdk-21.0.4.7-hotspot";
    WriteFile(jdk21 / "bin" / ("java" + kExe), "java");
    WriteFile(jdk21 / "release", "IMPLEMENTOR=\"Eclipse Adoptium\"\nJAVA_VERSION=\"21.0.4\"\n");
    const fs::path jdk17 = base / "ProgramFiles" / "Java" / "jdk-17";
    WriteFile(jdk17 / "bin" / ("java" + kExe), "java");
    WriteFile(jdk17 / "release", "JAVA_VERSION=\"17.0.9\"\n");
    WriteFile(environment.home / "Downloads" / "vineflower-1.11.1.jar", "jar");
    WriteFile(environment.home / ".dotnet" / "tools" / ("ilspycmd" + kExe), "tool");
    WriteFile(base / "bin" / ("dotnet" + kExe), "dotnet");
    WriteFile(environment.home / "Downloads" / "netcoredbg" / ("netcoredbg" + kExe), "dbg");

    const auto engines = DetectEngines(environment);
    auto engine = [&engines](const std::string& id) -> const EngineReport* {
        for (const auto& report : engines)
            if (report.id == id) return &report;
        return nullptr;
    };
    Check(engine("ghidra") && !engine("ghidra")->found.empty() && engine("ghidra")->found[0].path == ghidra &&
              engine("ghidra")->found[0].version == "11.4.2",
          "Ghidra found two levels down in Downloads, version read");
    Check(engine("java") && engine("java")->found.size() == 2 && engine("java")->found[0].version == "21.0.4",
          "both JDKs found, 21 first");
    Check(engine("vineflower") && !engine("vineflower")->found.empty() && engine("vineflower")->found[0].version == "1.11.1",
          "Vineflower jar found with its version");
    Check(engine("ilspycmd") && !engine("ilspycmd")->found.empty() && engine("ilspycmd")->found[0].source == "dotnet tool",
          "ilspycmd found as a dotnet tool");
    Check(engine("dotnet") && !engine("dotnet")->found.empty() && engine("dotnet")->found[0].source == "PATH", "dotnet on PATH");
    Check(engine("netcoredbg") && !engine("netcoredbg")->found.empty(), "netcoredbg found");
    std::string version;
    Check(!ValidateEngine("ghidra", base, version), "a folder that is not Ghidra is rejected");
    Check(engine("ghidra")->ToJson()["found"][0]["version"] == "11.4.2", "report serializes");

    // ------------------------------------------------------------ versions
    Check(CompareVersions("2.1.0", "2.0.0") > 0 && CompareVersions("v2.0.0", "2.0.0") == 0 &&
              CompareVersions("2.0.10", "2.0.9") > 0 && CompareVersions("2.1.0-rc1", "2.1.0") < 0 &&
              CompareVersions("2.1", "2.1.0") == 0,
          "version order");

    // ------------------------------------------------------------ update from a local feed
    const fs::path feedDir = base / "feed";
    const std::string zip = StoredZip({{"cortex.exe", "new exe"}, {"runtime/x64/cortex_core.dll", "dll"}, {"docs/a.md", "doc"}});
    WriteFile(feedDir / "cortex-v2.1.0-windows-portable.zip", zip);
    WriteFile(feedDir / "cortex-v2.1.0-windows-setup.exe", "installer");
    const std::string zipHash = HexOf(feedDir / "cortex-v2.1.0-windows-portable.zip");
    WriteFile(feedDir / "SHA256SUMS.txt", zipHash + "  cortex-v2.1.0-windows-portable.zip\n" +
                                               std::string(64, '0') + "  cortex-v2.1.0-windows-setup.exe\n");
    const json release = {
        {"tag_name", "v2.1.0"}, {"name", "Cortex 2.1.0"}, {"body", "notes"}, {"html_url", "https://example/r"},
        {"assets", json::array({
            {{"name", "cortex-v2.1.0-windows-portable.zip"}, {"browser_download_url", (feedDir / "cortex-v2.1.0-windows-portable.zip").u8string()}, {"size", zip.size()}},
            {{"name", "cortex-v2.1.0-windows-setup.exe"}, {"browser_download_url", (feedDir / "cortex-v2.1.0-windows-setup.exe").u8string()}, {"size", 9}},
            {{"name", "SHA256SUMS.txt"}, {"browser_download_url", (feedDir / "SHA256SUMS.txt").u8string()}, {"size", 10}}})}};
    WriteFile(feedDir / "latest.json", release.dump());

    const UpdateCheck newer = CheckForUpdate((feedDir / "latest.json").u8string(), "2.0.0");
    Check(newer.ok && newer.available && newer.latest.version == "2.1.0" && newer.latest.assets.size() == 3,
          "2.1.0 is offered to 2.0.0");
    const UpdateCheck same = CheckForUpdate((feedDir / "latest.json").u8string(), "2.1.0");
    Check(same.ok && !same.available, "no update for the same version");
    Check(!CheckForUpdate((feedDir / "missing.json").u8string(), "2.0.0").ok, "a missing feed is an error");
    Check(ChecksumFor("ABCDEF" + std::string(58, '1') + " *file.zip\n", "file.zip") == "abcdef" + std::string(58, '1'),
          "checksum lines (binary marker, case)");

    const fs::path work = base / "work";
    const PreparedUpdate portable = PrepareUpdate(newer.latest, InstallKind::Portable, work);
    Check(portable.ok && portable.sha256 == zipHash && fs::exists(portable.file), "portable archive downloaded and verified");
    const PreparedUpdate installer = PrepareUpdate(newer.latest, InstallKind::Installed, work);
    Check(!installer.ok && installer.error.find("does not match") != std::string::npos &&
              !fs::exists(work / "cortex-v2.1.0-windows-setup.exe"),
          "a download with the wrong SHA-256 is refused and deleted");

    const fs::path current = base / "apps" / "cortex-v2.0.0";
    fs::create_directories(current);
    WriteFile(current / "cortex.portable", "");
    const ApplyResult applied = ApplyUpdate(portable, current, "2.1.0");
    Check(applied.ok && applied.newFolder == base / "apps" / "cortex-v2.1.0" &&
              ReadFile(applied.newFolder / "cortex.exe") == "new exe" &&
              fs::exists(applied.newFolder / "runtime" / "x64" / "cortex_core.dll") &&
              fs::exists(applied.newFolder / "cortex.portable"),
          "portable update unpacked beside the current folder, portable marker kept");
    Check(fs::exists(current / "cortex.portable"), "the current folder is untouched");

    WriteFile(base / "evil.zip", StoredZip({{"../evil.txt", "x"}}));
    std::string error;
    Check(!ExtractZip(base / "evil.zip", base / "evil-out", error) && !fs::exists(base / "evil.txt"),
          "an archive entry outside the folder is refused");

    Check(DetectInstallKind(current) == InstallKind::Portable, "no uninstaller: portable");
    WriteFile(current / "unins000.exe", "");
    Check(DetectInstallKind(current) == InstallKind::Installed, "an Inno Setup uninstaller: installed");
    Check(FindBlockingProcesses(base / "apps" / "nothing-runs-here").empty(), "no process uses an unused folder");

    fs::remove_all(base);
    std::printf("%s: setup tests (%d failure%s)\n", failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
