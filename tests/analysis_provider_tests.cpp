// External analysis providers (roadmap step 8).
//
//   cortex_analysis_provider_tests <test target exe> <Cortex.Fixture.dll> <Cortex.Fixture.jar> <actor.elf>
//   ("-" skips one)
//
// Always: the process runner (exit code, output, timeout, cancellation that
// kills a grandchild), result views, and the job service (cache by SHA-256,
// queueing, cancellation, a file changed during a run) with a fake provider.
//
// With real engines: Ghidra when GHIDRA_INSTALL_DIR is set, ILSpy when
// CORTEX_ILSPYCMD is set. Each run is checked against Cortex's own parsers
// and the fixture source, not against the engine's own claims.
// CORTEX_REQUIRE_ENGINES=1 turns a missing engine into a failure (CI).

#include "artifacts/artifact.h"
#include "artifacts/dotnet_metadata.h"
#include "artifacts/native_code.h"
#include "artifacts/pe_provider.h"
#include "providers/analysis_provider.h"
#include "providers/process_runner.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;
using namespace cortex::providers;
using json = nlohmann::json;

int failures = 0;
std::string self;

#define CHECK(condition)                                                              \
    do {                                                                              \
        if (!(condition)) {                                                           \
            ++failures;                                                               \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #condition \
                      << std::endl;                                                   \
        }                                                                             \
    } while (false)

bool ProcessAlive(long pid) {
#if defined(_WIN32)
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!process) return false;
    const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return alive;
#else
    // A killed process whose parent has not reaped it is a zombie: dead.
    std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
    std::string text;
    if (!std::getline(stat, text)) return kill(static_cast<pid_t>(pid), 0) == 0;
    const size_t close = text.rfind(')');
    return close != std::string::npos && close + 2 < text.size() && text[close + 2] != 'Z' && text[close + 2] != 'X';
#endif
}

long CurrentPid() {
#if defined(_WIN32)
    return static_cast<long>(GetCurrentProcessId());
#else
    return static_cast<long>(getpid());
#endif
}

// Child modes of this executable.
int Child(const std::string& mode, const std::string& argument) {
    if (mode == "echo") {
        std::cout << "out:" << argument << std::endl;
        std::cerr << "err:" << argument << std::endl;
        return 3;
    }
    if (mode == "sleep") {
        std::this_thread::sleep_for(std::chrono::seconds(60));
        return 0;
    }
    if (mode == "spawn") {
        // A grandchild that records its PID, then both wait. It is started
        // the plain way (same POSIX process group; inherited job on Windows).
#if defined(_WIN32)
        std::wstring line = L"\"" + fs::u8path(self).wstring() + L"\" --child grandchild \"" + fs::u8path(argument).wstring() + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
            return 98;
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
#else
        if (std::system(("'" + self + "' --child grandchild '" + argument + "' &").c_str()) != 0) return 98;
#endif
        std::this_thread::sleep_for(std::chrono::seconds(60));
        return 0;
    }
    if (mode == "grandchild") {
        std::ofstream(argument) << CurrentPid();
        std::this_thread::sleep_for(std::chrono::seconds(60));
        return 0;
    }
    if (mode == "env") {
        const char* value = std::getenv("CORTEX_RUNNER_TEST");
        std::cout << (value ? value : "<unset>") << std::endl;
        return 0;
    }
    return 99;
}

