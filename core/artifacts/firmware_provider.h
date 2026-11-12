#pragma once

// Firmware and embedded images, read offline: Intel HEX and Motorola
// S-record text images (records, checksums, merged address ranges, entry
// point), UF2 flashing images (blocks, family, ranges) and flattened device
// trees (DTB: header, memory reservations, node tree with properties).

#include "artifact.h"

namespace cortex::artifacts {

#define CORTEX_FIRMWARE_PROVIDER(Class, id, fmt)                                                \
    class Class final : public Provider {                                                      \
    public:                                                                                    \
        const char* Name() const override { return id; }                                       \
        const char* Version() const override { return "1"; }                                   \
        std::vector<std::string> Formats() const override { return {fmt}; }                    \
        std::vector<Capability> Capabilities() const override;                                 \
        int Detect(const ByteSource& source) const override;                                   \
        json Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const override; \
    };

CORTEX_FIRMWARE_PROVIDER(IntelHexProvider, "intel-hex", "ihex")
CORTEX_FIRMWARE_PROVIDER(SrecProvider, "srec", "srec")
CORTEX_FIRMWARE_PROVIDER(Uf2Provider, "uf2", "uf2")
CORTEX_FIRMWARE_PROVIDER(DtbProvider, "device-tree", "dtb")

#undef CORTEX_FIRMWARE_PROVIDER

} // namespace cortex::artifacts
