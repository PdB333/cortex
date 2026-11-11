#pragma once
#include "artifact.h"
namespace cortex::artifacts {
class CompoundProvider final : public Provider {
public:
    const char* Name() const override { return "compound"; }
    const char* Version() const override { return "1"; }
    std::vector<std::string> Formats() const override { return {"cfb", "msi"}; }
    std::vector<Capability> Capabilities() const override;
    int Detect(const ByteSource& source) const override;
    json Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const override;
    std::vector<Child> Children(const ByteSource& source, const InspectOptions& options, std::string& error) const override;
    ByteSourcePtr OpenChild(ByteSourcePtr container, const Child& child, std::string& error) const override;
};
} // namespace cortex::artifacts
