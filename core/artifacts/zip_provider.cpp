#include "zip_provider.h"
#include "package_metadata.h"

#include "inflate.h"
#include "appx_metadata.h"
#include "bounded_reader.h"

#include <algorithm>
#include <map>

namespace cortex::artifacts {
namespace {

constexpr uint32_t kEndOfDirectory = 0x06054b50;
constexpr uint32_t kZip64Locator = 0x07064b50;
constexpr uint32_t kZip64End = 0x06064b50;
constexpr uint32_t kCentralEntry = 0x02014b50;
constexpr uint32_t kLocalEntry = 0x04034b50;
constexpr uint64_t kCrcBudget = 256ull << 20;  // bytes expanded to check CRCs in Inspect
constexpr size_t kMaxEntries = 1u << 20;

struct Directory {
    uint64_t entries = 0;
    uint64_t size = 0;
    uint64_t offset = 0;     // where the directory really is
    uint64_t prefix = 0;     // bytes before the archive (self-extractor, stub)
    bool zip64 = false;
    std::string comment;
};

struct Entry {
    std::string name;
    bool nameEncodingValid = true;
    uint16_t flags = 0;
    uint16_t method = 0;
    uint32_t crc = 0;
    uint64_t compressed = 0;
    uint64_t size = 0;
    uint64_t localOffset = 0;
};

bool FindDirectory(const ByteSource& source, Directory& directory, std::string& error) {
    const uint64_t size = source.Size();
    if (size < 22) {
        error = "not_zip";
        return false;
    }
    const uint64_t window = std::min<uint64_t>(size, 22 + 0xFFFF);
    const auto tail = source.ReadSome(size - window, static_cast<size_t>(window));
    if (tail.size() != window) {
        error = "read_failed";
        return false;
    }
    for (size_t i = tail.size() - 22 + 1; i-- > 0;) {
        if (tail[i] != 0x50 || tail[i + 1] != 0x4b || tail[i + 2] != 0x05 || tail[i + 3] != 0x06) continue;
        const uint64_t at = size - window + i;
        uint16_t disk = 0, startDisk = 0, entriesHere = 0, entries = 0, commentLength = 0;
        uint32_t directorySize = 0, directoryOffset = 0;
        if (!ReadU16(source, at + 4, disk) || !ReadU16(source, at + 6, startDisk) || !ReadU16(source, at + 8, entriesHere) ||
            !ReadU16(source, at + 10, entries) || !ReadU32(source, at + 12, directorySize) ||
            !ReadU32(source, at + 16, directoryOffset) || !ReadU16(source, at + 20, commentLength))
            continue;
        if (at + 22 + commentLength != size) continue;  // a stray signature inside the comment or data
        directory.entries = entries;
        directory.size = directorySize;
        directory.offset = directoryOffset;
        if (commentLength) {
            const auto comment = source.ReadSome(at + 22, std::min<size_t>(commentLength, 1024));
            directory.comment = bounded::Utf8(comment.data(), comment.size());
        }
        uint64_t directoryEnd = at;
        if (entries == 0xFFFF || directorySize == 0xFFFFFFFF || directoryOffset == 0xFFFFFFFF) {
            uint32_t signature = 0;
            uint64_t recordOffset = 0;
            if (at < 20 || !ReadU32(source, at - 20, signature) || signature != kZip64Locator ||
                !ReadU64(source, at - 20 + 8, recordOffset)) {
                error = "zip64_locator_missing";
                return false;
            }
            // Usually right before the locator; otherwise where the locator says
            // (relative to the archive start, so before any prefix shift).
            uint64_t recordAt = at >= 76 ? at - 20 - 56 : 0;
            if (at < 76 || !ReadU32(source, recordAt, signature) || signature != kZip64End) recordAt = recordOffset;
            if (!ReadU32(source, recordAt, signature) || signature != kZip64End ||
                !ReadU64(source, recordAt + 32, directory.entries) || !ReadU64(source, recordAt + 40, directory.size) ||
                !ReadU64(source, recordAt + 48, directory.offset)) {
                error = "zip64_record_invalid";
                return false;
            }
            directory.zip64 = true;
            directoryEnd = recordAt;
        }
        if (directory.size > directoryEnd) {
            error = "zip_directory_size_invalid";
            return false;
        }
        // Where the directory actually starts; any difference is a prefix.
        const uint64_t actual = directoryEnd - directory.size;
        if (actual < directory.offset) {
            error = "zip_directory_offset_invalid";
            return false;
        }
        directory.prefix = actual - directory.offset;
        directory.offset = actual;
        if (directory.entries > kMaxEntries) {
            error = "zip_too_many_entries";
            return false;
        }
        return true;
    }
    error = "not_zip";
    return false;
}

bool ReadEntries(const ByteSource& source, const Directory& directory, std::vector<Entry>& entries, size_t limit,
                 std::string& error) {
    uint64_t at = directory.offset;
    const uint64_t end = directory.offset + directory.size;
    for (uint64_t index = 0; index < directory.entries; ++index) {
        uint32_t signature = 0;
        uint16_t flags = 0, method = 0, nameLength = 0, extraLength = 0, commentLength = 0;
        uint32_t crc = 0, compressed = 0, size = 0, localOffset = 0;
        if (at + 46 > end || !ReadU32(source, at, signature) || signature != kCentralEntry ||
            !ReadU16(source, at + 8, flags) || !ReadU16(source, at + 10, method) || !ReadU32(source, at + 16, crc) ||
            !ReadU32(source, at + 20, compressed) || !ReadU32(source, at + 24, size) ||
            !ReadU16(source, at + 28, nameLength) || !ReadU16(source, at + 30, extraLength) ||
            !ReadU16(source, at + 32, commentLength) || !ReadU32(source, at + 42, localOffset)) {
            error = "zip_directory_entry_invalid";
            return false;
        }
        if (at + 46 + nameLength + extraLength + commentLength > end) {
            error = "zip_directory_entry_truncated";
            return false;
        }
        Entry entry;
        const auto name = source.ReadSome(at + 46, nameLength);
        entry.name = bounded::Utf8(name.data(), name.size());
        entry.nameEncodingValid = entry.name == std::string(name.begin(), name.end());
        entry.flags = flags;
        entry.method = method;
        entry.crc = crc;
        entry.compressed = compressed;
        entry.size = size;
        entry.localOffset = localOffset;
        // ZIP64 extra field: the 32-bit values set to 0xFFFFFFFF follow in order.
        uint64_t extra = at + 46 + nameLength;
        const uint64_t extraEnd = extra + extraLength;
        while (extra + 4 <= extraEnd) {
            uint16_t id = 0, length = 0;
            if (!ReadU16(source, extra, id) || !ReadU16(source, extra + 2, length) || extra + 4 + length > extraEnd) break;
            if (id == 0x0001) {
                uint64_t field = extra + 4;
                const uint64_t fieldEnd = field + length;
                auto take = [&](uint64_t& value) {
                    if (field + 8 <= fieldEnd && ReadU64(source, field, value)) field += 8;
                };
                if (size == 0xFFFFFFFF) take(entry.size);
                if (compressed == 0xFFFFFFFF) take(entry.compressed);
                if (localOffset == 0xFFFFFFFF) take(entry.localOffset);
            }
            extra += 4 + length;
        }
        entry.localOffset += directory.prefix;
        if (entries.size() < limit) entries.push_back(std::move(entry));
        at += 46 + nameLength + extraLength + commentLength;
    }
    return true;
}

bool UnsafeName(const std::string& name) {
    if (name.empty() || name[0] == '/' || name.find('\\') != std::string::npos || name.find(':') != std::string::npos)
        return true;
    size_t start = 0;
    while (start <= name.size()) {
        const size_t end = name.find('/', start);
        const std::string segment = name.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (segment == "..") return true;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return false;
}

Child ToChild(const ByteSource& source, const Entry& entry) {
    Child child;
    child.name = entry.name;
    child.size = entry.size;
    child.storedSize = entry.compressed;
    child.crc32 = entry.crc;
    child.compression = entry.method == 0 ? "stored" : entry.method == 8 ? "deflate" : "method " + std::to_string(entry.method);
    uint32_t signature = 0;
    uint16_t nameLength = 0, extraLength = 0;
    if (!ReadU32(source, entry.localOffset, signature) || signature != kLocalEntry ||
        !ReadU16(source, entry.localOffset + 26, nameLength) || !ReadU16(source, entry.localOffset + 28, extraLength)) {
        child.readable = false;
        child.note = "local header missing";
        return child;
    }
    const uint64_t headerSize = 30ull + nameLength + extraLength;
    if (!bounded::Range(source.Size(), entry.localOffset, headerSize)) {
        child.readable = false; child.note = "local header outside the archive"; return child;
    }
    child.offset = entry.localOffset + headerSize;
    if (child.offset > source.Size() || entry.compressed > source.Size() - child.offset) {
        child.readable = false;
        child.note = "data outside the archive";
    } else if (entry.flags & 1) {
        child.readable = false;
        child.note = "encrypted";
    } else if (entry.method != 0 && entry.method != 8) {
        child.readable = false;
        child.note = "compression method " + std::to_string(entry.method) + " not supported";
    } else if (entry.method == 0 && entry.compressed != entry.size) {
        child.readable = false;
        child.note = "stored entry with differing sizes";
    } else if (entry.size > kZipMaxExpandedEntry) {
        child.readable = false;
        child.note = "larger than the " + std::to_string(kZipMaxExpandedEntry >> 20) + " MiB expansion limit";
    }
    if (!entry.nameEncodingValid) {
        child.readable = false;
        child.note = "filename is not supported UTF-8; display was repaired, extraction refused";
    }
    if (UnsafeName(entry.name)) {
        child.readable = false;
        child.note = "unsafe entry name (absolute, drive, backslash or ..)";
    }
    return child;
}

std::string Lower(std::string text) {
    for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

bool EndsWith(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// The text between <tag> and </tag> (first occurrence), for .nuspec files.
std::string XmlElement(const std::string& xml, const std::string& tag) {
    const size_t open = xml.find("<" + tag + ">");
    if (open == std::string::npos) return {};
    const size_t start = open + tag.size() + 2;
    const size_t close = xml.find("</" + tag + ">", start);
    if (close == std::string::npos || close - start > 4096) return {};
    return xml.substr(start, close - start);
}

std::vector<uint8_t> Expand(const ByteSource& source, const Child& child, std::string& error) {
    std::vector<uint8_t> out;
    if (!child.readable) {
        error = "child_not_readable";
        return out;
    }
    if (child.storedSize > kZipMaxExpandedEntry) { error = "zip_stored_read_limit"; return out; }
    const auto stored = source.ReadSome(child.offset, static_cast<size_t>(child.storedSize));
    if (stored.size() != child.storedSize) {
        error = "read_failed";
        return out;
    }
    if (child.compression == "stored") {
        out = stored;
    } else if (!Inflate(stored.data(), stored.size(), out, static_cast<size_t>(child.size), error)) {
        if (error == "inflate_output_limit") error = "zip_entry_larger_than_declared";
        out.clear();
        return out;
    }
    if (out.size() != child.size) {
        error = "zip_entry_size_mismatch";
        out.clear();
    } else if (Crc32(out.data(), out.size()) != child.crc32) {
        error = "zip_crc_mismatch";
        out.clear();
    }
    return out;
}

} // namespace

std::vector<Capability> ZipProvider::Capabilities() const {
    return {{"static", CapabilityStatus::Validated,
             "cortex_dotnet_tests (NuGet package from dotnet pack, Python zipfile cases, malformed archives, fuzzing)",
             "central directory, ZIP64, prefixed archives, stored and deflated entries with CRC-32 check, JAR/APK/"
             "NuGet/wheel recognition"},
            {"decompile", CapabilityStatus::Unavailable, "", "entries are inspected by their own providers"},
            {"live", CapabilityStatus::Unavailable, "", "not applicable"}};
}

int ZipProvider::Detect(const ByteSource& source) const {
    Directory directory;
    std::string error;
    return FindDirectory(source, directory, error) ? 40 : 0;
}

std::vector<Child> ZipProvider::Children(const ByteSource& source, const InspectOptions& options,
                                         std::string& error) const {
    std::vector<Child> children;
    Directory directory;
    std::vector<Entry> entries;
    if (!FindDirectory(source, directory, error) || !ReadEntries(source, directory, entries, kMaxEntries, error))
        return children;
    for (const auto& entry : entries) {
        if (EndsWith(entry.name, "/")) continue;  // folders are implied by their files
        if (children.size() >= options.maxEntries) break;
        children.push_back(ToChild(source, entry));
    }
    return children;
}

ByteSourcePtr ZipProvider::OpenChild(ByteSourcePtr container, const Child& child, std::string& error) const {
    auto bytes = Expand(*container, child, error);
    if (!error.empty()) return {};
    return FromBytes(std::move(bytes), container->Name() + "!" + child.name);
}

json ZipProvider::Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const {
    Directory directory;
    std::vector<Entry> entries;
    if (!FindDirectory(source, directory, error) || !ReadEntries(source, directory, entries, kMaxEntries, error))
        return nullptr;
    InspectOptions all = options;
    all.maxEntries = kMaxEntries;
    std::string childError;
    const auto children = Children(source, all, childError);

    uint64_t expanded = 0, stored = 0, encrypted = 0, unsafe = 0, unreadable = 0;
    std::map<std::string, uint64_t> methods;
    std::map<std::string, const Child*> byName;
    for (const auto& child : children) {
        expanded += child.size;
        stored += child.storedSize;
        methods[child.compression]++;
        if (child.note == "encrypted") ++encrypted;
        if (child.note.rfind("unsafe", 0) == 0) ++unsafe;
        if (!child.readable) ++unreadable;
        byName[child.name] = &child;
    }

    // Recognise the formats built on ZIP.
    std::string kind = "zip";
    const auto has = [&](const std::string& name) { return byName.count(name) > 0; };
    std::string nuspec, wheel;
    for (const auto& [name, child] : byName) {
        if (name.find('/') == std::string::npos && EndsWith(Lower(name), ".nuspec")) nuspec = name;
        if (EndsWith(name, ".dist-info/WHEEL")) wheel = name;
    }
    if (has("AndroidManifest.xml") && has("classes.dex")) kind = "apk";
    else if (!nuspec.empty() && has("[Content_Types].xml")) kind = "nupkg";
    else if (!wheel.empty()) kind = "wheel";
    else if (has("META-INF/MANIFEST.MF")) kind = "jar";

    json data = {{"format", kind},
                 {"entries", children.size()},
                 {"zip64", directory.zip64},
                 {"prefix_bytes", directory.prefix},
                 {"stored_bytes", stored},
                 {"expanded_bytes", expanded},
                 {"methods", methods},
                 {"encrypted_entries", encrypted},
                 {"unreadable_entries", unreadable}};
    if (unsafe) data["unsafe_entry_names"] = unsafe;
    if (!directory.comment.empty()) data["comment"] = directory.comment;
    if (stored && expanded / std::max<uint64_t>(stored, 1) > 1000)
        data["warnings"] = json::array({"expansion ratio above 1000:1 (possible zip bomb); entries over " +
                                        std::to_string(kZipMaxExpandedEntry >> 20) + " MiB are never expanded"});

    // CRC of every readable entry, within a budget.
    uint64_t hashed = 0, matched = 0, skipped = 0;
    json mismatches = json::array();
    for (const auto& child : children) {
        if (!child.readable) continue;
        if (hashed + child.size > kCrcBudget) {
            ++skipped;
            continue;
        }
        hashed += child.size;
        std::string expandError;
        Expand(source, child, expandError);
        if (expandError.empty()) ++matched;
        else if (mismatches.size() < options.maxEntries) mismatches.push_back({{"file", child.name}, {"result", expandError}});
    }
    data["crc"] = {{"checked", matched + mismatches.size()}, {"matched", matched}, {"mismatches", mismatches},
                   {"skipped_budget", skipped}};

    // Format details.
    auto text = [&](const std::string& name, size_t limit) {
        const auto it = byName.find(name);
        if (it == byName.end() || it->second->size > limit) return std::string();
        std::string expandError;
        const auto bytes = Expand(source, *it->second, expandError);
        return bounded::Utf8(bytes.data(), bytes.size());
    };
    if (kind == "nupkg") {
        const std::string xml = text(nuspec, 1u << 20);
        json package = {{"nuspec", nuspec}};
        for (const char* tag : {"id", "version", "authors", "description"})
            if (const std::string value = XmlElement(xml, tag); !value.empty()) package[tag] = value;
        json assemblies = json::array();
        for (const auto& child : children)
            if (child.name.rfind("lib/", 0) == 0 && EndsWith(Lower(child.name), ".dll") && assemblies.size() < options.maxEntries)
                assemblies.push_back(child.name);
        package["assemblies"] = assemblies;
        data["package"] = package;
    } else if (kind == "jar") {
        const std::string manifest = text("META-INF/MANIFEST.MF", 1u << 20);
        for (const char* key : {"Main-Class", "Created-By", "Implementation-Title", "Implementation-Version"}) {
            const size_t at = manifest.find(std::string(key) + ": ");
            if (at == std::string::npos) continue;
            const size_t start = at + std::string(key).size() + 2;
            data["manifest"][key] = manifest.substr(start, manifest.find_first_of("\r\n", start) - start);
        }
    } else if (kind == "wheel") {
        data["wheel"] = wheel;
    }
    const json appx = InspectAppx(source, children, options);
    if (!appx.empty()) {
        data["appx"] = appx;
        if (appx.contains("format")) data["format"] = appx["format"];
    }
    if (!data.contains("appx")) {
        const json package = InspectZipPackage(source, children, options);
        if (!package.empty()) {
            data["package_metadata"] = package;
            if (package.contains("format")) data["format"] = package["format"];
        }
    }
    if (!childError.empty()) data["warnings"].push_back(childError);
    return data;
}

} // namespace cortex::artifacts
