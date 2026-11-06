#include "dotnet_provider.h"

#include "cil.h"
#include "dotnet_metadata.h"
#include "pe_provider.h"

namespace cortex::artifacts {

std::vector<Capability> DotNetProvider::Capabilities() const {
    return {{"static", CapabilityStatus::Validated,
             "cortex_dotnet_tests (C# fixture compared with System.Reflection.Metadata and the runtime OpCodes table)",
             "CLR header, metadata streams and all 45 table layouts; assembly, references, types, fields, methods, "
             "signatures, member references, resources, P/Invoke; CIL disassembly with exception clauses"},
            {"decompile", CapabilityStatus::Unavailable, "",
             "no built-in C# decompiler (CIL is disassembled). External: the ILSpy analysis provider "
             "(cortex_analysis_start provider=ilspy, `cortex provider run ilspy`) when the operator configures ilspycmd"},
            {"live", CapabilityStatus::Unavailable, "",
             "no managed debugging in this file provider; the native engine still attaches to the process. External: "
             "the netcoredbg live provider (cortex_live_attach protocol=dap adapter=netcoredbg, log breakpoints only) "
             "when the operator configures --netcoredbg"}};
}

int DotNetProvider::Detect(const ByteSource& source) const {
    PeFile pe;
    std::string error;
    return ParsePe(source, pe, error) && pe.HasDirectory(kPeClr) ? 58 : 0;
}

json DotNetProvider::Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const {
    // The PE view first: a .NET assembly is still a PE file.
    json data = PeProvider().Inspect(source, options, error);
    if (data.is_null()) return nullptr;
    PeFile pe;
    std::string peError;
    if (!ParsePe(source, pe, peError)) {
        error = peError;
        return nullptr;
    }
    dotnet::Metadata metadata;
    std::string metadataError;
    if (!metadata.Parse(source, pe, metadataError)) {
        data["dotnet"] = {{"error", metadataError}};
    } else {
        data["dotnet"] = metadata.Summary(options);
    }
    data["format"] = "dotnet";
    return data;
}

json DisassembleMethods(const ByteSource& source, const std::string& query, size_t maxInstructions) {
    PeFile pe;
    std::string error;
    if (!ParsePe(source, pe, error)) return {{"ok", false}, {"error", error}};
    dotnet::Metadata metadata;
    if (!metadata.Parse(source, pe, error)) return {{"ok", false}, {"error", error}};
    const auto rows = dotnet::FindMethods(metadata, query);
    if (rows.empty()) return {{"ok", false}, {"error", "method_not_found"}};
    json methods = json::array();
    for (uint32_t row : rows) {
        std::string methodError;
        methods.push_back(dotnet::MethodJson(source, pe, metadata, row, maxInstructions, methodError));
    }
    return {{"ok", true}, {"name", source.Name()}, {"methods", methods},
            {"note", "CIL disassembly (static); names resolved from metadata"}};
}

} // namespace cortex::artifacts
