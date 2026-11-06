#pragma once

// cortex_analysis_*: external analysis engines (core/providers) for MCP and
// the `cortex.exe analyze` command line. Engines are located by the operator
// (--ghidra-dir, --ilspycmd, environment); an MCP client chooses a provider,
// a file inside --artifact-root and bounded options, never an executable or
// engine arguments. Runs read the file and write the result cache; they do
// not touch any target process.

#include "artifact_tools.h"

#include "../core/providers/analysis_provider.h"
#include "../core/artifacts/sha256.h"
#include "../core/services/user_data.h"

#include <cstdio>
#include <fstream>
#include <functional>
#include <iostream>

namespace mcp_bridge::analysis {

namespace fs = std::filesystem;
using json = nlohmann::json;
namespace providers = cortex::providers;

inline json ToolDefinitions() {
    const json path{{"type", "string"}, {"description", "File inside an operator-configured --artifact-root."}};
    const json child{{"type", "string"},
                     {"description", "Optional entry inside the file (ZIP/NuGet/ASAR), '!'-separated for nesting; the "
                                     "entry's bytes are extracted to the cache folder and analysed."}};
    const json view{{"part", {{"type", "string"}, {"description", "'summary' (default) or a result array: functions, "
                                                                  "decompiled, imports, types, files"}}},
                    {"rva", {{"type", "string"}, {"description", "Only items at this RVA (hex)"}}},
                    {"query", {{"type", "string"}, {"maxLength", 256}, {"description", "Substring of name/path/signature"}}},
                    {"offset", {{"type", "integer"}, {"minimum", 0}}},
                    {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 500}}},
                    {"max_chars", {{"type", "integer"}, {"minimum", 256}, {"maximum", 1048576}}}};
    json getProperties = view;
    getProperties["job_id"] = {{"type", "string"}};
    return json::array({
        {{"name", "cortex_analysis_providers"},
         {"description", "External analysis engines (Ghidra headless, ILSpy) Cortex can run: availability, version, "
                         "licence, formats, features and their validation status. Engines are configured by the operator "
                         "(--ghidra-dir, --ilspycmd or GHIDRA_INSTALL_DIR/CORTEX_ILSPYCMD); they are never linked."},
         {"inputSchema", {{"type", "object"},
                          {"properties", {{"refresh", {{"type", "boolean"}, {"description", "Re-detect the engines"}}}}},
                          {"additionalProperties", false}}},
         {"_cortex", {{"read_only", true}}}},
        {{"name", "cortex_analysis_start"},
         {"description", "Start an analysis job: ghidra/analyze (functions, imports, calls, types and decompiled C keyed "
                         "by RVA; minutes per binary), ilspy/decompile|list_types (C# for a .NET assembly) or vineflower/decompile (Java "
                         "sources for a .jar/.class). Answered "
                         "from the cache when the same file (SHA-256), engine version and options were analysed before. "
                         "Poll cortex_analysis_get. Reads the file only; no target process is touched. Decompiled code "
                         "is the engine's reconstruction, not the original source."},
         {"inputSchema", {{"type", "object"},
                          {"properties", {{"provider", {{"type", "string"}, {"enum", json::array({"ghidra", "ilspy", "vineflower"})}}},
                                          {"feature", {{"type", "string"}, {"enum", json::array({"analyze", "decompile", "list_types"})}}},
                                          {"path", path},
                                          {"child", child},
                                          {"options", {{"type", "object"},
                                                       {"description", "ghidra: decompile_limit (0-5000), decompile (RVA list), "
                                                                       "decompile_timeout (s), timeout (s), max_cpu. ilspy: type "
                                                                       "(Namespace.Type, decompile only), timeout (s). vineflower: timeout (s)."}}}}},
                          {"required", json::array({"provider", "feature", "path"})},
                          {"additionalProperties", false}}},
         {"_cortex", {{"read_only", true}, {"host_process", true}}}},
        {{"name", "cortex_analysis_get"},
         {"description", "State of an analysis job and, once completed, a bounded view of its result: the summary, or "
                         "one result array paged and filtered by RVA or name (for example part=decompiled, rva=0x14c0). "
                         "Includes provenance (engine, version, command, SHA-256)."},
         {"inputSchema", {{"type", "object"}, {"properties", getProperties}, {"required", json::array({"job_id"})},
                          {"additionalProperties", false}}},
         {"_cortex", {{"read_only", true}}}},
        {{"name", "cortex_analysis_cancel"},
         {"description", "Cancel a queued or running analysis job; the engine's process tree is terminated. Affects only "
                         "Cortex's own engine process, never a target."},
         {"inputSchema", {{"type", "object"}, {"properties", {{"job_id", {{"type", "string"}}}}},
                          {"required", json::array({"job_id"})}, {"additionalProperties", false}}},
         {"_cortex", {{"host_control", true}}}},
        {{"name", "cortex_analysis_map"},
         {"description", "Map RVAs of a completed analysis job (Ghidra functions, decompiled code) to absolute addresses in "
                         "the selected live target. Cortex hashes the files of the target's loaded modules and maps only to a "
                         "module whose on-disk file has the job's SHA-256; otherwise nothing is mapped. A matching file does not "
                         "prove the loaded image is unmodified. Read-only."},
         {"inputSchema", {{"type", "object"},
                          {"properties", {{"job_id", {{"type", "string"}}},
                                          {"rvas", {{"type", "array"}, {"items", {{"type", "string"}, {"maxLength", 20}}}, {"minItems", 1}, {"maxItems", 64}}},
                                          {"_cortex_target", {{"oneOf", json::array({{{"type", "integer"}}, {{"type", "string"}}})}}},
                                          {"_cortex_generation", {{"type", "integer"}, {"minimum", 1}}}}},
                          {"required", json::array({"job_id", "rvas", "_cortex_target", "_cortex_generation"})},
                          {"additionalProperties", false}}},
         {"_cortex", {{"read_only", true}}}},
        {{"name", "cortex_analysis_jobs"},
         {"description", "Analysis jobs of this session, newest first."},
         {"inputSchema", {{"type", "object"}, {"properties", json::object()}, {"additionalProperties", false}}},
         {"_cortex", {{"read_only", true}}}},
    });
}

