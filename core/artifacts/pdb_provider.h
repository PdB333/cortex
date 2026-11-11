#pragma once

#include "artifact.h"

namespace cortex::artifacts {

// Offline MSF 7 / native PDB reader. Does not use DIA, dbghelp, the symbol
// server, or execute source-server commands contained in a PDB.
class PdbProvider final : public Provider {
public:
    const char* Name() const override { return "pdb"; }
    const char* Version() const override { return "1"; }
    std::vector<std::string> Formats() const override { return {"pdb"}; }
    std::vector<Capability> Capabilities() const override;
    int Detect(const ByteSource& source) const override;
    json Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const override;
};

} // namespace cortex::artifacts
