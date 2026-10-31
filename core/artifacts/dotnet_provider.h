#pragma once

// .NET assemblies (step 4c): a PE with a CLR header, read as metadata and
// CIL. The PE view stays available under the same result.

#include "artifact.h"

namespace cortex::artifacts {

class DotNetProvider final : public Provider {
public:
    const char* Name() const override { return "dotnet"; }
    const char* Version() const override { return "1"; }
    std::vector<std::string> Formats() const override { return {"dotnet"}; }
    std::vector<Capability> Capabilities() const override;
    int Detect(const ByteSource& source) const override;
    json Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const override;
};

// CIL of the methods matching `query` (a 0x06xxxxxx token, Type::Method or
// a method name) in a .NET assembly: {"ok", "methods": [...]} or an error.
json DisassembleMethods(const ByteSource& source, const std::string& query, size_t maxInstructions);

} // namespace cortex::artifacts
