#pragma once

// Native binaries beyond PE and ELF, read offline: Mach-O (thin and fat, load
// commands, segments and sections, symbols, linked dylibs, build version,
// code-signature blobs, encryption info) and COFF object files (.obj, and the
// members of .lib archives). Plus the Go build information and function-name
// table (pclntab) that every Go binary carries, whatever its container:
// names stay recoverable after `-ldflags=-s -w`.

#include "artifact.h"

namespace cortex::artifacts {

class MachOProvider final : public Provider {
public:
    const char* Name() const override { return "macho"; }
    const char* Version() const override { return "1"; }
    std::vector<std::string> Formats() const override { return {"macho", "macho-fat"}; }
    std::vector<Capability> Capabilities() const override;
    int Detect(const ByteSource& source) const override;
    json Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const override;
    std::vector<Child> Children(const ByteSource& source, const InspectOptions& options, std::string& error) const override;
};

class CoffObjectProvider final : public Provider {
public:
    const char* Name() const override { return "coff-object"; }
    const char* Version() const override { return "1"; }
    std::vector<std::string> Formats() const override { return {"coff-object"}; }
    std::vector<Capability> Capabilities() const override;
    int Detect(const ByteSource& source) const override;
    json Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const override;
};

// Go build info and pclntab function names found anywhere in the file, or
// null when the binary is not a Go binary. Bounded scan.
json GoBinaryInfo(const ByteSource& source, size_t maxFunctions = 4096);

} // namespace cortex::artifacts
