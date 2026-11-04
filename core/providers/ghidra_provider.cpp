#include "analysis_provider.h"
#include "process_runner.h"

#include <algorithm>
#include <fstream>
#include <sstream>

// Ghidra (Apache-2.0) run headless as an external process. Cortex writes its
// own post-script into a fresh temporary project and never opens a project
// of the person's. See docs/analysis-providers.md and docs/third-party.md.

namespace cortex::providers {
namespace {

namespace fs = std::filesystem;

const char* kExportScript = R"JAVA(//Cortex export: functions, imports, call references, composite types and
//decompiled C keyed by RVA. Written by Cortex into its own temporary project.
//@category Cortex
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.framework.Application;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressRange;
import ghidra.program.model.data.Composite;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.DataTypeComponent;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.Program;
import ghidra.program.model.symbol.Reference;

import java.io.OutputStreamWriter;
import java.io.Writer;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.HashSet;
import java.util.Iterator;
import java.util.Set;
import java.util.TreeSet;

public class CortexExport extends GhidraScript {
    private static String q(String s) {
        if (s == null) return "null";
        StringBuilder b = new StringBuilder("\"");
        for (int i = 0; i < s.length(); ++i) {
            char c = s.charAt(i);
            switch (c) {
                case '"': b.append("\\\""); break;
                case '\\': b.append("\\\\"); break;
                case '\n': b.append("\\n"); break;
                case '\r': b.append("\\r"); break;
                case '\t': b.append("\\t"); break;
                default:
                    if (c < 0x20) b.append(String.format("\\u%04x", (int) c));
                    else b.append(c);
            }
        }
        return b.append('"').toString();
    }

    private long base;
    private Program program;

    private boolean inImage(Address a) {
        return a != null && a.isMemoryAddress() && a.getAddressSpace() == program.getAddressFactory().getDefaultAddressSpace();
    }

    private String rva(Address a) { return "\"0x" + Long.toHexString(a.getOffset() - base) + "\""; }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String outPath = args[0];
        int maxDecompile = Integer.parseInt(args[1]);
        int timeout = Integer.parseInt(args[2]);
        Set<Long> only = new HashSet<>();
        for (int i = 3; i < args.length; ++i) only.add(Long.parseLong(args[i].replace("0x", ""), 16));
        program = currentProgram;
        base = program.getImageBase().getOffset();
        StringBuilder o = new StringBuilder(1 << 20);
        o.append("{\"schema\":\"cortex.ghidra.v1\"");
        o.append(",\"ghidra_version\":").append(q(Application.getApplicationVersion()));
        o.append(",\"format\":").append(q(program.getExecutableFormat()));
        o.append(",\"language\":").append(q(program.getLanguageID().getIdAsString()));
        o.append(",\"compiler\":").append(q(program.getCompilerSpec().getCompilerSpecID().getIdAsString()));
        o.append(",\"image_base\":\"0x").append(Long.toHexString(base)).append('"');
        o.append(",\"executable_sha256\":").append(q(program.getExecutableSHA256()));

        o.append(",\"functions\":[");
        boolean first = true;
        Iterator<Function> it = program.getFunctionManager().getFunctions(true);
        while (it.hasNext()) {
            Function f = it.next();
            if (monitor.isCancelled()) break;
            if (!inImage(f.getEntryPoint())) continue;
            if (!first) o.append(',');
            first = false;
            o.append("{\"rva\":").append(rva(f.getEntryPoint()));
            o.append(",\"name\":").append(q(f.getName()));
            o.append(",\"source\":").append(q(f.getSymbol().getSource().toString()));
            o.append(",\"thunk\":").append(f.isThunk());
            o.append(",\"signature\":").append(q(f.getPrototypeString(false, false)));
            o.append(",\"calling_convention\":").append(q(f.getCallingConventionName()));
            o.append(",\"size\":").append(f.getBody().getNumAddresses());
            o.append(",\"ranges\":[");
            boolean firstRange = true;
            for (AddressRange r : f.getBody()) {
                if (!firstRange) o.append(',');
                firstRange = false;
                o.append('[').append(rva(r.getMinAddress())).append(',').append(r.getLength()).append(']');
            }
            o.append("],\"calls\":[");
            Set<String> calls = new TreeSet<>();
            for (Instruction ins : program.getListing().getInstructions(f.getBody(), true)) {
                for (Reference ref : ins.getReferencesFrom()) {
                    if (!ref.getReferenceType().isCall()) continue;
                    Address to = ref.getToAddress();
                    Function callee = program.getFunctionManager().getFunctionAt(to);
                    String target = inImage(to) ? rva(to) : q(callee != null ? callee.getName(true) : to.toString());
                    calls.add("{\"from\":" + rva(ins.getAddress()) + ",\"to\":" + target + "}");
                }
            }
            o.append(String.join(",", calls)).append("]}");
        }
        o.append(']');

