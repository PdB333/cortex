#include "artifact.h"
#include "asar_provider.h"
#include "dotnet_provider.h"
#include "hive_provider.h"
#include "minidump_provider.h"
#include "zip_provider.h"
#include "js_provider.h"
#include "pe_provider.h"
#include "pdb_provider.h"
#include "elf_provider.h"
#include "cab_provider.h"
#include "compound_provider.h"
#include "archive_provider.h"
#include "certificate_provider.h"
#include "typelib_provider.h"
#include "package_metadata.h"

namespace cortex::artifacts {

const Registry& DefaultRegistry() {
    static const Registry registry = [] {
        Registry built;
        built.Add(std::make_shared<PeProvider>());
        built.Add(std::make_shared<AsarProvider>());
        built.Add(std::make_shared<JavaScriptProvider>());
        built.Add(std::make_shared<SourceMapProvider>());
        built.Add(std::make_shared<DotNetProvider>());
        built.Add(std::make_shared<ZipProvider>());
        built.Add(std::make_shared<MinidumpProvider>());
        built.Add(std::make_shared<HiveProvider>());
        built.Add(std::make_shared<PdbProvider>());
        built.Add(std::make_shared<ElfProvider>());
        built.Add(std::make_shared<CabProvider>());
        built.Add(std::make_shared<CompoundProvider>());
        built.Add(std::make_shared<TarProvider>());
        built.Add(std::make_shared<GzipProvider>());
        built.Add(std::make_shared<ArProvider>());
        built.Add(std::make_shared<CertificateProvider>());
        built.Add(std::make_shared<TypeLibProvider>());
        built.Add(std::make_shared<WrappedZipProvider>());
        built.Add(std::make_shared<SquirrelReleasesProvider>());
        return built;
    }();
    return registry;
}

} // namespace cortex::artifacts
