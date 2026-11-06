#include "artifact.h"

#include "sha256.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace cortex::artifacts {
namespace {

class FileSource final : public ByteSource {
public:
    FileSource(std::string path, uint64_t size, std::ifstream stream)
        : path_(std::move(path)), size_(size), stream_(std::move(stream)) {}

    uint64_t Size() const override { return size_; }
    std::string Name() const override { return path_; }

    bool Read(uint64_t offset, void* buffer, size_t size) const override {
        if (offset > size_ || size > size_ - offset) return false;
        if (size == 0) return true;
        std::lock_guard<std::mutex> lock(mutex_);
        stream_.clear();
        stream_.seekg(static_cast<std::streamoff>(offset));
        stream_.read(static_cast<char*>(buffer), static_cast<std::streamsize>(size));
        return static_cast<size_t>(stream_.gcount()) == size;
    }

private:
    std::string path_;
    uint64_t size_;
    mutable std::mutex mutex_;
    mutable std::ifstream stream_;
};

class MemorySource final : public ByteSource {
public:
    MemorySource(std::vector<uint8_t> bytes, std::string name) : bytes_(std::move(bytes)), name_(std::move(name)) {}
    uint64_t Size() const override { return bytes_.size(); }
    std::string Name() const override { return name_; }
    bool Read(uint64_t offset, void* buffer, size_t size) const override {
        if (offset > bytes_.size() || size > bytes_.size() - offset) return false;
        if (size) std::memcpy(buffer, bytes_.data() + offset, size);
        return true;
    }

private:
    std::vector<uint8_t> bytes_;
    std::string name_;
};

class SliceSource final : public ByteSource {
public:
    SliceSource(ByteSourcePtr parent, uint64_t offset, uint64_t size, std::string name)
        : parent_(std::move(parent)), offset_(offset), size_(size), name_(std::move(name)) {}
    uint64_t Size() const override { return size_; }
    std::string Name() const override { return name_; }
    bool Read(uint64_t offset, void* buffer, size_t size) const override {
        if (offset > size_ || size > size_ - offset) return false;
        return parent_->Read(offset_ + offset, buffer, size);
    }

private:
    ByteSourcePtr parent_;
    uint64_t offset_;
    uint64_t size_;
    std::string name_;
};

} // namespace

std::vector<uint8_t> ByteSource::ReadSome(uint64_t offset, size_t size) const {
    if (offset >= Size()) return {};
    const uint64_t available = Size() - offset;
    std::vector<uint8_t> bytes(static_cast<size_t>(std::min<uint64_t>(available, size)));
    if (!bytes.empty() && !Read(offset, bytes.data(), bytes.size())) bytes.clear();
    return bytes;
}

bool ByteSource::ReadCString(uint64_t offset, size_t max, std::string& text) const {
    text.clear();
    const auto bytes = ReadSome(offset, max);
    for (uint8_t byte : bytes) {
        if (byte == 0) return true;
        text.push_back(static_cast<char>(byte));
    }
    return false;  // no terminator within `max` (or out of range)
}

ByteSourcePtr OpenFile(const std::string& path, std::string* error) {
    std::error_code fsError;
    const auto native = std::filesystem::u8path(path);
    if (!std::filesystem::is_regular_file(native, fsError)) {
        if (error) *error = "not_a_regular_file";
        return {};
    }
    const uint64_t size = std::filesystem::file_size(native, fsError);
    if (fsError) {
        if (error) *error = "file_size_unavailable";
        return {};
    }
    std::ifstream stream(native, std::ios::binary);
    if (!stream) {
        if (error) *error = "file_not_readable";
        return {};
    }
    return std::make_shared<FileSource>(path, size, std::move(stream));
}

ByteSourcePtr FromBytes(std::vector<uint8_t> bytes, std::string name) {
    return std::make_shared<MemorySource>(std::move(bytes), std::move(name));
}

ByteSourcePtr Slice(ByteSourcePtr parent, uint64_t offset, uint64_t size, std::string name) {
    if (!parent || offset > parent->Size() || size > parent->Size() - offset) return {};
    return std::make_shared<SliceSource>(std::move(parent), offset, size, std::move(name));
}

std::string Sha256Of(const ByteSource& source) {
    Sha256 hash;
    std::vector<uint8_t> buffer(1 << 20);
    for (uint64_t offset = 0; offset < source.Size();) {
        const size_t take = static_cast<size_t>(std::min<uint64_t>(buffer.size(), source.Size() - offset));
        if (!source.Read(offset, buffer.data(), take)) return {};
        hash.Update(buffer.data(), take);
        offset += take;
    }
    return Sha256::Hex(hash.Final());
}

const char* CapabilityStatusName(CapabilityStatus status) {
    switch (status) {
        case CapabilityStatus::Validated: return "validated";
        case CapabilityStatus::Experimental: return "experimental";
        case CapabilityStatus::Unavailable: break;
    }
    return "unavailable";
}

json CapabilitiesJson(const std::vector<Capability>& capabilities) {
    json rows = json::object();
    for (const auto& capability : capabilities) {
        json row = {{"status", CapabilityStatusName(capability.status)}};
        if (!capability.fixture.empty()) row["fixture"] = capability.fixture;
        if (!capability.note.empty()) row["note"] = capability.note;
        rows[capability.name] = std::move(row);
    }
    for (const char* name : {"static", "decompile", "live"})
        if (!rows.contains(name)) rows[name] = {{"status", "unavailable"}};
    return rows;
}

