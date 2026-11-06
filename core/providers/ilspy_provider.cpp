#include "analysis_provider.h"
#include "process_runner.h"

#include <algorithm>
#include <fstream>
#include <regex>

// ILSpy's command line (ilspycmd, MIT) run as an external process to
// decompile .NET assemblies to C#. --disable-updatecheck is always passed:
// ilspycmd otherwise contacts NuGet on every run.

namespace cortex::providers {
namespace {

namespace fs = std::filesystem;

std::string Tail(const std::string& text, size_t size) { return text.size() <= size ? text : text.substr(text.size() - size); }

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

class IlspyProvider final : public AnalysisProvider {
public:
    explicit IlspyProvider(const Settings& settings) : configured_(settings.ilspycmd), dotnet_(settings.dotnet) {}

    std::string Name() const override { return "ilspy"; }

    ProviderInfo Detect() override {
        ProviderInfo info;
        info.name = "ilspy";
        info.engine = "ILSpy (ilspycmd)";
        info.license = "MIT";
        info.formats = {"dotnet"};
        info.permissions = {"reads_file", "spawns_process", "writes_temp_files", "writes_cache"};
        info.features = {
            {"decompile", "validated",
             "C# for the whole assembly as a project (one file per type), or one type with option 'type'. Validated on "
             "tests/fixtures/dotnet: the decompiled project builds and its members match the original metadata."},
            {"list_types", "validated", "Type names (classes, interfaces, structs, delegates, enums) as ILSpy sees them."}};
        std::vector<std::string> command;
        std::string where;
        if (!Command(command, where)) {
            info.availability = where;
            return info;
        }
        ProcessSpec spec;
        spec.argv = command;
        spec.argv.push_back("--version");
        spec.argv.push_back("--disable-updatecheck");
        spec.timeout = std::chrono::seconds(60);
        const ProcessOutcome run = RunProcess(spec);
        std::smatch match;
        static const std::regex version(R"(ilspycmd:\s*([0-9][0-9A-Za-z.+-]*))");
        if (!run.started || run.exitCode != 0 || !std::regex_search(run.output, match, version)) {
            info.availability = where + ": --version failed (" + (run.started ? Tail(run.output, 300) : run.error) + ")";
            return info;
        }
        info.version = match[1];
        info.available = true;
        info.availability = where;
        return info;
    }

    bool NormalizeOptions(const std::string& feature, const json& options, json& out, std::string& error) const override {
        if (!options.is_object()) {
            error = "options must be an object";
            return false;
        }
        for (auto it = options.begin(); it != options.end(); ++it)
            if (it.key() != "type" && it.key() != "timeout") {
                error = "unknown option " + it.key();
                return false;
            }
        int64_t timeout = 600;
        if (options.contains("timeout")) {
            if (!options["timeout"].is_number_integer() || options["timeout"].get<int64_t>() < 10 ||
                options["timeout"].get<int64_t>() > 3600) {
                error = "timeout must be an integer in [10, 3600]";
                return false;
            }
            timeout = options["timeout"].get<int64_t>();
        }
        out = {{"timeout", timeout}};
        if (feature == "decompile" && options.contains("type")) {
            // A metadata name: no switches, no paths.
            static const std::regex name(R"([A-Za-z_<][A-Za-z0-9_.`<>+$-]{0,511})");
            if (!options["type"].is_string() || !std::regex_match(options["type"].get<std::string>(), name)) {
                error = "type must be a full type name such as Namespace.Type";
                return false;
            }
            out["type"] = options["type"];
        } else if (options.contains("type")) {
            error = "type applies to decompile only";
            return false;
        }
        return feature == "decompile" || feature == "list_types" || (error = "unknown feature", false);
    }