        o.append(",\"imports\":[");
        first = true;
        Iterator<Function> ext = program.getFunctionManager().getExternalFunctions();
        while (ext.hasNext()) {
            Function f = ext.next();
            if (!first) o.append(',');
            first = false;
            String library = f.getExternalLocation() != null ? f.getExternalLocation().getLibraryName() : null;
            o.append("{\"library\":").append(q(library)).append(",\"name\":").append(q(f.getName())).append('}');
        }
        o.append(']');

        o.append(",\"types\":[");
        first = true;
        int types = 0;
        Iterator<DataType> all = program.getDataTypeManager().getAllDataTypes();
        while (all.hasNext() && types < 2000) {
            DataType t = all.next();
            if (!(t instanceof Composite)) continue;
            Composite c = (Composite) t;
            if (!first) o.append(',');
            first = false;
            ++types;
            o.append("{\"name\":").append(q(c.getPathName())).append(",\"size\":").append(c.getLength()).append(",\"fields\":[");
            boolean firstField = true;
            for (DataTypeComponent d : c.getDefinedComponents()) {
                if (!firstField) o.append(',');
                firstField = false;
                o.append("{\"offset\":").append(d.getOffset()).append(",\"name\":").append(q(d.getFieldName()))
                 .append(",\"type\":").append(q(d.getDataType().getDisplayName())).append('}');
            }
            o.append("]}");
        }
        o.append(']');

        o.append(",\"decompiled\":[");
        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(program);
        first = true;
        int done = 0;
        it = program.getFunctionManager().getFunctions(true);
        while (it.hasNext() && done < maxDecompile) {
            Function f = it.next();
            if (monitor.isCancelled()) break;
            if (!inImage(f.getEntryPoint()) || f.isThunk()) continue;
            if (!only.isEmpty() && !only.contains(f.getEntryPoint().getOffset() - base)) continue;
            ++done;
            DecompileResults r = decompiler.decompileFunction(f, timeout, monitor);
            if (!first) o.append(',');
            first = false;
            o.append("{\"rva\":").append(rva(f.getEntryPoint()));
            if (r != null && r.decompileCompleted() && r.getDecompiledFunction() != null)
                o.append(",\"c\":").append(q(r.getDecompiledFunction().getC()));
            else
                o.append(",\"error\":").append(q(r == null ? "no result" : r.getErrorMessage()));
            o.append('}');
        }
        decompiler.dispose();
        o.append("],\"decompile_limit_reached\":").append(it.hasNext() && done >= maxDecompile).append('}');

        try (Writer w = new OutputStreamWriter(Files.newOutputStream(Paths.get(outPath)), StandardCharsets.UTF_8)) {
            w.write(o.toString());
        }
        println("Cortex export written: " + outPath);
    }
}
)JAVA";

std::string ReadProperty(const fs::path& file, const std::string& key) {
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind(key + "=", 0) == 0) {
            std::string value = line.substr(key.size() + 1);
            while (!value.empty() && (value.back() == '\r' || value.back() == ' ')) value.pop_back();
            return value;
        }
    }
    return {};
}

std::string Tail(const std::string& text, size_t size) { return text.size() <= size ? text : text.substr(text.size() - size); }

class GhidraProvider final : public AnalysisProvider {
public:
    explicit GhidraProvider(const Settings& settings) : dir_(settings.ghidraDir) {}

