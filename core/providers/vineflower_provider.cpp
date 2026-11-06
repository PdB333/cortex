#include "analysis_provider.h"
#include "process_runner.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <regex>

// Vineflower (Apache-2.0), the Java decompiler (a Fernflower fork), run as
// `java -jar vineflower.jar -s <input> <out>` in a separate JVM.

namespace cortex::providers {
namespace {

namespace fs = std::filesystem;

std::string Tail(const std::string& text, size_t size) { return text.size() <= size ? text : text.substr(text.size() - size); }

class VineflowerProvider final : public AnalysisProvider {
public:
    explicit VineflowerProvider(const Settings& settings) : jar_(settings.vineflowerJar), java_(settings.java) {}

    std::string Name() const override { return "vineflower"; }

    ProviderInfo Detect() override {
        ProviderInfo info;
        info.name = "vineflower";
        info.engine = "Vineflower (Java decompiler)";
        info.license = "Apache-2.0";
        info.formats = {"jar", "class"};
        info.permissions = {"reads_file", "spawns_process", "writes_temp_files", "writes_cache"};
        info.features = {{"decompile", "validated",
                          "Java sources for a .jar or .class. Validated on tests/fixtures/java: the decompiled sources "
                          "compile and every class has the original's javap signatures."}};
        std::string java, where;
        if (!Command(java, where)) {
            info.availability = where;
            return info;
        }
        ProcessSpec spec;
        spec.argv = {java, "-jar", jar_, "--help"};
        spec.timeout = std::chrono::seconds(60);
        const ProcessOutcome run = RunProcess(spec);
        std::smatch match;
        static const std::regex version(R"(Vineflower Decompiler ([0-9][0-9A-Za-z.+-]*))");
        if (!run.started || !std::regex_search(run.output, match, version)) {
            info.availability = where + ": no version banner (" + (run.started ? Tail(run.output, 300) : run.error) + ")";
            return info;
        }
        info.version = match[1];
        info.available = true;
        info.availability = where;
        return info;
    }

    bool NormalizeOptions(const std::string& feature, const json& options, json& out, std::string& error) const override {
        if (feature != "decompile") {
            error = "unknown feature";
            return false;
        }
        if (!options.is_object()) {
            error = "options must be an object";
            return false;
        }
        for (auto it = options.begin(); it != options.end(); ++it)
            if (it.key() != "timeout") {
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
        return true;
    }

    Outcome Run(const Request& request, const RunContext& context) override {
        Outcome outcome;
        std::string java, where;
        if (!Command(java, where)) {
            outcome.error = "engine_unavailable";
            outcome.message = where;
            return outcome;
        }
        const fs::path out = context.workDir / "out";
        std::error_code ec;
        fs::create_directories(out, ec);
        ProcessSpec spec;
        // -s: errors only on the console. The input is an absolute path.
        spec.argv = {java, "-jar", jar_, "-s", request.path, out.u8string()};
        spec.workingDirectory = context.workDir.u8string();
        spec.timeout = std::chrono::seconds(request.options.value("timeout", int64_t{600}));
        if (context.progress) context.progress("vineflower running");
        const ProcessOutcome run = RunProcess(spec, context.cancel);
        outcome.provenance = {{"engine", "Vineflower"}, {"command", spec.argv}, {"exit_code", run.exitCode}, {"milliseconds", run.milliseconds}};
        if (!run.started) {
            outcome.error = "engine_start_failed";
            outcome.message = run.error;
            return outcome;
        }
        if (run.cancelled || run.timedOut) {
            outcome.error = run.cancelled ? "cancelled" : "timeout";
            return outcome;
        }
        json files = json::array();
        size_t total = 0;
        for (fs::recursive_directory_iterator it(out, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file()) continue;
            std::ifstream file(it->path(), std::ios::binary);
            std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            total += text.size();
            if (files.size() >= 50000 || total > (256u << 20)) {
                outcome.error = "output_too_large";
                return outcome;
            }
            files.push_back({{"path", it->path().lexically_relative(out).generic_u8string()}, {"size", text.size()}, {"text", std::move(text)}});
        }
        if (run.exitCode != 0 || files.empty()) {
            outcome.error = "engine_failed";
            outcome.message = Tail(run.output, 2048);
            return outcome;
        }
        std::sort(files.begin(), files.end(), [](const json& a, const json& b) { return a["path"] < b["path"]; });
        outcome.data = {{"files", files}, {"total_bytes", total}};
        outcome.ok = true;
        return outcome;
    }

private:
    bool Command(std::string& java, std::string& where) const {
        if (jar_.empty()) {
            where = "not configured: pass --vineflower <vineflower.jar> or set CORTEX_VINEFLOWER";
            return false;
        }
        std::error_code ec;
        if (!fs::is_regular_file(fs::u8path(jar_), ec)) {
            where = "vineflower jar not found at " + jar_;
            return false;
        }
        java = java_;
        if (java.empty()) {
            if (const char* home = std::getenv("JAVA_HOME")) {
#if defined(_WIN32)
                const fs::path candidate = fs::u8path(home) / "bin" / "java.exe";
#else
                const fs::path candidate = fs::u8path(home) / "bin" / "java";
#endif
                if (fs::is_regular_file(candidate, ec)) java = candidate.u8string();
            }
        }
        if (java.empty()) java = FindOnPath("java");
        if (java.empty()) {
            where = jar_ + " needs java 11+ (JAVA_HOME or PATH)";
            return false;
        }
        where = jar_ + " with " + java;
        return true;
    }

    std::string jar_;
    std::string java_;
};

} // namespace

std::unique_ptr<AnalysisProvider> MakeVineflowerProvider(const Settings& settings) {
    return std::make_unique<VineflowerProvider>(settings);
}

} // namespace cortex::providers
