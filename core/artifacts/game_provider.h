#pragma once

// Game archives, read offline for inventory only: Valve VPK (v1/v2), id
// Software PAK and WAD, and Ren'Py RPA (2.0/3.0; its index is a zlib pickle,
// read as plain data only: anything else in it is refused). Containers list
// their entries; plainly stored entries can be opened for a nested inspect.
// Entries kept outside the file (VPK _NNN archives) are listed, not readable.

#include "artifact.h"

namespace cortex::artifacts {

#define CORTEX_GAME_ARCHIVE(Class, id, fmt)                                                     \
    class Class final : public Provider {                                                      \
    public:                                                                                    \
        const char* Name() const override { return id; }                                       \
        const char* Version() const override { return "1"; }                                   \
        std::vector<std::string> Formats() const override { return {fmt}; }                    \
        std::vector<Capability> Capabilities() const override;                                 \
        int Detect(const ByteSource& source) const override;                                   \
        json Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const override; \
        std::vector<Child> Children(const ByteSource& source, const InspectOptions& options, std::string& error) const override; \
        ByteSourcePtr OpenChild(ByteSourcePtr container, const Child& child, std::string& error) const override; \
    };

CORTEX_GAME_ARCHIVE(VpkProvider, "valve-vpk", "vpk")
CORTEX_GAME_ARCHIVE(QuakePakProvider, "id-pak", "id-pak")
CORTEX_GAME_ARCHIVE(WadProvider, "id-wad", "wad")
CORTEX_GAME_ARCHIVE(RenpyArchiveProvider, "renpy-rpa", "rpa")

#undef CORTEX_GAME_ARCHIVE

} // namespace cortex::artifacts
