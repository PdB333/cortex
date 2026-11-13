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
#include "bytecode_provider.h"
#include "native_more_provider.h"
#include "data_provider.h"
#include "capture_provider.h"
#include "firmware_provider.h"
#include "document_provider.h"
#include "game_provider.h"
#include "generic_provider.h"

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
        built.Add(std::make_shared<PycProvider>());
        built.Add(std::make_shared<JavaClassProvider>());
        built.Add(std::make_shared<LuaBytecodeProvider>());
        built.Add(std::make_shared<WasmProvider>());
        built.Add(std::make_shared<MachOProvider>());
        built.Add(std::make_shared<CoffObjectProvider>());
        built.Add(std::make_shared<SqliteProvider>());
        built.Add(std::make_shared<BinaryPlistProvider>());
        built.Add(std::make_shared<PickleProvider>());
        built.Add(std::make_shared<NpyProvider>());
        built.Add(std::make_shared<SafetensorsProvider>());
        built.Add(std::make_shared<GgufProvider>());
        built.Add(std::make_shared<OnnxProvider>());
        built.Add(std::make_shared<SpirvProvider>());
        built.Add(std::make_shared<PcapProvider>());
        built.Add(std::make_shared<IntelHexProvider>());
        built.Add(std::make_shared<SrecProvider>());
        built.Add(std::make_shared<Uf2Provider>());
        built.Add(std::make_shared<DtbProvider>());
        built.Add(std::make_shared<PdfProvider>());
        built.Add(std::make_shared<LnkProvider>());
        built.Add(std::make_shared<VpkProvider>());
        built.Add(std::make_shared<QuakePakProvider>());
        built.Add(std::make_shared<WadProvider>());
        built.Add(std::make_shared<RenpyArchiveProvider>());
        // Last resort: claims any file with the lowest score, so a dedicated
        // provider always wins and nothing is left without a description.
        built.Add(std::make_shared<GenericProvider>());
        return built;
    }();
    return registry;
}

} // namespace cortex::artifacts
