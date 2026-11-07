#include "artifact.h"
#include "asar_provider.h"
#include "dotnet_provider.h"
#include "hive_provider.h"
#include "minidump_provider.h"
#include "zip_provider.h"
#include "js_provider.h"
#include "pe_provider.h"

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
        return built;
    }();
    return registry;
}

} // namespace cortex::artifacts
