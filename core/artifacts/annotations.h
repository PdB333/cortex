#pragma once

// Annotations on artifacts (step 4d): names and comments a person or an
// agent attaches to a location, stored per artifact SHA-256 so they follow
// the file's content, not its path. Locations:
//   rva:0x1234          native code or data (PE)
//   token:0x06000004    .NET metadata token
//   js:path:line:col    JavaScript source position
// Records are what someone wrote, never facts derived by Cortex. Each
// change bumps the artifact's revision; a stale expected revision is
// refused so concurrent writers do not overwrite each other.

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <mutex>
#include <string>

namespace cortex::artifacts {

class AnnotationStore {
public:
    explicit AnnotationStore(std::filesystem::path directory) : directory_(std::move(directory)) {}

    // {"ok", "sha256", "revision", "annotations": [...]}; an artifact with
    // no file yet has revision 0 and no entries.
    nlohmann::json Get(const std::string& sha256) const;

    // Sets (or with empty `text`, removes) the `kind` ("name" | "comment")
    // annotation at `location`. `source` is who wrote it ("desktop",
    // "agent", "cli"). `expectedRevision` < 0 skips the check.
    nlohmann::json Put(const std::string& sha256, const std::string& location, const std::string& kind,
                       const std::string& text, const std::string& source, int64_t expectedRevision);

    // RVA -> name for rva: locations of an artifact.
    std::map<uint32_t, std::string> NativeNames(const std::string& sha256) const;

    const std::filesystem::path& Directory() const { return directory_; }

private:
    std::filesystem::path FileFor(const std::string& sha256) const;
    std::filesystem::path directory_;
    mutable std::mutex mutex_;
};

bool ValidSha256(const std::string& text);
bool ValidAnnotationLocation(const std::string& location);

} // namespace cortex::artifacts
