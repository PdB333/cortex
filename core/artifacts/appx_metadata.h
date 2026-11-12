#pragma once
#include "artifact.h"
namespace cortex::artifacts {
// Supplement the ZIP provider with Windows APPX/MSIX/bundle metadata.
// Empty object when no relevant package footprint exists. Signature presence
// is never reported as trusted; matching block hashes are not authentication.
json InspectAppx(const ByteSource& source, const std::vector<Child>& children,
                 const InspectOptions& options);
} // namespace cortex::artifacts
