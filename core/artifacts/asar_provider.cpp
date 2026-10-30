#include "asar_provider.h"

#include <algorithm>
#include <functional>

namespace cortex::artifacts {
namespace {

constexpr uint32_t kMaxHeader = 64u << 20;  // a header above 64 MiB is not a real app
constexpr int kMaxDepth = 64;

bool ParseOffset(const json& value, uint64_t& offset) {
    // ASAR stores offsets as decimal strings (they can exceed 2^53).
    try {
        if (value.is_string()) {
            const std::string text = value.get<std::string>();
            if (text.empty() || text.size() > 20 || text.find_first_not_of("0123456789") != std::string::npos)
                return false;
            offset = std::stoull(text);
            return true;
        }
        if (value.is_number_unsigned()) {
            offset = value.get<uint64_t>();
            return true;
        }
    } catch (...) {
    }
    return false;
}

// Walks the file tree depth-first in name order. `visit` returns false to stop.
void Walk(const json& node, const std::string& prefix, int depth,
          const std::function<bool(const std::string&, const json&)>& visit, bool& stopped, bool& tooDeep) {
    if (stopped) return;
    if (depth > kMaxDepth) {
        tooDeep = true;
        return;
    }
    const auto files = node.find("files");
    if (files == node.end() || !files->is_object()) return;
    for (auto it = files->begin(); it != files->end() && !stopped; ++it) {
        const std::string& name = it.key();
        // A name with a separator or a dot segment would escape its folder
        // once extracted; it is listed as-is but never joined into a path
        // that could climb out.
        const std::string path = prefix.empty() ? name : prefix + "/" + name;
        if (!it.value().is_object()) continue;
        if (!visit(path, it.value())) {
            stopped = true;
            return;
        }
        if (it.value().contains("files")) Walk(it.value(), path, depth + 1, visit, stopped, tooDeep);
    }
}

bool UnsafeName(const std::string& path) {
    if (path.find('\\') != std::string::npos || path.find(':') != std::string::npos) return true;
    size_t start = 0;
    while (start <= path.size()) {
        const size_t end = path.find('/', start);
        const std::string segment = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (segment.empty() || segment == "." || segment == "..") return true;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return false;
}

} // namespace

bool ReadAsarHeader(const ByteSource& source, json& header, uint64_t& dataOffset, std::string& error) {
    uint32_t sizePayload = 0, headerSize = 0, headerPayload = 0, jsonLength = 0;
    if (!ReadU32(source, 0, sizePayload) || sizePayload != 4 || !ReadU32(source, 4, headerSize)) {
        error = "not_asar";
        return false;
    }
    if (headerSize < 8 || headerSize > kMaxHeader || 8ull + headerSize > source.Size()) {
        error = "asar_header_size_invalid";
        return false;
    }
    if (!ReadU32(source, 8, headerPayload) || !ReadU32(source, 12, jsonLength) || jsonLength == 0 ||
        jsonLength > headerSize - 8 || headerPayload > headerSize - 4) {
        error = "asar_header_pickle_invalid";
        return false;
    }
    std::string text(jsonLength, '\0');
    if (!source.Read(16, text.data(), jsonLength)) {
        error = "asar_header_truncated";
        return false;
    }
    header = json::parse(text, nullptr, false);
    if (header.is_discarded() || !header.is_object() || !header.contains("files") || !header["files"].is_object()) {
        error = "asar_header_not_a_file_tree";
        return false;
    }
    dataOffset = 8ull + headerSize;
    return true;
}

std::vector<Capability> AsarProvider::Capabilities() const {
    return {{"static", CapabilityStatus::Validated, "cortex_artifacts_tests (generated ASAR fixtures)",
             "file tree, sizes, unpacked and link entries, integrity metadata, Electron entry point"},
            {"decompile", CapabilityStatus::Unavailable, "", "JavaScript inside is read as source (step 4b)"},
            {"live", CapabilityStatus::Unavailable, "", "Electron processes through CDP (step 8)"}};
}

int AsarProvider::Detect(const ByteSource& source) const {
    json header;
    uint64_t dataOffset = 0;
    std::string error;
    return ReadAsarHeader(source, header, dataOffset, error) ? 60 : 0;
}

std::vector<Child> AsarProvider::Children(const ByteSource& source, const InspectOptions& options,
                                          std::string& error) const {
    std::vector<Child> children;
    json header;
    uint64_t dataOffset = 0;
    if (!ReadAsarHeader(source, header, dataOffset, error)) return children;
    bool stopped = false, tooDeep = false;
    Walk(header, "", 0, [&](const std::string& path, const json& node) {
        if (node.contains("files")) return true;  // folders are implied by their entries
        if (children.size() >= options.maxEntries) return false;
        Child child;
        child.name = path;
        if (node.contains("link")) {
            child.readable = false;
            child.note = "link -> " + node.value("link", std::string());
        } else if (node.value("unpacked", false)) {
            child.readable = false;
            child.note = "unpacked: stored in app.asar.unpacked/";
            child.size = node.value("size", uint64_t{0});
        } else {
            uint64_t offset = 0;
            child.size = node.value("size", uint64_t{0});
            if (!ParseOffset(node.value("offset", json()), offset) || offset > source.Size() ||
                dataOffset > source.Size() - offset || child.size > source.Size() - dataOffset - offset) {
                child.readable = false;
                child.note = "offset outside the archive";
            } else {
                child.offset = dataOffset + offset;
            }
        }
        if (UnsafeName(path)) {
            child.readable = false;
            child.note = "unsafe entry name (separator, dot segment or drive)";
        }
        children.push_back(std::move(child));
        return true;
    }, stopped, tooDeep);
    if (tooDeep) error = "asar_tree_too_deep";
    return children;
}

json AsarProvider::Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const {
    json header;
    uint64_t dataOffset = 0;
    if (!ReadAsarHeader(source, header, dataOffset, error)) return nullptr;
    std::string childError;
    const auto children = Children(source, options, childError);

    uint64_t files = 0, unpacked = 0, links = 0, bytes = 0, withIntegrity = 0, unsafe = 0;
    json extensions = json::object();
    bool stopped = false, tooDeep = false;
    Walk(header, "", 0, [&](const std::string& path, const json& node) {
        if (node.contains("files")) return true;
        if (node.contains("link")) { ++links; return true; }
        ++files;
        if (node.value("unpacked", false)) ++unpacked;
        if (node.contains("integrity")) ++withIntegrity;
        if (UnsafeName(path)) ++unsafe;
        bytes += node.value("size", uint64_t{0});
        const auto dot = path.find_last_of('.');
        const auto slash = path.find_last_of('/');
        const std::string extension = dot == std::string::npos || (slash != std::string::npos && dot < slash)
            ? std::string("(none)") : path.substr(dot);
        extensions[extension] = extensions.value(extension, 0) + 1;
        return true;
    }, stopped, tooDeep);

    json data = {{"format", "asar"},
                 {"header_size", dataOffset - 8},
                 {"data_offset", dataOffset},
                 {"files", files},
                 {"unpacked_files", unpacked},
                 {"links", links},
                 {"total_file_bytes", bytes},
                 {"files_with_integrity", withIntegrity},
                 {"extensions", extensions}};
    if (unsafe) data["unsafe_entry_names"] = unsafe;
    if (tooDeep || !childError.empty()) data["warnings"] = json::array({childError.empty() ? "asar_tree_too_deep" : childError});

    // Electron reads package.json at the archive root for its entry point.
    for (const auto& child : children) {
        if (child.name != "package.json" || !child.readable || child.size > (1u << 20)) continue;
        std::string text(static_cast<size_t>(child.size), '\0');
        if (!source.Read(child.offset, text.data(), text.size())) break;
        const json package = json::parse(text, nullptr, false);
        if (!package.is_object()) {
            data["package_json_error"] = "not_json";
            break;
        }
        json summary = json::object();
        for (const char* key : {"name", "version", "main", "productName", "description", "type"})
            if (package.contains(key) && package[key].is_string()) summary[key] = package[key];
        if (package.contains("dependencies") && package["dependencies"].is_object())
            summary["dependencies"] = package["dependencies"].size();
        data["package_json"] = std::move(summary);
        break;
    }
    return data;
}

} // namespace cortex::artifacts