    std::string Name() const override { return "ghidra"; }

    ProviderInfo Detect() override {
        ProviderInfo info;
        info.name = "ghidra";
        info.engine = "Ghidra (analyzeHeadless)";
        info.license = "Apache-2.0";
        info.formats = {"pe", "elf"};
        info.permissions = {"reads_file", "spawns_process", "writes_temp_project", "writes_cache"};
        info.features = {{"analyze", "validated",
                          "Auto-analysis in a Cortex-owned temporary project; exports functions (RVA, name, signature, "
                          "body ranges, calls), imports, composite types and decompiled C keyed by RVA. Validated on "
                          "PE x64/x86 (cortex_test_target) and ELF x86-64 (tests/fixtures/elf)."}};
        if (dir_.empty()) {
            info.availability = "not configured: pass --ghidra-dir or set GHIDRA_INSTALL_DIR";
            return info;
        }
        const fs::path root = fs::u8path(dir_);
        std::error_code ec;
        const fs::path launcher = Launcher();
        if (!fs::is_regular_file(launcher, ec)) {
            info.availability = "analyzeHeadless not found under " + dir_;
            return info;
        }
        info.version = ReadProperty(root / "Ghidra" / "application.properties", "application.version");
        if (info.version.empty()) {
            info.availability = "Ghidra/application.properties has no application.version";
            return info;
        }
        const std::string java = std::getenv("JAVA_HOME") ? "JAVA_HOME" : FindOnPath("java").empty() ? "" : "PATH";
        info.available = true;
        info.availability = launcher.u8string() + (java.empty() ? " (no java on PATH or JAVA_HOME; Ghidra needs a JDK " +
                                                                       ReadProperty(root / "Ghidra" / "application.properties",
                                                                                    "application.java.min") + "+)"
                                                                 : " (java from " + java + ")");
        return info;
    }

    bool NormalizeOptions(const std::string& feature, const json& options, json& out, std::string& error) const override {
        if (feature != "analyze") {
            error = "unknown feature";
            return false;
        }
        if (!options.is_object()) {
            error = "options must be an object";
            return false;
        }
        for (auto it = options.begin(); it != options.end(); ++it)
            if (it.key() != "decompile_limit" && it.key() != "decompile" && it.key() != "decompile_timeout" &&
                it.key() != "timeout" && it.key() != "max_cpu") {
                error = "unknown option " + it.key();
                return false;
            }
        auto number = [&](const char* key, int64_t fallback, int64_t low, int64_t high, int64_t& value) {
            value = fallback;
            if (!options.contains(key)) return true;
            if (!options[key].is_number_integer() || options[key].get<int64_t>() < low || options[key].get<int64_t>() > high) {
                error = std::string(key) + " must be an integer in [" + std::to_string(low) + ", " + std::to_string(high) + "]";
                return false;
            }
            value = options[key].get<int64_t>();
            return true;
        };
        int64_t limit, perFunction, timeout, cpu;
        if (!number("decompile_limit", 200, 0, 5000, limit) || !number("decompile_timeout", 30, 1, 300, perFunction) ||
            !number("timeout", 1800, 30, 7200, timeout) || !number("max_cpu", 2, 1, 16, cpu))
            return false;
        json rvas = json::array();
        if (options.contains("decompile")) {
            if (!options["decompile"].is_array() || options["decompile"].size() > 256) {
                error = "decompile must be an array of at most 256 RVAs";
                return false;
            }
            for (const auto& value : options["decompile"]) {
                std::string text = value.is_string() ? value.get<std::string>() : "";
                if (text.rfind("0x", 0) == 0 || text.rfind("0X", 0) == 0) text = text.substr(2);
                if (text.empty() || text.size() > 16 || text.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
                    error = "decompile entries are hexadecimal RVA strings";
                    return false;
                }
                std::ostringstream normal;
                normal << "0x" << std::hex << std::stoull(text, nullptr, 16);
                rvas.push_back(normal.str());
            }
            std::vector<std::string> sorted = rvas.get<std::vector<std::string>>();
            std::sort(sorted.begin(), sorted.end());
            sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
            rvas = sorted;
        }
        out = {{"decompile_limit", limit}, {"decompile", rvas}, {"decompile_timeout", perFunction}, {"timeout", timeout},
               {"max_cpu", cpu}};
        return true;
    }

