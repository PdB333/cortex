#pragma once

// ZIP containers: plain archives and the formats built on them (JAR, APK,
// NuGet packages, Python wheels). Entries are listed from the central
// directory; stored and deflated entries are readable, expanded on open
// with a size cap and checked against their CRC-32. Encrypted entries and
// other compression methods are listed but not readable.

#include "artifact.h"

namespace cortex::artifacts {

class ZipProvider final : public Provider {
public:
    const char* Name() const override { return "zip"; }
    const char* Version() const override { return "3"; }
    std::vector<std::string> Formats() const override { return {"zip", "jar", "apk", "nupkg", "wheel", "appx", "appxbundle", "vsix", "xpi", "webextension", "war", "ear", "python-zipapp"}; }
    std::vector<Capability> Capabilities() const override;
    int Detect(const ByteSource& source) const override;
    json Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const override;
    std::vector<Child> Children(const ByteSource& source, const InspectOptions& options,
                                std::string& error) const override;
    ByteSourcePtr OpenChild(ByteSourcePtr container, const Child& child, std::string& error) const override;
};

// Largest entry expanded in memory when opened.
constexpr uint64_t kZipMaxExpandedEntry = 64ull << 20;

} // namespace cortex::artifacts
