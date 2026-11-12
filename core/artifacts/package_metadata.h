#pragma once
#include "artifact.h"
namespace cortex::artifacts {
// Optional structure-based metadata of ZIP packages. No scripts/entry points
// are executed, no referenced URL is fetched, and no extension is installed.
json InspectZipPackage(const ByteSource& source, const std::vector<Child>& children,
                       const InspectOptions& options);
class WrappedZipProvider final : public Provider {
public:
    const char* Name() const override { return "wrapped-zip"; }
    const char* Version() const override { return "1"; }
    std::vector<std::string> Formats() const override { return {"crx2", "crx3", "jmod"}; }
    std::vector<Capability> Capabilities() const override;
    int Detect(const ByteSource& source) const override;
    json Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const override;
    std::vector<Child> Children(const ByteSource& source, const InspectOptions& options, std::string& error) const override;
    ByteSourcePtr OpenChild(ByteSourcePtr source, const Child& child, std::string& error) const override;
};
class SquirrelReleasesProvider final : public Provider {
public:
    const char* Name() const override { return "squirrel-releases"; }
    const char* Version() const override { return "1"; }
    std::vector<std::string> Formats() const override { return {"squirrel-releases"}; }
    std::vector<Capability> Capabilities() const override;
    int Detect(const ByteSource& source) const override;
    json Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const override;
};
} // namespace cortex::artifacts
