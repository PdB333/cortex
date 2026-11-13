// Same bounded read-only entry point for libFuzzer and deterministic replay.
// The first byte picks a provider (stable modes: see kProviders below and
// tests/fuzz/make_seeds.py); the rest is the file. Inputs are never executed,
// unpickled, loaded or replayed: every provider only reads bytes.
#include "artifacts/artifact.h"
#include "artifacts/bytecode_provider.h"
#include "artifacts/capture_provider.h"
#include "artifacts/data_provider.h"
#include "artifacts/document_provider.h"
#include "artifacts/firmware_provider.h"
#include "artifacts/game_provider.h"
#include "artifacts/generic_provider.h"
#include "artifacts/native_more_provider.h"
#include <cstdint>
#include <memory>
#include <vector>
using namespace cortex::artifacts;

namespace {
const std::vector<std::shared_ptr<const Provider>>& Providers() {
    static const std::vector<std::shared_ptr<const Provider>> list = {
        std::make_shared<PycProvider>(),           // 0
        std::make_shared<JavaClassProvider>(),     // 1
        std::make_shared<LuaBytecodeProvider>(),   // 2
        std::make_shared<WasmProvider>(),          // 3
        std::make_shared<SpirvProvider>(),         // 4
        std::make_shared<MachOProvider>(),         // 5
        std::make_shared<CoffObjectProvider>(),    // 6
        std::make_shared<SqliteProvider>(),        // 7
        std::make_shared<BinaryPlistProvider>(),   // 8
        std::make_shared<PickleProvider>(),        // 9
        std::make_shared<NpyProvider>(),           // 10
        std::make_shared<SafetensorsProvider>(),   // 11
        std::make_shared<GgufProvider>(),          // 12
        std::make_shared<OnnxProvider>(),          // 13
        std::make_shared<PcapProvider>(),          // 14
        std::make_shared<IntelHexProvider>(),      // 15
        std::make_shared<SrecProvider>(),          // 16
        std::make_shared<Uf2Provider>(),           // 17
        std::make_shared<DtbProvider>(),           // 18
        std::make_shared<PdfProvider>(),           // 19
        std::make_shared<LnkProvider>(),           // 20
        nullptr,                                   // 21: the Go scanner (stable mode, see below)
        std::make_shared<VpkProvider>(),           // 22
        std::make_shared<QuakePakProvider>(),      // 23
        std::make_shared<WadProvider>(),           // 24
        std::make_shared<RenpyArchiveProvider>(),  // 25
        std::make_shared<GenericProvider>(),       // 26: the reader of last resort
    };
    return list;
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* input, size_t size) {
    if (!size || size > (1u << 20)) return 0;
    const auto& providers = Providers();
    const unsigned mode = input[0] % providers.size();
    ++input; --size;
    auto source = FromBytes(std::vector<uint8_t>(input, input + size), "fuzz-input");
    if (!providers[mode]) {  // the Go scanner runs inside PE, ELF and Mach-O
        (void)GoBinaryInfo(*source, 64).dump();
        return 0;
    }
    const Provider& provider = *providers[mode];
    InspectOptions options; options.maxEntries = 32;
    std::string error;
    (void)provider.Detect(*source);
    const json report = provider.Inspect(*source, options, error);
    (void)report.dump();
    if (report.is_null()) return 0;
    error.clear();
    const auto children = provider.Children(*source, options, error);
    unsigned opened = 0;
    for (const auto& child : children) {
        if (!child.readable || opened++ >= 2) continue;
        error.clear();
        const auto inner = provider.OpenChild(source, child, error);
        if (inner) (void)inner->ReadSome(0, 64);
    }
    return 0;
}
