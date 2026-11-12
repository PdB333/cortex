#pragma once

// Windows registry hives ("regf"): SYSTEM, SOFTWARE, NTUSER.DAT, a file from
// `reg save`. Read offline from the file: the base block (name, last write,
// version, checksum) and the key tree with its values (strings, numbers,
// lists, binary as hex), bounded in depth and size. Loops between cells and
// cells outside the hive are refused, never followed.

#include "artifact.h"

namespace cortex::artifacts {

class HiveProvider final : public Provider {
public:
    const char* Name() const override { return "registry-hive"; }
    const char* Version() const override { return "1"; }
    std::vector<std::string> Formats() const override { return {"registry-hive"}; }
    std::vector<Capability> Capabilities() const override;
    int Detect(const ByteSource& source) const override;
    json Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const override;
};

}  // namespace cortex::artifacts
