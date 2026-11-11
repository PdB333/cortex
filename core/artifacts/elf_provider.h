#pragma once
#include "artifact.h"
namespace cortex::artifacts {
// Native ELF32/ELF64, either byte order, read from bytes without dlopen or
// executing its interpreter. Ghidra is not needed for structural inspection.
class ElfProvider final : public Provider {
public:
    const char* Name() const override { return "elf"; }
    const char* Version() const override { return "1"; }
    std::vector<std::string> Formats() const override { return {"elf"}; }
    std::vector<Capability> Capabilities() const override;
    int Detect(const ByteSource& source) const override;
    json Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const override;
};
} // namespace cortex::artifacts