    Outcome Run(const Request& request, const RunContext& context) override {
        Outcome outcome;
        std::vector<std::string> command;
        std::string where;
        if (!Command(command, where)) {
            outcome.error = "engine_unavailable";
            outcome.message = where;
            return outcome;
        }
        const fs::path out = context.workDir / "out";
        std::error_code ec;
        fs::create_directories(out, ec);
        ProcessSpec spec;
        spec.argv = command;
        spec.argv.push_back("--disable-updatecheck");
        const bool project = request.feature == "decompile" && !request.options.contains("type");
        if (request.feature == "list_types") {
            spec.argv.insert(spec.argv.end(), {"-l", "cisde"});
        } else if (!project) {
            spec.argv.insert(spec.argv.end(), {"-t", request.options["type"].get<std::string>()});
        } else {
            spec.argv.insert(spec.argv.end(), {"-p", "-o", out.u8string()});
        }
        spec.argv.push_back(request.path);
        spec.workingDirectory = context.workDir.u8string();
        spec.timeout = std::chrono::seconds(request.options.value("timeout", int64_t{600}));
        spec.outputLimit = 64u << 20;  // a single type comes back on stdout
        // Keep ilspycmd's own .NET runtime from phoning home.
        spec.environment = {{"DOTNET_CLI_TELEMETRY_OPTOUT", "1"}, {"DOTNET_NOLOGO", "1"}};
        if (context.progress) context.progress("ilspycmd running");
        const ProcessOutcome run = RunProcess(spec, context.cancel);
        outcome.provenance = {{"engine", "ilspycmd"},
                              {"command", spec.argv},
                              {"exit_code", run.exitCode},
                              {"milliseconds", run.milliseconds}};
        if (!run.started) {
            outcome.error = "engine_start_failed";
            outcome.message = run.error;
            return outcome;
        }
        if (run.cancelled || run.timedOut) {
            outcome.error = run.cancelled ? "cancelled" : "timeout";
            return outcome;
        }
        if (run.exitCode != 0) {
            outcome.error = "engine_failed";
            outcome.message = Tail(run.output, 2048);
            return outcome;
        }
        if (run.outputTruncated) {
            outcome.error = "output_too_large";
            return outcome;
        }
        if (request.feature == "list_types") {
            json types = json::array();
            static const std::regex line(R"(^(Class|Interface|Struct|Delegate|Enum) (.+?)\r?$)");
            size_t start = 0;
            while (start < run.output.size()) {
                size_t end = run.output.find('\n', start);
                if (end == std::string::npos) end = run.output.size();
                const std::string text = run.output.substr(start, end - start);
                start = end + 1;
                std::smatch match;
                if (std::regex_match(text, match, line)) types.push_back({{"kind", Lower(match[1])}, {"name", match[2]}});
            }
            outcome.data = {{"types", types}};
        } else if (!project) {
            const std::string type = request.options["type"];
            outcome.data = {{"type", type}, {"files", json::array({{{"path", type + ".cs"}, {"size", run.output.size()}, {"text", run.output}}})}};
        } else {
            json files = json::array();
            size_t total = 0;
            for (fs::recursive_directory_iterator it(out, ec), end; !ec && it != end; it.increment(ec)) {
                if (!it->is_regular_file()) continue;
                const fs::path relative = it->path().lexically_relative(out);
                std::ifstream file(it->path(), std::ios::binary);
                std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                total += text.size();
                if (files.size() >= 20000 || total > (256u << 20)) {
                    outcome.error = "output_too_large";
                    return outcome;
                }
                files.push_back({{"path", relative.generic_u8string()}, {"size", text.size()}, {"text", std::move(text)}});
            }
            std::sort(files.begin(), files.end(), [](const json& a, const json& b) { return a["path"] < b["path"]; });
            outcome.data = {{"files", files}, {"total_bytes", total}};
        }
        outcome.ok = true;
        return outcome;
    }

private:
    // ilspycmd[.exe], or ilspycmd.dll run by dotnet.
    bool Command(std::vector<std::string>& command, std::string& where) const {
        std::string tool = configured_.empty() ? FindOnPath("ilspycmd") : configured_;
        if (tool.empty()) {
            where = "not configured: pass --ilspycmd, set CORTEX_ILSPYCMD or put ilspycmd on PATH";
            return false;
        }
        std::error_code ec;
        if (!fs::is_regular_file(fs::u8path(tool), ec)) {
            where = "ilspycmd not found at " + tool;
            return false;
        }
        if (Lower(fs::u8path(tool).extension().u8string()) == ".dll") {
            const std::string dotnet = !dotnet_.empty() ? dotnet_ : FindOnPath("dotnet");
            if (dotnet.empty()) {
                where = tool + " needs dotnet (DOTNET_HOST_PATH or PATH)";
                return false;
            }
            command = {dotnet, tool};
        } else {
            command = {tool};
        }
        where = tool;
        return true;
    }

    std::string configured_;
    std::string dotnet_;
};

} // namespace

std::unique_ptr<AnalysisProvider> MakeIlspyProvider(const Settings& settings) {
    return std::make_unique<IlspyProvider>(settings);
}

} // namespace cortex::providers