bool ReadU16(const ByteSource& source, uint64_t offset, uint16_t& value) {
    uint8_t b[2];
    if (!source.Read(offset, b, 2)) return false;
    value = static_cast<uint16_t>(b[0] | (b[1] << 8));
    return true;
}

bool ReadU32(const ByteSource& source, uint64_t offset, uint32_t& value) {
    uint8_t b[4];
    if (!source.Read(offset, b, 4)) return false;
    value = uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
    return true;
}

bool ReadU64(const ByteSource& source, uint64_t offset, uint64_t& value) {
    uint32_t low = 0, high = 0;
    if (!ReadU32(source, offset, low) || !ReadU32(source, offset + 4, high)) return false;
    value = (uint64_t(high) << 32) | low;
    return true;
}

std::string HexU64(uint64_t value) {
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "0x%llx", static_cast<unsigned long long>(value));
    return buffer;
}

void Registry::Add(ProviderPtr provider) {
    if (provider) providers_.push_back(std::move(provider));
}

ProviderPtr Registry::Detect(const ByteSource& source) const {
    ProviderPtr best;
    int bestScore = 0;
    for (const auto& provider : providers_) {
        const int score = provider->Detect(source);
        if (score > bestScore) {
            bestScore = score;
            best = provider;
        }
    }
    return best;
}

ProviderPtr Registry::Find(const std::string& name) const {
    for (const auto& provider : providers_)
        if (name == provider->Name()) return provider;
    return {};
}

namespace {

json IdentityJson(const ByteSource& source) {
    return {{"name", source.Name()}, {"size", source.Size()}, {"sha256", Sha256Of(source)}};
}

} // namespace

json Registry::Inspect(const ByteSource& source, const InspectOptions& options) const {
    json result = {{"schema", "cortex.artifact.v1"}, {"identity", IdentityJson(source)}};
    const auto provider = Detect(source);
    if (!provider) {
        result["ok"] = false;
        result["error"] = "unknown_format";
        result["providers_tried"] = providers_.size();
        return result;
    }
    result["provider"] = {{"name", provider->Name()}, {"version", provider->Version()}};
    result["capabilities"] = CapabilitiesJson(provider->Capabilities());
    std::string error;
    json data;
    try {
        data = provider->Inspect(source, options, error);
    } catch (const std::exception& ex) {
        // A provider bug must not take the host down with it.
        error = std::string("provider_exception:") + ex.what();
        data = nullptr;
    }
    if (data.is_null()) {
        result["ok"] = false;
        result["error"] = error.empty() ? "inspect_failed" : error;
        return result;
    }
    result["ok"] = true;
    result["format"] = data.value("format", std::string(provider->Formats().empty() ? "" : provider->Formats()[0]));
    result["data"] = std::move(data);
    return result;
}

json Registry::ListChildren(const ByteSource& source, const InspectOptions& options) const {
    json result = {{"schema", "cortex.artifact.children.v1"}, {"identity", IdentityJson(source)}};
    const auto provider = Detect(source);
    if (!provider) {
        result["ok"] = false;
        result["error"] = "unknown_format";
        return result;
    }
    result["provider"] = {{"name", provider->Name()}, {"version", provider->Version()}};
    std::string error;
    std::vector<Child> children;
    try {
        children = provider->Children(source, options, error);
    } catch (const std::exception& ex) {
        error = std::string("provider_exception:") + ex.what();
    }
    if (!error.empty()) {
        result["ok"] = false;
        result["error"] = error;
        return result;
    }
    json rows = json::array();
    for (const auto& child : children) {
        json row = {{"name", child.name}, {"size", child.size}, {"readable", child.readable}};
        if (!child.note.empty()) row["note"] = child.note;
        if (!child.compression.empty()) {
            row["compression"] = child.compression;
            row["stored_size"] = child.storedSize;
            row["crc32"] = HexU64(child.crc32);
        }
        rows.push_back(std::move(row));
    }
    result["ok"] = true;
    result["children"] = std::move(rows);
    result["truncated"] = children.size() >= options.maxEntries;
    return result;
}

ByteSourcePtr Provider::OpenChild(ByteSourcePtr container, const Child& child, std::string& error) const {
    auto slice = Slice(container, child.offset, child.size, container->Name() + "!" + child.name);
    if (!slice) error = "child_out_of_range";
    return slice;
}

ByteSourcePtr Registry::OpenChild(ByteSourcePtr container, const std::string& childName, std::string& error,
                                  const InspectOptions& options) const {
    error.clear();
    if (!container) {
        error = "no_container";
        return {};
    }
    const auto provider = Detect(*container);
    if (!provider) {
        error = "unknown_format";
        return {};
    }
    InspectOptions all = options;
    all.maxEntries = std::max<size_t>(all.maxEntries, 1u << 20);
    const auto children = provider->Children(*container, all, error);
    if (!error.empty()) return {};
    for (const auto& child : children) {
        if (child.name != childName) continue;
        if (!child.readable) {
            error = "child_not_readable";
            return {};
        }
        return provider->OpenChild(container, child, error);
    }
    error = "child_not_found";
    return {};
}

} // namespace cortex::artifacts