void TestRunner(const fs::path& temp) {
    {
        ProcessSpec spec;
        spec.argv = {self, "--child", "echo", "a b \"quoted\" \\"};
        const auto outcome = RunProcess(spec);
        CHECK(outcome.started && outcome.exitCode == 3);
        CHECK(outcome.output.find("out:a b \"quoted\" \\") != std::string::npos);
        CHECK(outcome.output.find("err:a b \"quoted\" \\") != std::string::npos);
    }
    {
        ProcessSpec spec;
        spec.argv = {self, "--child", "env"};
        spec.environment = {{"CORTEX_RUNNER_TEST", "set-by-spec"}};
        const auto outcome = RunProcess(spec);
        CHECK(outcome.output.find("set-by-spec") != std::string::npos);
    }
    {
        ProcessSpec spec;
        spec.argv = {self, "--child", "sleep"};
        spec.timeout = std::chrono::milliseconds(500);
        const auto outcome = RunProcess(spec);
        CHECK(outcome.timedOut && !outcome.cancelled && outcome.milliseconds < 10000);
    }
    {
        ProcessSpec spec;
        spec.argv = {(temp / "missing-engine.exe").u8string()};
        const auto outcome = RunProcess(spec);
        CHECK(!outcome.started || outcome.exitCode == 127);
    }
    {
        // Cancellation kills the child and the grandchild it started.
        const fs::path pidFile = temp / "grandchild.pid";
        ProcessSpec spec;
        spec.argv = {self, "--child", "spawn", pidFile.u8string()};
        std::atomic<bool> cancel{false};
        std::thread canceller([&] {
            for (int i = 0; i < 200 && !fs::exists(pidFile); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            cancel = true;
        });
        const auto outcome = RunProcess(spec, &cancel);
        canceller.join();
        CHECK(outcome.cancelled && outcome.milliseconds < 20000);
        long grandchild = 0;
        std::ifstream(pidFile) >> grandchild;
        CHECK(grandchild > 0);
        bool gone = false;
        for (int i = 0; i < 100 && !(gone = !ProcessAlive(grandchild)); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(gone);
    }
#if defined(_WIN32)
    {
        // Ghidra's Windows launcher is a .bat: run through cmd.exe, with
        // arguments cmd would expand refused.
        const fs::path script = temp / "engine dir" / "echo args.bat";
        fs::create_directories(script.parent_path());
        std::ofstream(script) << "@echo [%~1] [%~2]\r\n@exit /b 4\r\n";
        ProcessSpec spec;
        spec.argv = {script.u8string(), "a b", "C:\\x (1)\\y.z"};
        const auto outcome = RunProcess(spec);
        CHECK(outcome.started && outcome.exitCode == 4);
        CHECK(outcome.output.find("[a b] [C:\\x (1)\\y.z]") != std::string::npos);
        for (const char* unsafe : {"%PATH%", "a&b", "a|b", "a^b", "a>b"}) {
            spec.argv = {script.u8string(), unsafe};
            CHECK(RunProcess(spec).error == "argument_unsafe_for_cmd");
        }
    }
#endif
    CHECK(QuoteArgument("plain") == "plain");
    CHECK(QuoteArgument("a b") == "\"a b\"");
    CHECK(QuoteArgument("a\\\"b") == "\"a\\\\\\\"b\"");
    CHECK(QuoteArgument("dir\\ x\\") == "\"dir\\ x\\\\\"");
    CHECK(SafeForCmd("C:\\Program Files (x86)\\x (1)\\a-b.exe"));
    CHECK(!SafeForCmd("%PATH%") && !SafeForCmd("a\"b") && !SafeForCmd("a!b") && !SafeForCmd("a&b") && !SafeForCmd("a^b"));
}

void TestViews() {
    const json data{{"schema", "x"},
                    {"functions", json::array({{{"rva", "0x10"}, {"name", "Alpha"}},
                                               {{"rva", "0x20"}, {"name", "beta"}, {"text", std::string(5000, 'x')}},
                                               {{"rva", "0x30"}, {"name", "Gamma"}}})}};
    json summary = ViewResult(data, json::object());
    CHECK(summary["summary"]["functions"]["count"] == 3 && summary["summary"]["schema"] == "x");
    json page = ViewResult(data, {{"part", "functions"}, {"offset", 1}, {"limit", 1}});
    CHECK(page["total"] == 3 && page["items"].size() == 1 && page["items"][0]["name"] == "beta" && page["next_offset"] == 2);
    json byRva = ViewResult(data, {{"part", "functions"}, {"rva", "0X0030"}});
    CHECK(byRva["total"] == 1 && byRva["items"][0]["name"] == "Gamma");
    json query = ViewResult(data, {{"part", "functions"}, {"query", "ALP"}});
    CHECK(query["total"] == 1);
    json clipped = ViewResult(data, {{"part", "functions"}, {"rva", "0x20"}, {"max_chars", 300}});
    CHECK(clipped["clipped"] == true && clipped["items"][0]["text"].get<std::string>().size() == 300);
    CHECK(ViewResult(data, {{"part", "schema"}})["error"] == "unknown_part");
    CHECK(ViewResult(data, {{"part", "functions"}, {"rva", "zz"}})["error"] == "invalid_rva");
}

// A provider whose runs are counted and can be held or made to edit the file.
class FakeProvider final : public AnalysisProvider {
public:
    std::atomic<int> runs{0};
    std::atomic<bool> hold{false};
    std::atomic<bool> touchFile{false};

    std::string Name() const override { return "fake"; }
    ProviderInfo Detect() override {
        ProviderInfo info;
        info.name = "fake";
        info.version = "1.0";
        info.available = true;
        info.features = {{"count", "validated", "counts bytes"}};
        return info;
    }
    bool NormalizeOptions(const std::string&, const json& options, json& out, std::string& error) const override {
        if (options.contains("bad")) {
            error = "bad option";
            return false;
        }
        out = {{"scale", options.value("scale", 1)}};
        return true;
    }
    Outcome Run(const Request& request, const RunContext& context) override {
        ++runs;
        Outcome outcome;
        CHECK(fs::is_directory(context.workDir));
        while (hold && !context.cancel->load()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (context.cancel->load()) {
            outcome.error = "cancelled";
            return outcome;
        }
        if (touchFile) std::ofstream(request.path, std::ios::app) << "changed";
        outcome.ok = true;
        outcome.data = {{"bytes", fs::file_size(request.path) * request.options["scale"].get<int>()},
                        {"items", json::array({{{"rva", "0x1"}, {"name", "one"}}})}};
        return outcome;
    }
};

void TestService(const fs::path& temp) {
    Settings settings;
    settings.cacheDir = temp / "cache";
    settings.tempDir = temp / "work";
    settings.maxConcurrentJobs = 1;
    Service service(settings);
    auto fakeOwned = std::make_unique<FakeProvider>();
    FakeProvider* fake = fakeOwned.get();
    service.Add(std::move(fakeOwned));
    const fs::path file = temp / "input.bin";
    std::ofstream(file, std::ios::binary) << "0123456789";

    const json providers = service.Providers();
    bool listed = false;
    for (const auto& row : providers["providers"]) listed = listed || (row["name"] == "fake" && row["linked"] == false);
    CHECK(listed);

    CHECK(service.Start("nope", {file.u8string(), "count"})["error"] == "unknown_provider");
    CHECK(service.Start("fake", {file.u8string(), "other"})["error"] == "unknown_feature");
    CHECK(service.Start("fake", {file.u8string(), "count", {{"bad", 1}}})["error"] == "invalid_options");
    CHECK(service.Start("fake", {(temp / "absent").u8string(), "count"})["error"] == "file_not_found");

    json first = service.Start("fake", {file.u8string(), "count", {{"scale", 2}}});
    CHECK(first["ok"] == true && first["cache_hit"] == false);
    CHECK(service.Wait(first["job_id"], std::chrono::seconds(10)));
    json done = service.Get(first["job_id"]);
    CHECK(done["state"] == "completed" && done["result"]["summary"]["bytes"] == 20);
    CHECK(done["provenance"]["sha256"] == FileSha256(file) && done["provenance"]["provider"] == "fake");
    json items = service.Get(first["job_id"], {{"part", "items"}, {"rva", "0x1"}});
    CHECK(items["result"]["items"][0]["name"] == "one");

    // Same file and options: the cache answers; other options run again.
    json cached = service.Start("fake", {file.u8string(), "count", {{"scale", 2}}});
    CHECK(cached["cache_hit"] == true && cached["state"] == "completed" && fake->runs == 1);
    CHECK(service.Get(cached["job_id"])["result"]["summary"]["bytes"] == 20);
    json other = service.Start("fake", {file.u8string(), "count", {{"scale", 3}}});
    service.Wait(other["job_id"], std::chrono::seconds(10));
    CHECK(fake->runs == 2 && service.Get(other["job_id"])["result"]["summary"]["bytes"] == 30);
    // A new service (restart) still has the cache on disk.
    {
        Service restarted(settings);
        auto again = std::make_unique<FakeProvider>();
        FakeProvider* second = again.get();
        restarted.Add(std::move(again));
        CHECK(restarted.Start("fake", {file.u8string(), "count", {{"scale", 2}}})["cache_hit"] == true && second->runs == 0);
    }

    {
        // A relative path is resolved before the engine runs elsewhere.
        const fs::path previous = fs::current_path();
        fs::current_path(temp);
        const json relative = service.Start("fake", {"input.bin", "count", {{"scale", 11}}});
        fs::current_path(previous);
        CHECK(relative["ok"] == true);
        service.Wait(relative["job_id"], std::chrono::seconds(10));
        const json done = service.Get(relative["job_id"]);
        CHECK(done["state"] == "completed" && done["path"] == fs::absolute(file).u8string());
    }
    // One slot: the second job queues; cancelling both ends them.
    fake->hold = true;
    std::ofstream(file, std::ios::binary) << "new content";
    json running = service.Start("fake", {file.u8string(), "count"});
    json queued = service.Start("fake", {file.u8string(), "count", {{"scale", 5}}});
    CHECK(running["state"] == "running" && queued["state"] == "queued");
    CHECK(service.Cancel(queued["job_id"])["state"] == "cancelled");
    CHECK(service.Cancel(running["job_id"])["cancel_requested"] == true);
    CHECK(service.Wait(running["job_id"], std::chrono::seconds(10)));
    CHECK(service.Get(running["job_id"])["state"] == "cancelled");
    CHECK(service.Get(queued["job_id"])["state"] == "cancelled");
    fake->hold = false;

    // The engine read a file that changed under it: no result, no cache.
    fake->touchFile = true;
    json changed = service.Start("fake", {file.u8string(), "count", {{"scale", 7}}});
    service.Wait(changed["job_id"], std::chrono::seconds(10));
    json failed = service.Get(changed["job_id"]);
    CHECK(failed["state"] == "failed" && failed["error"] == "file_changed_during_run");
    fake->touchFile = false;
    CHECK(service.Get("missing")["error"] == "unknown_job");
    CHECK(service.Jobs()["jobs"].size() >= 5);
    // Work folders are removed after each run.
    size_t leftovers = 0;
    for (const auto& entry : fs::directory_iterator(settings.tempDir)) (void)entry, ++leftovers;
    CHECK(leftovers == 0);
}

bool RequireEngines() {
    const char* value = std::getenv("CORTEX_REQUIRE_ENGINES");
    return value && std::string(value) == "1";
}

void TestGhidra(const fs::path& temp, const std::string& target) {
    Settings settings;
    settings.cacheDir = temp / "ghidra-cache";
    settings.tempDir = temp / "ghidra-work";
    Service service(settings);
    json info;
    const json providers = service.Providers();
    for (const auto& row : providers["providers"])
        if (row["name"] == "ghidra") info = row;
    if (!info.value("available", false)) {
        std::cout << "ghidra: SKIPPED (" << info.value("availability", std::string()) << ")" << std::endl;
        CHECK(!RequireEngines());
        return;
    }
    std::cout << "ghidra " << info["version"] << ": analysing " << target << std::endl;

    std::string error;
    const auto source = cortex::artifacts::OpenFile(target, &error);
    cortex::artifacts::PeFile pe;
    CHECK(source && cortex::artifacts::ParsePe(*source, pe, error));
    if (!source) return;
    cortex::artifacts::native::CodeIndex index;
    CHECK(cortex::artifacts::native::BuildIndex(*source, pe, index, error));
    uint32_t applyDamage = 0;
    for (const auto& function : index.functions)
        if (function.exportName == "CortexDemoApplyDamage") applyDamage = function.rva;
    CHECK(applyDamage != 0);

    const json job = service.RunAndWait("ghidra", {target, "analyze", {{"decompile_limit", 400}}}, json::object());
    CHECK(job["state"] == "completed");
    if (job["state"] != "completed") {
        std::cerr << job.dump(2).substr(0, 6000) << std::endl;
        return;
    }
    const json summary = job["result"]["summary"];
    CHECK(summary["executable_sha256"] == FileSha256(fs::u8path(target)));
    char base[32];
    snprintf(base, sizeof base, "0x%llx", static_cast<unsigned long long>(pe.imageBase));
    CHECK(summary["image_base"] == base);
    CHECK(summary["language"].get<std::string>().find(pe.machine == 0x8664 ? ":64:" : ":32:") != std::string::npos);

    // Every exported function Cortex finds is a Ghidra function at the same
    // RVA; and (x64) nearly every exception-directory function start too.
    std::set<uint64_t> ghidraEntries;
    for (size_t offset = 0;;) {
        const json page = service.Get(job["job_id"], {{"part", "functions"}, {"limit", 500}, {"offset", offset}});
        for (const auto& row : page["result"]["items"]) ghidraEntries.insert(std::stoull(row["rva"].get<std::string>(), nullptr, 16));
        if (page["result"]["next_offset"].is_null()) break;
        offset = page["result"]["next_offset"];
    }
    size_t exports = 0, exportsFound = 0, pdata = 0, pdataFound = 0;
    for (const auto& function : index.functions) {
        if (!function.exportName.empty() && function.sources.count("export")) {
            ++exports;
            exportsFound += ghidraEntries.count(function.rva);
            if (!ghidraEntries.count(function.rva)) std::cerr << "export not a Ghidra function: " << function.exportName << std::endl;
        }
        if (function.sources.count("pdata")) {
            ++pdata;
            pdataFound += ghidraEntries.count(function.rva);
        }
    }
    std::cout << "ghidra: " << ghidraEntries.size() << " functions; exports " << exportsFound << "/" << exports
              << ", pdata starts " << pdataFound << "/" << pdata << std::endl;
    CHECK(exports >= 5 && exportsFound == exports);
    if (pdata) CHECK(pdataFound * 100 >= pdata * 95);

    // The decompiled health write: the source subtracts from health (+4)
    // and armor (+8) and increments hits (+0xc) of the actor argument.
    char rva[32];
    snprintf(rva, sizeof rva, "0x%x", applyDamage);
    const json decompiled = service.Get(job["job_id"], {{"part", "decompiled"}, {"rva", rva}});
    CHECK(decompiled["result"]["total"] == 1);
    const std::string c = decompiled["result"]["items"][0].value("c", std::string());
    std::cout << c << std::endl;
    CHECK(c.find("CortexDemoApplyDamage") != std::string::npos);
    CHECK(c.find("+ 4)") != std::string::npos && c.find("+ 8)") != std::string::npos && c.find("+ 0xc)") != std::string::npos);
    CHECK(c.find("/ 3") != std::string::npos);

    // Calls are attributed: some function calls CortexDemoApplyDamage.
    bool called = false;
    for (size_t offset = 0; !called;) {
        const json page = service.Get(job["job_id"], {{"part", "functions"}, {"limit", 500}, {"offset", offset}});
        for (const auto& row : page["result"]["items"])
            for (const auto& call : row["calls"]) called = called || call["to"] == rva;
        if (page["result"]["next_offset"].is_null()) break;
        offset = page["result"]["next_offset"];
    }
    CHECK(called);

    // Second request: from the cache, without starting Ghidra.
    const json again = service.Start("ghidra", {target, "analyze", {{"decompile_limit", 400}}});
    CHECK(again["cache_hit"] == true && again["state"] == "completed");

    // Decompiling only the listed function, then cancelling a run.
    const json one = service.RunAndWait("ghidra", {target, "analyze", {{"decompile", {rva}}, {"decompile_limit", 50}}}, json::object());
    CHECK(one["state"] == "completed" && one["result"]["summary"]["decompiled"]["count"] == 1);
    const json slow = service.Start("ghidra", {target, "analyze", {{"decompile_limit", 7}}});
    std::this_thread::sleep_for(std::chrono::seconds(5));
    const auto cancelAt = std::chrono::steady_clock::now();
    service.Cancel(slow["job_id"]);
    CHECK(service.Wait(slow["job_id"], std::chrono::seconds(60)));
    const double cancelSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - cancelAt).count();
    std::cout << "ghidra: cancelled after " << cancelSeconds << " s" << std::endl;
    CHECK(service.Get(slow["job_id"])["state"] == "cancelled" && cancelSeconds < 30);
    std::error_code ec;
    size_t leftovers = 0;
    for (const auto& entry : fs::directory_iterator(settings.tempDir, ec)) (void)entry, ++leftovers;
    CHECK(leftovers == 0);
}

void TestIlspy(const fs::path& temp, const std::string& assembly) {
    Settings settings;
    settings.cacheDir = temp / "ilspy-cache";
    settings.tempDir = temp / "ilspy-work";
    Service service(settings);
    json info;
    const json providers = service.Providers();
    for (const auto& row : providers["providers"])
        if (row["name"] == "ilspy") info = row;
    if (!info.value("available", false)) {
        std::cout << "ilspy: SKIPPED (" << info.value("availability", std::string()) << ")" << std::endl;
        CHECK(!RequireEngines());
        return;
    }
    std::cout << "ilspy " << info["version"] << std::endl;

    // Types: the same set as Cortex's own metadata reader.
    std::string error;
    const auto source = cortex::artifacts::OpenFile(assembly, &error);
    cortex::artifacts::PeFile pe;
    CHECK(source && cortex::artifacts::ParsePe(*source, pe, error));
    cortex::artifacts::dotnet::Metadata metadata;
    CHECK(metadata.Parse(*source, pe, error));
    std::set<std::string> expected;
    for (uint32_t row = 1; row <= metadata.Rows(cortex::artifacts::dotnet::TypeDef); ++row) {
        std::string name = metadata.TypeDefName(row);
        for (auto& c : name)
            if (c == '/') c = '.';
        expected.insert(name);
    }
    const json types = service.RunAndWait("ilspy", {assembly, "list_types"}, {{"part", "types"}, {"limit", 500}});
    CHECK(types["state"] == "completed");
    std::set<std::string> listed;
    if (types["state"] != "completed") std::cerr << types.dump(2).substr(0, 6000) << std::endl;
    else
        for (const auto& row : types["result"]["items"]) listed.insert(row["name"].get<std::string>());
    CHECK(!listed.empty() && listed == expected);

    // The whole assembly as a project.
    const json project = service.RunAndWait("ilspy", {assembly, "decompile"}, {{"part", "files"}, {"query", "Actor.cs"}});
    CHECK(project["state"] == "completed" && project["result"]["total"] == 1);
    if (project["state"] == "completed" && project["result"]["total"] == 1) {
        const std::string actor = project["result"]["items"][0]["text"];
        CHECK(actor.find("public void ApplyDamage(int amount, DamageKind kind)") != std::string::npos);
        CHECK(actor.find("public static T Pick<T>(IList<T> items, int index) where T : class") != std::string::npos);
    }
    const json csproj = service.Get(project["job_id"], {{"part", "files"}, {"query", ".csproj"}});
    CHECK(csproj["result"]["total"] == 1);

    // One type on its own.
    const json one = service.RunAndWait("ilspy", {assembly, "decompile", {{"type", "Cortex.Fixture.DamageKind"}}},
                                        {{"part", "files"}});
    CHECK(one["state"] == "completed");
    if (one["state"] == "completed")
        CHECK(one["result"]["items"][0]["text"].get<std::string>().find("enum DamageKind") != std::string::npos);
    CHECK(service.Start("ilspy", {assembly, "decompile", {{"type", "--help"}}})["error"] == "invalid_options");
    CHECK(service.Start("ilspy", {assembly, "list_types"})["cache_hit"] == true);
}

// Ghidra on an ELF (tests/fixtures/elf): function entries against nm's
// symbols (actor-expected.json) and the decompiled health write.
void TestGhidraElf(const fs::path& temp, const std::string& elf) {
    Settings settings;
    settings.cacheDir = temp / "ghidra-elf-cache";
    settings.tempDir = temp / "ghidra-elf-work";
    Service service(settings);
    const json providers = service.Providers();
    json info;
    for (const auto& row : providers["providers"])
        if (row["name"] == "ghidra") info = row;
    if (!info.value("available", false)) {
        std::cout << "ghidra (elf): SKIPPED" << std::endl;
        CHECK(!RequireEngines());
        return;
    }
    std::ifstream file(fs::u8path(elf).parent_path() / "actor-expected.json");
    const json expected = json::parse(file, nullptr, false);
    CHECK(expected.is_object());
    if (!expected.is_object()) return;
    const std::string apply = expected["functions"]["CortexDemoApplyDamage"];
    const json job = service.RunAndWait("ghidra", {elf, "analyze", {{"decompile", {apply}}}}, json::object());
    CHECK(job["state"] == "completed");
    if (job["state"] != "completed") {
        std::cerr << job.dump(2).substr(0, 4000) << std::endl;
        return;
    }
    const json summary = job["result"]["summary"];
    CHECK(summary["executable_sha256"] == FileSha256(fs::u8path(elf)));
    CHECK(summary["format"].get<std::string>().find("ELF") != std::string::npos);
    CHECK(summary["language"].get<std::string>().find("x86:LE:64") == 0);
    std::map<std::string, std::string> byName;
    const json functions = service.Get(job["job_id"], {{"part", "functions"}, {"limit", 500}});
    for (const auto& row : functions["result"]["items"]) {
        char normal[32];
        snprintf(normal, sizeof normal, "0x%llx", static_cast<unsigned long long>(std::stoull(row["rva"].get<std::string>(), nullptr, 16)));
        byName[row["name"]] = normal;
    }
    size_t found = 0;
    for (const auto& [name, rva] : expected["functions"].items()) {
        const bool same = byName.count(name) && byName[name] == rva.get<std::string>();
        if (!same) std::cerr << "elf: " << name << " at " << rva << " is not a Ghidra function there" << std::endl;
        found += same ? 1 : 0;
    }
    std::cout << "ghidra (elf): " << found << "/" << expected["functions"].size() << " nm functions at the same RVA" << std::endl;
    CHECK(found == expected["functions"].size());
    const json decompiled = service.Get(job["job_id"], {{"part", "decompiled"}, {"rva", apply}});
    CHECK(decompiled["result"]["total"] == 1);
    const std::string c = decompiled["result"]["total"] == 1 ? decompiled["result"]["items"][0].value("c", std::string()) : "";
    std::cout << c << std::endl;
    CHECK(c.find("+ 4)") != std::string::npos && c.find("+ 8)") != std::string::npos && c.find("+ 0xc)") != std::string::npos);
}

void TestVineflower(const fs::path& temp, const std::string& jar) {
    Settings settings;
    settings.cacheDir = temp / "vineflower-cache";
    settings.tempDir = temp / "vineflower-work";
    Service service(settings);
    json info;
    const json providers = service.Providers();
    for (const auto& row : providers["providers"])
        if (row["name"] == "vineflower") info = row;
    if (!info.value("available", false)) {
        std::cout << "vineflower: SKIPPED (" << info.value("availability", std::string()) << ")" << std::endl;
        CHECK(!RequireEngines());
        return;
    }
    std::cout << "vineflower " << info["version"] << std::endl;
    const json job = service.RunAndWait("vineflower", {jar, "decompile"}, {{"part", "files"}, {"query", "Actor.java"}});
    CHECK(job["state"] == "completed");
    if (job["state"] != "completed") {
        std::cerr << job.dump(2).substr(0, 4000) << std::endl;
        return;
    }
    CHECK(job["result"]["total"] == 1);
    const std::string actor = job["result"]["items"][0].value("text", std::string());
    CHECK(actor.find("public void applyDamage(int") != std::string::npos);
    CHECK(actor.find("public static <T extends CharSequence> T pick(List<T>") != std::string::npos);
    CHECK(actor.find("public static final class Snapshot") != std::string::npos);
    const json kinds = service.Get(job["job_id"], {{"part", "files"}, {"query", "DamageKind.java"}});
    CHECK(kinds["result"]["total"] == 1 && kinds["result"]["items"][0]["text"].get<std::string>().find("enum DamageKind") != std::string::npos);
    CHECK(service.Start("vineflower", {jar, "decompile"})["cache_hit"] == true);
    CHECK(service.Start("vineflower", {jar, "decompile", {{"args", "-x"}}})["error"] == "invalid_options");
}

} // namespace

int main(int argc, char** argv) {
    self = fs::absolute(fs::u8path(argv[0])).u8string();
    if (argc >= 3 && std::string(argv[1]) == "--child") return Child(argv[2], argc > 3 ? argv[3] : "");
    const fs::path temp = fs::temp_directory_path() / ("cortex-analysis-tests-" + std::to_string(CurrentPid()));
    fs::remove_all(temp);
    fs::create_directories(temp);
    TestRunner(temp);
    TestViews();
    TestService(temp);
    if (argc > 1 && std::string(argv[1]) != "-") TestGhidra(temp, argv[1]);
    if (argc > 2 && std::string(argv[2]) != "-") TestIlspy(temp, argv[2]);
    if (argc > 3 && std::string(argv[3]) != "-") TestVineflower(temp, argv[3]);
    if (argc > 4 && std::string(argv[4]) != "-") TestGhidraElf(temp, argv[4]);
    std::error_code ec;
    fs::remove_all(temp, ec);
    if (failures) {
        std::cerr << failures << " failure(s)" << std::endl;
        return 1;
    }
    std::cout << "analysis provider tests passed" << std::endl;
    return 0;
}