inline bool IsTool(const std::string& name) { return name.rfind("cortex_analysis_", 0) == 0 && name != "cortex_analysis_map"; }

struct LoadedModule {
    std::string name, path;
    uint64_t base = 0, size = 0;
};

// cortex_analysis_map without the target: `hash(path)` returns
// {"status":"hashed","sha256","file_size"} (mcp_bridge/test_file_version.h).
// Only modules whose file has the job's size are hashed.
inline json MapRvas(const json& job, const json& rvas, const std::vector<LoadedModule>& modules,
                    const std::function<json(const std::string&)>& hash) {
    if (job.value("state", std::string()) != "completed")
        return {{"ok", false}, {"error", "job_not_completed"}};
    const std::string sha = job.value("sha256", std::string());
    std::error_code ec;
    const uint64_t size = fs::file_size(fs::u8path(job.value("path", std::string())), ec);
    json checked = json::array();
    const LoadedModule* match = nullptr;
    for (const auto& module : modules) {
        std::error_code sizeError;
        if (ec || fs::file_size(fs::u8path(module.path), sizeError) != size || sizeError) continue;
        const json digest = hash(module.path);
        checked.push_back({{"module", module.name}, {"status", digest.value("status", std::string())}});
        if (digest.value("status", std::string()) == "hashed" && digest.value("sha256", std::string()) == sha) {
            match = &module;
            break;
        }
    }
    if (!match)
        return {{"ok", false}, {"error", "no_loaded_module_matches"},
                {"message", "No loaded module's file has the analysed file's SHA-256; RVAs are not mapped."},
                {"sha256", sha}, {"modules_checked", checked}};
    json rows = json::array();
    for (const auto& value : rvas) {
        std::string text = value.is_string() ? value.get<std::string>() : "";
        if (text.rfind("0x", 0) == 0 || text.rfind("0X", 0) == 0) text = text.substr(2);
        if (text.empty() || text.size() > 16 || text.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
            rows.push_back({{"rva", value}, {"error", "invalid_rva"}});
            continue;
        }
        const uint64_t rva = std::stoull(text, nullptr, 16);
        if (rva >= match->size) {
            rows.push_back({{"rva", value}, {"error", "outside_module_image"}});
            continue;
        }
        char address[32];
        std::snprintf(address, sizeof address, "0x%llx", static_cast<unsigned long long>(match->base + rva));
        rows.push_back({{"rva", value}, {"address", address}});
    }
    char base[32];
    std::snprintf(base, sizeof base, "0x%llx", static_cast<unsigned long long>(match->base));
    return {{"ok", true}, {"module", match->name}, {"path", match->path}, {"base", base}, {"sha256", sha},
            {"mapped", rows}, {"note", "same file on disk; the loaded image may still differ (patches, relocations applied)"}};
}

