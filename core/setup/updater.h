#pragma once

// Updates from the project's GitHub releases.
//
// The check reads one small JSON document (the latest release) and compares
// its version with this build's. Nothing is downloaded or installed without
// the person's go-ahead. A download is accepted only when its SHA-256 matches
// the SHA256SUMS.txt published with the same release; an installed copy is
// then updated by running the release's installer, a portable copy by
// unpacking the release's archive into a new folder beside the current one.
//
// Limits: the checksum file comes from the same release as the download, so
// it proves the file arrived intact, not who built it. A code-signing
// certificate would add that; the release is not signed yet.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace cortex::setup {

namespace fs = std::filesystem;
using json = nlohmann::json;

// The default feed: the latest published release of PdB333/cortex.
inline constexpr const char* kDefaultUpdateFeed = "https://api.github.com/repos/PdB333/cortex/releases/latest";

struct ReleaseAsset {
    std::string name;
    std::string url;   // https URL, or a local path for a file feed
    uint64_t size = 0;
};

struct Release {
    std::string version;  // "2.1.0" (the tag without its leading v)
    std::string tag;
    std::string name;
    std::string notes;    // the release body (Markdown)
    std::string page;     // the release's web page
    std::vector<ReleaseAsset> assets;
    const ReleaseAsset* Find(const std::string& suffix) const;
};

// -1, 0 or 1. Numeric parts compare as numbers; "2.1.0-rc1" < "2.1.0".
int CompareVersions(const std::string& a, const std::string& b);

// From a GitHub release document (or the same shape in a local file).
bool ParseRelease(const json& document, Release& release, std::string& error);

// "hex  filename" lines; the hash for `fileName`, lower case, or empty.
std::string ChecksumFor(const std::string& sums, const std::string& fileName);

// How this copy of Cortex was installed.
enum class InstallKind { Installed, Portable };
InstallKind DetectInstallKind(const fs::path& applicationDirectory);
const char* InstallKindName(InstallKind kind);

// The asset this copy updates from: the installer when installed, the
// portable archive otherwise.
std::string AssetSuffix(InstallKind kind);

struct UpdateCheck {
    bool ok = false;
    std::string error;
    bool available = false;
    std::string current;
    Release latest;
    json ToJson() const;
};

// Reads the feed (a URL, or a local JSON file for tests and offline mirrors).
UpdateCheck CheckForUpdate(const std::string& feed, const std::string& currentVersion);

using Progress = std::function<void(uint64_t done, uint64_t total)>;

// Downloads `url` (or copies a local path) to `destination`, at most `limit`
// bytes.
bool Download(const std::string& url, const fs::path& destination, uint64_t limit, std::string& error,
              const Progress& progress = {});
// Text of a small document (the feed, the checksum file).
bool FetchText(const std::string& url, std::string& text, std::string& error, size_t limit = 4u << 20);

struct PreparedUpdate {
    bool ok = false;
    std::string error;
    InstallKind kind = InstallKind::Portable;
    fs::path file;       // the verified installer or archive
    std::string sha256;
    json ToJson() const;
};

// Downloads the asset for `kind` and its checksum, and verifies it.
PreparedUpdate PrepareUpdate(const Release& release, InstallKind kind, const fs::path& workDirectory,
                             const Progress& progress = {});

// Processes that would keep files of this installation in use: other
// cortex.exe from the same folder (MCP servers) and programs with its
// runtime loaded. Empty when an update can be applied.
struct BlockingProcess {
    uint32_t pid = 0;
    std::string name;
    std::string reason;  // "cortex is running" or "the Cortex runtime is loaded"
};
std::vector<BlockingProcess> FindBlockingProcesses(const fs::path& applicationDirectory);

// Installed: starts the verified installer (it waits for Cortex to close).
// Portable: unpacks the archive into a new folder beside the current one and
// returns it in `result`. The caller exits afterwards when asked to.
struct ApplyResult {
    bool ok = false;
    std::string error;
    bool exitToFinish = false;  // installed: close Cortex so the installer can replace it
    fs::path newFolder;         // portable: where the new version is
};
ApplyResult ApplyUpdate(const PreparedUpdate& update, const fs::path& applicationDirectory, const std::string& version);

// Unpacks a ZIP archive into `destination` (entries outside it are refused).
bool ExtractZip(const fs::path& archive, const fs::path& destination, std::string& error);

}  // namespace cortex::setup
