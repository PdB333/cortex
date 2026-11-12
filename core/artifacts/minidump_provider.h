#pragma once

// Windows minidumps (.dmp, "MDMP"): what a crash or a hang captured. Read
// offline from the file: the system, the process, the exception and the
// faulting address as module+offset, the threads with their instruction and
// stack pointers, the modules with their versions and PDB identities, and
// the memory ranges kept. Unknown streams (Wine's, vendor ones) are listed
// and skipped.

#include "artifact.h"

namespace cortex::artifacts {

class MinidumpProvider final : public Provider {
public:
    const char* Name() const override { return "minidump"; }
    const char* Version() const override { return "1"; }
    std::vector<std::string> Formats() const override { return {"minidump"}; }
    std::vector<Capability> Capabilities() const override;
    int Detect(const ByteSource& source) const override;
    json Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const override;
};

}  // namespace cortex::artifacts