    Outcome Run(const Request& request, const RunContext& context) override {
        Outcome outcome;
        const std::string launcher = Launcher().u8string();
        const fs::path scripts = context.workDir / "scripts";
        const fs::path project = context.workDir / "project";
        const fs::path output = context.workDir / "export.json";
        std::error_code ec;
        fs::create_directories(scripts, ec);
        fs::create_directories(project, ec);
        {
            std::ofstream file(scripts / "CortexExport.java", std::ios::binary | std::ios::trunc);
            file << kExportScript;
            if (!file) {
                outcome.error = "script_write_failed";
                return outcome;
            }
        }
        const json& options = request.options;
        ProcessSpec spec;
        spec.argv = {launcher,
                     project.u8string(),
                     "cortex",
                     "-import",
                     request.path,
                     "-scriptPath",
                     scripts.u8string(),
                     "-postScript",
                     "CortexExport.java",
                     output.u8string(),
                     std::to_string(options["decompile_limit"].get<int64_t>()),
                     std::to_string(options["decompile_timeout"].get<int64_t>())};
        for (const auto& rva : options["decompile"]) spec.argv.push_back(rva.get<std::string>());
        for (const std::string& extra :
             std::vector<std::string>{"-deleteProject", "-max-cpu", std::to_string(options["max_cpu"].get<int64_t>())})
            spec.argv.push_back(extra);
        spec.workingDirectory = context.workDir.u8string();
        spec.timeout = std::chrono::seconds(options["timeout"].get<int64_t>());
        if (context.progress) context.progress("analyzeHeadless running");
        const ProcessOutcome run = RunProcess(spec, context.cancel);
        outcome.provenance = {{"engine", "Ghidra"},
                              {"command", spec.argv},
                              {"exit_code", run.exitCode},
                              {"milliseconds", run.milliseconds},
                              {"temporary_project", true},
                              {"log_tail", Tail(run.output, 4096)}};
        if (!run.started) {
            outcome.error = "engine_start_failed";
            outcome.message = run.error;
            return outcome;
        }
        if (run.cancelled) {
            outcome.error = "cancelled";
            return outcome;
        }
        if (run.timedOut) {
            outcome.error = "timeout";
            return outcome;
        }
        std::ifstream file(output, std::ios::binary);
        if (!file) {
            outcome.error = "engine_failed";
            outcome.message = Tail(run.output, 2048);
            return outcome;
        }
        try {
            outcome.data = json::parse(file);
        } catch (const std::exception& e) {
            outcome.error = "engine_output_invalid";
            outcome.message = e.what();
            return outcome;
        }
        if (outcome.data.value("schema", std::string()) != "cortex.ghidra.v1") {
            outcome.error = "engine_output_invalid";
            return outcome;
        }
        // Ghidra hashes the bytes it imported; they must be the hashed file.
        std::string engineHash = outcome.data.value("executable_sha256", std::string());
        std::transform(engineHash.begin(), engineHash.end(), engineHash.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (engineHash != context.sha256) {
            outcome.error = "identity_mismatch";
            outcome.message = "Ghidra imported bytes with SHA-256 " + engineHash;
            outcome.data = json();
            return outcome;
        }
        outcome.data["rva_note"] = "RVAs are relative to image_base; map to a loaded module only when its file SHA-256 matches";
        outcome.ok = true;
        return outcome;
    }

private:
    fs::path Launcher() const {
#if defined(_WIN32)
        return fs::u8path(dir_) / "support" / "analyzeHeadless.bat";
#else
        return fs::u8path(dir_) / "support" / "analyzeHeadless";
#endif
    }

    std::string dir_;
};

} // namespace

std::unique_ptr<AnalysisProvider> MakeGhidraProvider(const Settings& settings) {
    return std::make_unique<GhidraProvider>(settings);
}

} // namespace cortex::providers
