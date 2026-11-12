#include "dotnet_provider.h"

#include "cil.h"
#include "dotnet_metadata.h"
#include "pe_provider.h"

#include <algorithm>
#include <cctype>

namespace cortex::artifacts {
namespace {

// WinMD is still a CLR PE. Windows Runtime types advertise tdWindowsRuntime;
// compliant metadata roots also contain the "Windows Runtime 1.2" version marker.
constexpr uint32_t kWindowsRuntimeType = 0x4000;

std::string LowerAscii(std::string text) {
    for (char& ch : text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return text;
}

std::string LeafName(const std::string& path) {
    const size_t separator = path.find_last_of("/\\!");
    return separator == std::string::npos ? path : path.substr(separator + 1);
}

json WinRtSummary(const dotnet::Metadata& metadata, const ByteSource& source,
                  const InspectOptions& options, bool& recognized) {
    const bool versionMarker = metadata.version.find("Windows Runtime 1.2") != std::string::npos;
    uint32_t winrtTypes = 0;
    uint32_t invalidPublicTypes = 0;
    json types = json::array();
    for (uint32_t index = 1; index <= metadata.Rows(dotnet::TypeDef); ++index) {
        const uint32_t flags = metadata.Cell(dotnet::TypeDef, index, 0);
        const bool winrt = (flags & kWindowsRuntimeType) != 0;
        const std::string name = metadata.TypeDefName(index);
        if (name == "<Module>") continue;
        // ECMA TypeAttributes.VisibilityMask: Public and NestedPublic.
        const bool publiclyVisible = (flags & 0x7) == 1 || (flags & 0x7) == 2;
        if (publiclyVisible && !winrt) ++invalidPublicTypes;
        if (!winrt) continue;
        ++winrtTypes;
        if (types.size() < options.maxEntries) {
            types.push_back({{"name", name},
                             {"token", HexU64((uint32_t(dotnet::TypeDef) << 24) | index)},
                             {"kind", (flags & 0x20) ? "interface" : "type"},
                             {"public", publiclyVisible}});
        }
    }
    recognized = versionMarker || winrtTypes > 0;
    if (!recognized) return nullptr;

    json result = {{"metadata_version_marker", versionMarker},
                   {"winrt_type_count", winrtTypes},
                   {"public_non_winrt_type_count", invalidPublicTypes},
                   {"types", std::move(types)},
                   {"types_truncated", static_cast<uint64_t>(winrtTypes) > options.maxEntries}};
    json warnings = json::array();
    if (!versionMarker) warnings.push_back("winmd_version_marker_missing");
    if (invalidPublicTypes) warnings.push_back("public_types_without_windows_runtime_flag");
    const std::string leaf = LowerAscii(LeafName(source.Name()));
    const bool namedWinmd = leaf.size() > 6 && leaf.compare(leaf.size() - 6, 6, ".winmd") == 0;
    if (namedWinmd) {
        if (metadata.Rows(dotnet::Assembly)) {
            const std::string assembly = LowerAscii(metadata.String(metadata.Cell(dotnet::Assembly, 1, 7)));
            const bool match = !assembly.empty() && leaf.substr(0, leaf.size() - 6) == assembly;
            result["assembly_name_matches_filename"] = match;
            if (!match) warnings.push_back("assembly_name_filename_mismatch");
        } else {
            warnings.push_back("no_assembly_name");
        }
    }
    if (!warnings.empty()) result["warnings"] = std::move(warnings);
    return result;
}

} // namespace

std::vector<Capability> DotNetProvider::Capabilities() const {
    return {{"static", CapabilityStatus::Validated,
             "cortex_dotnet_tests (C# fixture compared with System.Reflection.Metadata and the runtime OpCodes table)",
             "CLR header, metadata streams and all 45 table layouts; assembly, references, types, fields, methods, "
             "signatures, member references, resources, P/Invoke; CIL disassembly with exception clauses; "
             "WinRT type flags, interfaces, version marker and assembly filename consistency"},
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
        bool winmd = false;
        const json winrt = WinRtSummary(metadata, source, options, winmd);
        if (winmd) {
            data["winrt"] = winrt;
            data["format"] = "winmd";
            return data;
        }
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
