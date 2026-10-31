#include "annotations.h"

#include "../services/user_data.h"

#include <chrono>
#include <fstream>
#include <map>

namespace cortex::artifacts {
namespace {

using json = nlohmann::json;

constexpr size_t kMaxText = 4096;
constexpr size_t kMaxAnnotations = 100000;

json Load(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return nullptr;
    json data = json::parse(in, nullptr, false);
    return data.is_object() ? data : json(nullptr);
}

bool HexDigits(const std::string& text) {
    return !text.empty() && text.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
}

} // namespace

bool ValidSha256(const std::string& text) {
    return text.size() == 64 && text.find_first_not_of("0123456789abcdef") == std::string::npos;
}

bool ValidAnnotationLocation(const std::string& location) {
    if (location.size() > 512 || location.find_first_of("\r\n") != std::string::npos) return false;
    if (location.rfind("rva:0x", 0) == 0) return location.size() <= 16 && HexDigits(location.substr(6));
    if (location.rfind("token:0x", 0) == 0) return location.size() == 16 && HexDigits(location.substr(8));
    if (location.rfind("js:", 0) == 0) return location.size() > 3;
    return false;
}

std::filesystem::path AnnotationStore::FileFor(const std::string& sha256) const {
    return directory_ / (sha256 + ".json");
}

json AnnotationStore::Get(const std::string& sha256) const {
    if (!ValidSha256(sha256)) return {{"ok", false}, {"error", "invalid_sha256"}};
    std::lock_guard<std::mutex> lock(mutex_);
    json data = Load(FileFor(sha256));
    if (data.is_null()) data = {{"revision", 0}, {"annotations", json::array()}};
    return {{"ok", true},
            {"sha256", sha256},
            {"revision", data.value("revision", 0)},
            {"annotations", data.value("annotations", json::array())},
            {"note", "names and comments written by people or agents; not derived or verified by Cortex"}};
}

json AnnotationStore::Put(const std::string& sha256, const std::string& location, const std::string& kind,
                          const std::string& text, const std::string& source, int64_t expectedRevision) {
    if (!ValidSha256(sha256)) return {{"ok", false}, {"error", "invalid_sha256"}};
    if (!ValidAnnotationLocation(location)) return {{"ok", false}, {"error", "invalid_location"}};
    if (kind != "name" && kind != "comment") return {{"ok", false}, {"error", "invalid_kind"}};
    if (text.size() > kMaxText || (kind == "name" && text.find_first_of("\r\n") != std::string::npos))
        return {{"ok", false}, {"error", "invalid_text"}};
    std::lock_guard<std::mutex> lock(mutex_);
    json data = Load(FileFor(sha256));
    if (data.is_null()) data = {{"revision", 0}, {"annotations", json::array()}};
    const int64_t revision = data.value("revision", int64_t{0});
    if (expectedRevision >= 0 && expectedRevision != revision)
        return {{"ok", false}, {"error", "revision_conflict"}, {"revision", revision}};
    json annotations = json::array();
    bool replaced = false;
    for (const auto& entry : data.value("annotations", json::array())) {
        if (entry.value("location", std::string()) == location && entry.value("kind", std::string()) == kind) {
            replaced = true;
            continue;  // replaced below, or removed when text is empty
        }
        annotations.push_back(entry);
    }
    if (!text.empty()) {
        if (annotations.size() >= kMaxAnnotations) return {{"ok", false}, {"error", "too_many_annotations"}};
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch()).count();
        annotations.push_back({{"location", location}, {"kind", kind}, {"text", text}, {"source", source},
                               {"updated_ms", now}});
    } else if (!replaced) {
        return {{"ok", false}, {"error", "annotation_not_found"}};
    }
    data["revision"] = revision + 1;
    data["annotations"] = annotations;
    std::error_code ignored;
    std::filesystem::create_directories(directory_, ignored);
    std::string error;
    if (!services::WriteFileAtomically(FileFor(sha256), data.dump(1), &error)) return {{"ok", false}, {"error", error}};
    return {{"ok", true}, {"sha256", sha256}, {"revision", revision + 1}, {"location", location}, {"kind", kind},
            {"removed", text.empty()}};
}

std::map<uint32_t, std::string> AnnotationStore::NativeNames(const std::string& sha256) const {
    std::map<uint32_t, std::string> names;
    const json data = Get(sha256);
    for (const auto& entry : data.value("annotations", json::array())) {
        const std::string location = entry.value("location", std::string());
        if (entry.value("kind", std::string()) != "name" || location.rfind("rva:0x", 0) != 0) continue;
        try {
            names[static_cast<uint32_t>(std::stoul(location.substr(6), nullptr, 16))] = entry.value("text", std::string());
        } catch (...) {
        }
    }
    return names;
}

} // namespace cortex::artifacts
