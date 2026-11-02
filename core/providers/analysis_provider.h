#pragma once

// External analysis engines behind one interface (roadmap step 8).
//
// A provider wraps an engine Cortex does not link: it is detected from an
// operator setting (CLI option or environment variable, never from an MCP
// argument), run as a separate process on a file, and its output is turned
// into typed JSON with provenance. Results are cached by the file's SHA-256,
// the engine version and the request options. A run is a background job that
// can be cancelled, which kills the engine's process tree.
//
// Each provider lists its features with a status: "validated" only for
// features the test suite checks against a representative fixture.

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cortex::providers {

using json = nlohmann::json;

// Engine locations chosen by the operator (CLI options, then environment).
struct Settings {
    std::string ghidraDir;   // --ghidra-dir, or GHIDRA_INSTALL_DIR
    std::string ilspycmd;    // --ilspycmd (ilspycmd[.exe] or ilspycmd.dll), or CORTEX_ILSPYCMD, or PATH
    std::string dotnet;      // runs ilspycmd.dll; DOTNET_HOST_PATH or PATH when empty
    std::filesystem::path cacheDir;  // results by SHA-256
    std::filesystem::path tempDir;   // engine projects; a fresh folder per run
    size_t maxConcurrentJobs = 2;

    // Empty fields filled from the environment.
    void ApplyEnvironment();
};

struct Feature {
    std::string name;
    std::string status;  // "validated" | "experimental"
    std::string description;
};

struct ProviderInfo {
    std::string name;      // "ghidra", "ilspy"
    std::string engine;    // "Ghidra", "ILSpy (ilspycmd)"
    std::string license;   // the engine's licence; it is not linked
    std::string version;   // the detected engine version
    bool available = false;
    std::string availability;  // why it is not available, or where it was found
    std::vector<std::string> formats;  // artifact formats it accepts ("pe", "dotnet")
    std::vector<Feature> features;
    std::vector<std::string> permissions;  // what a run does on the host
    json ToJson() const;
};

struct Request {
    std::string path;     // a local file (callers check it against their roots)
    std::string feature;  // one of the provider's features
    json options = json::object();
};

struct RunContext {
    std::string sha256;                // of `path`, computed before the run
    std::filesystem::path workDir;     // fresh, removed after the run
    const std::atomic<bool>* cancel = nullptr;
    std::function<void(const std::string&)> progress;
};

struct Outcome {
    bool ok = false;
    std::string error;     // stable code
    std::string message;   // details (engine log tail)
    json data;             // typed result
    json provenance = json::object();  // command, exit code, durations
};

class AnalysisProvider {
public:
    virtual ~AnalysisProvider() = default;
    virtual std::string Name() const = 0;
    // Detects the engine; may start it (`--version`). Cached by the service.
    virtual ProviderInfo Detect() = 0;
    // Validates and normalizes options; the result is part of the cache key.
    virtual bool NormalizeOptions(const std::string& feature, const json& options, json& normalized,
                                  std::string& error) const = 0;
    virtual Outcome Run(const Request& request, const RunContext& context) = 0;
};

std::unique_ptr<AnalysisProvider> MakeGhidraProvider(const Settings& settings);
std::unique_ptr<AnalysisProvider> MakeIlspyProvider(const Settings& settings);

// Views over a result: scalars and array sizes ("summary"), or one array
// paged and filtered ("functions", "decompiled", "files", ...).
json ViewResult(const json& data, const json& view);

// Jobs over the registered providers.
class Service {
public:
    explicit Service(Settings settings);
    ~Service();
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

    void Add(std::unique_ptr<AnalysisProvider> provider);  // tests
    // Detected providers; `refresh` re-runs detection.
    json Providers(bool refresh = false);
    // Starts (or answers from cache) a job: {"ok", "job_id", "state"}.
    json Start(const std::string& provider, const Request& request);
    // {"state": queued|running|completed|failed|cancelled, ...}; `view`
    // selects a part of a completed result.
    json Get(const std::string& jobId, const json& view = json::object());
    json Cancel(const std::string& jobId);
    json Jobs();
    // Synchronous run for the CLI: start, wait, return Get(view).
    json RunAndWait(const std::string& provider, const Request& request, const json& view);
    // Waits for a job to leave queued/running (tests and the CLI).
    bool Wait(const std::string& jobId, std::chrono::milliseconds timeout);

private:
    struct Job;
    void Execute(const std::shared_ptr<Job>& job);
    std::filesystem::path CachePath(const std::string& provider, const std::string& version, const std::string& sha256,
                                    const std::string& feature, const json& options) const;
    json JobJson(const Job& job, const json& view) const;

    Settings settings_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::map<std::string, std::unique_ptr<AnalysisProvider>> providers_;
    std::map<std::string, ProviderInfo> detected_;
    std::map<std::string, std::shared_ptr<Job>> jobs_;
    std::vector<std::string> order_;
    std::vector<std::thread> threads_;
    size_t running_ = 0;
    uint64_t counter_ = 0;
    bool stopping_ = false;
};

// SHA-256 of a file, hex; "" when it cannot be read.
std::string FileSha256(const std::filesystem::path& path);

} // namespace cortex::providers
