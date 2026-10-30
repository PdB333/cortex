#pragma once

// Artifact providers (roadmap step 4a, §8): static analysis of files of any
// supported format through one interface, so native PE, .NET, JavaScript,
// Electron and later runtimes share detection, identity, the GUI, the CLI
// and MCP. Everything read from an artifact is untrusted data: providers
// bound every read and every count, and report what they could not parse
// instead of guessing.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cortex::artifacts {

using json = nlohmann::json;

// Random-access bytes of one artifact (a file, or an entry of a container).
class ByteSource {
public:
    virtual ~ByteSource() = default;
    virtual uint64_t Size() const = 0;
    // Reads exactly `size` bytes at `offset`; false if any byte is outside.
    virtual bool Read(uint64_t offset, void* buffer, size_t size) const = 0;
    // Display name: a path, or "container!entry".
    virtual std::string Name() const = 0;

    // Reads `size` bytes or fewer if the source ends first.
    std::vector<uint8_t> ReadSome(uint64_t offset, size_t size) const;
    // Reads a NUL-terminated string of at most `max` bytes.
    bool ReadCString(uint64_t offset, size_t max, std::string& text) const;
};

using ByteSourcePtr = std::shared_ptr<const ByteSource>;

ByteSourcePtr OpenFile(const std::string& path, std::string* error = nullptr);
ByteSourcePtr FromBytes(std::vector<uint8_t> bytes, std::string name);
// A window of another source (a container entry), without copying it.
ByteSourcePtr Slice(ByteSourcePtr parent, uint64_t offset, uint64_t size, std::string name);

// Streams the source through SHA-256; empty string if a read fails.
std::string Sha256Of(const ByteSource& source);

// Static parsing, decompilation and live debugging are separate
// capabilities, each validated on its own against a named fixture.
enum class CapabilityStatus { Validated, Experimental, Unavailable };
const char* CapabilityStatusName(CapabilityStatus status);

struct Capability {
    std::string name;            // "static" | "decompile" | "live"
    CapabilityStatus status = CapabilityStatus::Unavailable;
    std::string fixture;         // test that validates it, when Validated
    std::string note;
};

struct Child {
    std::string name;            // path inside the container
    uint64_t offset = 0;         // in the container's bytes
    uint64_t size = 0;
    bool readable = true;        // false: stored outside (ASAR unpacked) or compressed without support
    std::string note;
};

struct InspectOptions {
    size_t maxEntries = 4096;    // per list (imports, exports, files, ...)
};

class Provider {
public:
    virtual ~Provider() = default;
    virtual const char* Name() const = 0;       // stable id, e.g. "pe"
    virtual const char* Version() const = 0;
    virtual std::vector<std::string> Formats() const = 0;
    virtual std::vector<Capability> Capabilities() const = 0;
    // 0 = not this format; higher = more specific (a .NET PE beats a PE).
    virtual int Detect(const ByteSource& source) const = 0;
    // Structured, bounded description of the artifact. Sets `error` and
    // returns null only when nothing can be said; partial results carry
    // "warnings".
    virtual json Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const = 0;
    // Containers list their entries; other providers return none.
    virtual std::vector<Child> Children(const ByteSource&, const InspectOptions&, std::string&) const { return {}; }
};

using ProviderPtr = std::shared_ptr<const Provider>;

class Registry {
public:
    void Add(ProviderPtr provider);
    const std::vector<ProviderPtr>& Providers() const { return providers_; }
    // The most specific provider for the bytes, or null.
    ProviderPtr Detect(const ByteSource& source) const;
    ProviderPtr Find(const std::string& name) const;

    // {"schema": "cortex.artifact.v1", "identity": {...}, "provider",
    //  "format", "capabilities", "data" | "error"}.
    json Inspect(const ByteSource& source, const InspectOptions& options = {}) const;
    // Entries of a container artifact, with the same identity block.
    json ListChildren(const ByteSource& source, const InspectOptions& options = {}) const;
    // A child as its own source, for a nested inspect; null if absent or
    // not readable.
    ByteSourcePtr OpenChild(ByteSourcePtr container, const std::string& childName, std::string& error,
                            const InspectOptions& options = {}) const;

private:
    std::vector<ProviderPtr> providers_;
};

// The registry with every built-in provider.
const Registry& DefaultRegistry();

json CapabilitiesJson(const std::vector<Capability>& capabilities);

// Little-endian readers over a source; false when out of range.
bool ReadU16(const ByteSource& source, uint64_t offset, uint16_t& value);
bool ReadU32(const ByteSource& source, uint64_t offset, uint32_t& value);
bool ReadU64(const ByteSource& source, uint64_t offset, uint64_t& value);

std::string HexU64(uint64_t value);

} // namespace cortex::artifacts