inline json View(const json& arguments) {
    json view = json::object();
    for (const char* key : {"part", "rva", "query", "offset", "limit", "max_chars"})
        if (arguments.contains(key)) view[key] = arguments[key];
    return view;
}

// Writes the bytes of `child` inside `path` to the cache folder (by SHA-256)
// so an engine can read it as a file.
inline bool ExtractChild(const std::string& path, const std::string& child, const fs::path& folder, std::string& out,
                         std::string& error) {
    auto source = artifacts::OpenChain(path, child, error);
    if (!source) return false;
    if (source->Size() > (512ull << 20)) {
        error = "child_too_large";
        return false;
    }
    std::string bytes;
    bytes.reserve(static_cast<size_t>(source->Size()));
    for (uint64_t offset = 0; offset < source->Size();) {
        const auto chunk = source->ReadSome(offset, 1 << 20);
        if (chunk.empty()) break;
        bytes.append(chunk.begin(), chunk.end());
        offset += chunk.size();
    }
    if (bytes.size() != source->Size()) {
        error = "child_unreadable";
        return false;
    }
    cortex::artifacts::Sha256 hash;
    hash.Update(bytes.data(), bytes.size());
    std::string name = fs::u8path(child.substr(child.find_last_of("!/\\") == std::string::npos ? 0 : child.find_last_of("!/\\") + 1)).u8string();
    std::string safe;
    for (char c : name) safe.push_back(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_' ? c : '_');
    const fs::path directory = folder / cortex::artifacts::Sha256::Hex(hash.Final());
    std::error_code ec;
    fs::create_directories(directory, ec);
    const fs::path file = directory / (safe.empty() ? "entry.bin" : safe);
    if (!fs::exists(file, ec) && !cortex::services::WriteFileAtomically(file, bytes, &error)) return false;
    out = file.u8string();
    return true;
}

inline json Handle(const std::string& name, const json& arguments, const std::vector<fs::path>& roots, providers::Service* service,
                   const fs::path& extractFolder) {
    if (!service) return artifacts::Failure("analysis_unavailable");
    if (!arguments.is_object()) return artifacts::Failure("invalid_arguments");
    if (name == "cortex_analysis_providers") return service->Providers(arguments.value("refresh", false));
    if (name == "cortex_analysis_jobs") return service->Jobs();
    const std::string jobId = arguments.value("job_id", std::string());
    if (name == "cortex_analysis_get") return service->Get(jobId, View(arguments));
    if (name == "cortex_analysis_cancel") return service->Cancel(jobId);
    if (name != "cortex_analysis_start") return artifacts::Failure("unknown_tool");

    fs::path resolved;
    std::string error;
    if (!artifacts::ResolveInsideRoots(arguments.value("path", std::string()), roots, resolved, error))
        return artifacts::Failure(error, error == "artifact_roots_not_configured"
                                             ? "Start cortex.exe mcp with --artifact-root <directory> to allow file analysis."
                                             : std::string());
    std::string path = resolved.u8string();
    const std::string child = arguments.value("child", std::string());
    if (!child.empty() && !ExtractChild(path, child, extractFolder, path, error)) return artifacts::Failure(error);
    const json options = arguments.contains("options") ? arguments["options"] : json::object();
    json started = service->Start(arguments.value("provider", std::string()),
                                  {path, arguments.value("feature", std::string()), options});
    if (!child.empty()) started["extracted_from"] = {{"path", resolved.u8string()}, {"child", child}};
    return started;
}

// `cortex.exe provider list|run`: the person's own files, so no root restriction; the
// engines come from the options or the environment.
inline int AnalyzeMain(int argc, char** argv, const fs::path& userData) {
    providers::Settings settings;
    settings.cacheDir = userData / "analysis-cache";
    settings.tempDir = fs::temp_directory_path();
    std::string provider, feature, path, child, optionsText = "{}";
    json view = json::object();
    bool list = false;
    auto usage = [] {
        std::cerr << "usage: cortex provider list [--ghidra-dir DIR] [--ilspycmd PATH] [--dotnet PATH]\n"
                     "       cortex provider run ghidra|ilspy|vineflower [--feature analyze|decompile|list_types]\n"
                     "              [--options JSON] [--child ENTRY] [--part P] [--rva HEX] [--query TEXT]\n"
                     "              [--offset N] [--limit N] [--cache DIR] [--ghidra-dir DIR] [--ilspycmd PATH]\n"
                     "              [--dotnet PATH] [--vineflower JAR] [--java PATH] <file>\n";
        return 2;
    };
    int first = 1;
    if (argc > 1 && std::string(argv[1]) == "list") list = true, first = 2;
    else if (argc > 2 && std::string(argv[1]) == "run") provider = argv[2], first = 3;
    else return usage();
    for (int i = first; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](std::string& out) {
            if (i + 1 >= argc) return false;
            out = argv[++i];
            return true;
        };
        std::string value;
        if (arg == "--feature" && next(feature)) {}
        else if (arg == "--options" && next(optionsText)) {}
        else if (arg == "--child" && next(child)) {}
        else if (arg == "--ghidra-dir" && next(settings.ghidraDir)) {}
        else if (arg == "--ilspycmd" && next(settings.ilspycmd)) {}
        else if (arg == "--dotnet" && next(settings.dotnet)) {}
        else if (arg == "--vineflower" && next(settings.vineflowerJar)) {}
        else if (arg == "--java" && next(settings.java)) {}
        else if (arg == "--cache" && next(value)) settings.cacheDir = fs::u8path(value);
        else if ((arg == "--part" || arg == "--rva" || arg == "--query") && next(value)) view[arg.substr(2)] = value;
        else if ((arg == "--offset" || arg == "--limit") && next(value)) {
            try {
                view[arg.substr(2)] = std::stoull(value);
            } catch (...) {
                return usage();
            }
        } else if (!arg.empty() && arg[0] != '-' && path.empty()) path = arg;
        else return usage();
    }
    providers::Service service(settings);
    if (list) {
        std::cout << service.Providers().dump(2) << std::endl;
        return 0;
    }
    if (provider.empty() || path.empty()) return usage();
    if (feature.empty()) feature = provider == "ghidra" ? "analyze" : "decompile";
    json options;
    try {
        options = json::parse(optionsText);
    } catch (const std::exception&) {
        std::cerr << "cortex provider: --options is not JSON\n";
        return 2;
    }
    std::string error;
    if (!child.empty() && !ExtractChild(path, child, settings.cacheDir / "extracted", path, error)) {
        std::cerr << "cortex provider: " << error << "\n";
        return 1;
    }
    const json result = service.RunAndWait(provider, {path, feature, options}, view);
    std::cout << result.dump(2) << std::endl;
    return result.value("state", std::string()) == "completed" ? 0 : 1;
}

} // namespace mcp_bridge::analysis
