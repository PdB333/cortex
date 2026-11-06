#pragma once

// Electron ASAR archives (app.asar): a Chromium pickle holding the header
// size, then a pickle holding a JSON file tree, then the file bytes.
//
//   [u32 4][u32 header_size]
//   [u32 payload][i32 json_length][json, padded to 4]   (header_size bytes)
//   file data, offsets relative to 8 + header_size
//
// Entries marked "unpacked" live in app.asar.unpacked/ beside the archive
// and are listed but not readable from it. Links are listed, never followed.

#include "artifact.h"

namespace cortex::artifacts {

class AsarProvider final : public Provider {
public:
    const char* Name() const override { return "asar"; }
    const char* Version() const override { return "2"; }
    std::vector<std::string> Formats() const override { return {"asar"}; }
    std::vector<Capability> Capabilities() const override;
    int Detect(const ByteSource& source) const override;
    json Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const override;
    std::vector<Child> Children(const ByteSource& source, const InspectOptions& options,
                                std::string& error) const override;
};

// The JSON header and the offset file data starts at; false if not ASAR.
bool ReadAsarHeader(const ByteSource& source, json& header, uint64_t& dataOffset, std::string& error);

} // namespace cortex::artifacts
