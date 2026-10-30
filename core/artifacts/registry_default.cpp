#include "artifact.h"
#include "asar_provider.h"
#include "pe_provider.h"

namespace cortex::artifacts {

const Registry& DefaultRegistry() {
    static const Registry registry = [] {
        Registry built;
        built.Add(std::make_shared<PeProvider>());
        built.Add(std::make_shared<AsarProvider>());
        return built;
    }();
    return registry;
}

} // namespace cortex::artifacts
