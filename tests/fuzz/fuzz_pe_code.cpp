// PE, CLR metadata, CIL and native code: the parts behind cortex_artifact_cil,
// _functions, _xrefs, _strings and _cfg. Every byte is attacker-controlled.
#include "artifacts/artifact.h"
#include "artifacts/cil.h"
#include "artifacts/dotnet_metadata.h"
#include "artifacts/native_code.h"
#include "artifacts/pe_provider.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace cortex::artifacts;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Keep native-code indexing bounded: the decoder is linear in the size.
    if (size > (1u << 20)) return 0;
    auto source = FromBytes(std::vector<uint8_t>(data, data + size), "input.dll");
    PeFile pe;
    std::string error;
    if (!ParsePe(*source, pe, error)) return 0;

    if (pe.HasDirectory(kPeClr)) {
        dotnet::Metadata metadata;
        if (metadata.Parse(*source, pe, error)) {
            const uint32_t methods = metadata.Rows(dotnet::MethodDef);
            for (uint32_t row = 1; row <= methods && row <= 16; ++row) {
                std::string methodError;
                dotnet::MethodJson(*source, pe, metadata, row, 256, methodError);
            }
            dotnet::FindMethods(metadata, "Cortex.Fixture.Actor::ApplyDamage");
        }
    }

    native::CodeIndex index;
    if (native::BuildIndex(*source, pe, index, error)) {
        const std::map<uint32_t, std::string> annotations;
        native::FunctionsJson(index, "", 32, annotations);
        native::StringsJson(index, "", 32);
        for (size_t i = 0; i < index.functions.size() && i < 4; ++i) {
            const uint32_t rva = index.functions[i].rva;
            native::XrefsJson(index, rva, 16, annotations);
            native::CfgJson(*source, pe, index, rva, {}, 64, annotations);
            native::Fingerprint(*source, pe, index, rva);
        }
    }
    return 0;
}
