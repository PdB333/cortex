#pragma once

// Read-only archive families. Children stay in bounded memory or ByteSource
// slices: no filesystem extraction, external command or artifact execution.
#include "artifact.h"

namespace cortex::artifacts {
class TarProvider final : public Provider {
public:
    const char* Name() const override { return "tar"; }
    const char* Version() const override { return "1"; }
    std::vector<std::string> Formats() const override { return {"tar", "npm-tar"}; }
    std::vector<Capability> Capabilities() const override;
    int Detect(const ByteSource& source) const override;
    json Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const override;
    std::vector<Child> Children(const ByteSource& source, const InspectOptions& options, std::string& error) const override;
};
class GzipProvider final : public Provider {
public:
    const char* Name() const override { return "gzip"; }
    const char* Version() const override { return "1"; }
    std::vector<std::string> Formats() const override { return {"gzip"}; }
    std::vector<Capability> Capabilities() const override;
    int Detect(const ByteSource& source) const override;
    json Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const override;
    std::vector<Child> Children(const ByteSource& source, const InspectOptions& options, std::string& error) const override;
    ByteSourcePtr OpenChild(ByteSourcePtr source, const Child& child, std::string& error) const override;
};
class ArProvider final : public Provider {
public:
    const char* Name() const override { return "ar"; }
    const char* Version() const override { return "1"; }
    std::vector<std::string> Formats() const override { return {"ar", "deb"}; }
    std::vector<Capability> Capabilities() const override;
    int Detect(const ByteSource& source) const override;
    json Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const override;
    std::vector<Child> Children(const ByteSource& source, const InspectOptions& options, std::string& error) const override;
};
} // namespace cortex::artifacts
