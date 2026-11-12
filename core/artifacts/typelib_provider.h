#pragma once
#include "artifact.h"

namespace cortex::artifacts {
// MSFT type-library metadata. Does not call LoadTypeLib, COM activation,
// registration, or any function from an analyzed file.
class TypeLibProvider final : public Provider {
public:
    const char* Name() const override { return "typelib"; }
    const char* Version() const override { return "1"; }
    std::vector<std::string> Formats() const override { return {"tlb-msft"}; }
    std::vector<Capability> Capabilities() const override;
    int Detect(const ByteSource& source) const override;
    json Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const override;
};
} // namespace cortex::artifacts
