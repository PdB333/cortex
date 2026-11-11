// Same bounded read-only entry point for libFuzzer and deterministic replay.
// Modes: TAR, gzip, ar/Debian, certificates/CMS, MSFT type libraries, ZIP.
// Input files, archive scripts and COM objects are never executed/activated.
#include "artifacts/artifact.h"
#include "artifacts/archive_provider.h"
#include "artifacts/certificate_provider.h"
#include "artifacts/typelib_provider.h"
#include "artifacts/zip_provider.h"
#include "artifacts/package_metadata.h"
#include <cstdint>
#include <vector>
using namespace cortex::artifacts;
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* input, size_t size) {
    if (!size || size > (1u << 20)) return 0;
    const unsigned mode = input[0] % 8; ++input; --size;
    const char* names[] = {"sample.tar", "sample.gz", "sample.a", "sample.der", "sample.tlb", "sample.zip", "sample.crx", "RELEASES"};
    auto source = FromBytes(std::vector<uint8_t>(input, input + size), names[mode]);
    TarProvider tar; GzipProvider gzip; ArProvider ar; CertificateProvider cert; TypeLibProvider tlb; ZipProvider zip;
    WrappedZipProvider wrapped; SquirrelReleasesProvider squirrel;
    const Provider* providers[] = {&tar, &gzip, &ar, &cert, &tlb, &zip, &wrapped, &squirrel};
    const auto& provider = *providers[mode];
    InspectOptions options; options.maxEntries = 32; std::string error;
    (void)provider.Detect(*source);
    const json report = provider.Inspect(*source, options, error);
    (void)report.dump();
    if (report.is_null()) return 0;
    error.clear(); const auto children = provider.Children(*source, options, error);
    unsigned opened = 0;
    for (const auto& child : children) {
        if (!child.readable) continue;
        if (opened++ >= 2) break;
        error.clear(); const auto inner = provider.OpenChild(source, child, error);
        if (inner) (void)inner->ReadSome(0, 64);
    }
    return 0;
}
