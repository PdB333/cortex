#include "analysis_provider.h"

#include "../artifacts/sha256.h"
#include "../services/user_data.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>

namespace cortex::providers {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr size_t kRetainedJobs = 64;

std::string Env(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "";
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool ParseHex(const json& value, uint64_t& out) {
    if (!value.is_string()) return false;
    std::string text = value.get<std::string>();
    if (text.rfind("0x", 0) == 0 || text.rfind("0X", 0) == 0) text = text.substr(2);
    if (text.empty() || text.size() > 16 || text.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return false;
    out = std::stoull(text, nullptr, 16);
    return true;
}

// Long strings in an item are cut so one view stays bounded.
json Clip(const json& item, size_t maxChars, bool& clipped) {
    if (item.is_string()) {
        const auto& text = item.get_ref<const std::string&>();
        if (text.size() <= maxChars) return item;
        clipped = true;
        return text.substr(0, maxChars);
    }
    if (item.is_object()) {
        json out = json::object();
        for (auto it = item.begin(); it != item.end(); ++it) out[it.key()] = Clip(it.value(), maxChars, clipped);
        return out;
    }
    if (item.is_array()) {
        json out = json::array();
        for (const auto& value : item) out.push_back(Clip(value, maxChars, clipped));
        return out;
    }
    return item;
}

std::string RandomSuffix() {
    std::random_device device;
    std::ostringstream out;
    out << std::hex << device() << device();
    return out.str();
}

} // namespace

void Settings::ApplyEnvironment() {
    if (ghidraDir.empty()) ghidraDir = Env("GHIDRA_INSTALL_DIR");
    if (ilspycmd.empty()) ilspycmd = Env("CORTEX_ILSPYCMD");
    if (dotnet.empty()) dotnet = Env("DOTNET_HOST_PATH");
}

json ProviderInfo::ToJson() const {
    json features_ = json::array();
    for (const auto& feature : features)
        features_.push_back({{"name", feature.name}, {"status", feature.status}, {"description", feature.description}});
    return {{"name", name},         {"engine", engine},          {"license", license},
            {"version", version},   {"available", available},    {"availability", availability},
            {"formats", formats},   {"features", features_},     {"permissions", permissions},
            {"linked", false},      {"invocation", "external process"}};
}

std::string FileSha256(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    artifacts::Sha256 hash;
    std::vector<char> buffer(1 << 16);
    while (file) {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        if (file.gcount() > 0) hash.Update(buffer.data(), static_cast<size_t>(file.gcount()));
    }
    if (file.bad()) return {};
    return artifacts::Sha256::Hex(hash.Final());
}

json ViewResult(const json& data, const json& view) {
    const std::string part = view.value("part", std::string("summary"));
    if (part == "summary") {
        json out = json::object();
        for (auto it = data.begin(); it != data.end(); ++it) {
            if (it.value().is_array()) out[it.key()] = {{"count", it.value().size()}};
            else if (it.value().is_object() && it.value().dump().size() > 4096) out[it.key()] = {{"keys", it.value().size()}};
            else out[it.key()] = it.value();
        }
        return {{"ok", true}, {"part", "summary"}, {"summary", out}};
    }
    if (!data.contains(part) || !data[part].is_array())
        return {{"ok", false}, {"error", "unknown_part"}, {"message", "part must be 'summary' or the name of a result array"}};
    const size_t offset = view.value("offset", size_t{0});
    const size_t limit = std::clamp<size_t>(view.value("limit", size_t{50}), 1, 500);
    const size_t maxChars = std::clamp<size_t>(view.value("max_chars", size_t{20000}), 256, 1u << 20);
    uint64_t rva = 0;
    const bool byRva = view.contains("rva") && ParseHex(view["rva"], rva);
    if (view.contains("rva") && !byRva) return {{"ok", false}, {"error", "invalid_rva"}};
    const std::string query = Lower(view.value("query", std::string()));

    json items = json::array();
    size_t total = 0;
    bool clipped = false;
    for (const auto& item : data[part]) {
        if (byRva) {
            uint64_t value = 0;
            if (!item.is_object() || !ParseHex(item.value("rva", json()), value) || value != rva) continue;
        }
        if (!query.empty()) {
            std::string haystack;
            if (item.is_object())
                for (const char* key : {"name", "path", "signature"})
                    if (item.contains(key) && item[key].is_string()) haystack += Lower(item[key].get<std::string>()) + "\n";
            if (item.is_string()) haystack = Lower(item.get<std::string>());
            if (haystack.find(query) == std::string::npos) continue;
        }
        if (total >= offset && items.size() < limit) items.push_back(Clip(item, maxChars, clipped));
        ++total;
    }
    return {{"ok", true},        {"part", part},   {"total", total},     {"offset", offset},
            {"items", items},    {"clipped", clipped},
            {"next_offset", offset + items.size() < total ? json(offset + items.size()) : json(nullptr)}};
}

struct Service::Job {
    std::string id, provider, version, feature, path, sha256;
    json options;
    std::string state = "queued";
    std::atomic<bool> cancel{false};
    Outcome outcome;
    bool cacheHit = false;
    std::string cacheFile;
    std::string progress;
    double milliseconds = 0;
};

Service::Service(Settings settings) : settings_(std::move(settings)) {
    settings_.ApplyEnvironment();
    Add(MakeGhidraProvider(settings_));
    Add(MakeIlspyProvider(settings_));
}

Service::~Service() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        for (auto& [id, job] : jobs_) job->cancel = true;
    }
    changed_.notify_all();
    for (auto& thread : threads_)
        if (thread.joinable()) thread.join();
}

void Service::Add(std::unique_ptr<AnalysisProvider> provider) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string name = provider->Name();
    detected_.erase(name);
    providers_[name] = std::move(provider);
}

json Service::Providers(bool refresh) {
    json out = json::array();
    std::vector<std::string> names;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [name, provider] : providers_) names.push_back(name);
    }
    for (const auto& name : names) {
        bool known = false;
        ProviderInfo info;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = detected_.find(name);
            known = it != detected_.end() && !refresh;
            if (known) info = it->second;
        }
        if (!known) {
            AnalysisProvider* provider = nullptr;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                provider = providers_[name].get();
            }
            info = provider->Detect();  // may start the engine; not under the lock
            std::lock_guard<std::mutex> lock(mutex_);
            detected_[name] = info;
        }
        out.push_back(info.ToJson());
    }
    return {{"ok", true}, {"providers", out}, {"cache", settings_.cacheDir.u8string()}};
}

fs::path Service::CachePath(const std::string& provider, const std::string& version, const std::string& sha256,
                            const std::string& feature, const json& options) const {
    artifacts::Sha256 hash;
    const std::string key = options.dump();
    hash.Update(key.data(), key.size());
    std::string safeVersion;
    for (char c : version) safeVersion.push_back(std::isalnum(static_cast<unsigned char>(c)) || c == '.' ? c : '_');
    return settings_.cacheDir / provider / safeVersion / sha256 / (feature + "-" + artifacts::Sha256::Hex(hash.Final()).substr(0, 16) + ".json");
}

json Service::Start(const std::string& providerName, const Request& request) {
    Providers(false);
    ProviderInfo info;
    AnalysisProvider* provider = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = providers_.find(providerName);
        if (it == providers_.end()) return {{"ok", false}, {"error", "unknown_provider"}};
        provider = it->second.get();
        info = detected_[providerName];
    }
    if (!info.available)
        return {{"ok", false}, {"error", "provider_unavailable"}, {"message", info.availability}};
    if (std::none_of(info.features.begin(), info.features.end(), [&](const Feature& f) { return f.name == request.feature; }))
        return {{"ok", false}, {"error", "unknown_feature"}};
    json options;
    std::string error;
    if (!provider->NormalizeOptions(request.feature, request.options, options, error))
        return {{"ok", false}, {"error", "invalid_options"}, {"message", error}};
    std::error_code ec;
    if (!fs::is_regular_file(fs::u8path(request.path), ec)) return {{"ok", false}, {"error", "file_not_found"}};
    const std::string sha256 = FileSha256(fs::u8path(request.path));
    if (sha256.empty()) return {{"ok", false}, {"error", "file_unreadable"}};

    auto job = std::make_shared<Job>();
    job->provider = providerName;
    job->version = info.version;
    job->feature = request.feature;
    // Engines run in their own work folder: a relative path would not
    // name the hashed file there.
    job->path = fs::absolute(fs::u8path(request.path), ec).u8string();
    if (ec) return {{"ok", false}, {"error", "file_not_found"}};
    job->sha256 = sha256;
    job->options = options;
    const fs::path cache = CachePath(providerName, info.version, sha256, request.feature, options);
    job->cacheFile = cache.u8string();
    {
        std::ifstream file(cache, std::ios::binary);
        if (file) {
            try {
                const json cached = json::parse(file);
                if (cached.value("sha256", std::string()) == sha256 && cached.contains("data")) {
                    job->outcome.ok = true;
                    job->outcome.data = cached["data"];
                    job->outcome.provenance = cached.value("provenance", json::object());
                    job->cacheHit = true;
                    job->state = "completed";
                }
            } catch (...) {
                // A damaged cache entry is ignored and rewritten.
            }
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return {{"ok", false}, {"error", "service_stopping"}};
    job->id = "analysis-" + std::to_string(++counter_) + "-" + RandomSuffix().substr(0, 8);
    jobs_[job->id] = job;
    order_.push_back(job->id);
    // Forget the oldest finished jobs beyond the retained count.
    while (order_.size() > kRetainedJobs) {
        const auto it = std::find_if(order_.begin(), order_.end(), [&](const std::string& id) {
            const auto& state = jobs_[id]->state;
            return state != "queued" && state != "running";
        });
        if (it == order_.end()) break;
        jobs_.erase(*it);
        order_.erase(it);
    }
    if (job->state == "queued") {
        if (running_ < std::max<size_t>(1, settings_.maxConcurrentJobs)) {
            ++running_;
            job->state = "running";
            threads_.emplace_back([this, job] { Execute(job); });
        }
    }
    return {{"ok", true}, {"job_id", job->id}, {"state", job->state}, {"cache_hit", job->cacheHit}, {"sha256", sha256}};
}

void Service::Execute(const std::shared_ptr<Job>& first) {
    std::shared_ptr<Job> job = first;
    while (job) {
        AnalysisProvider* provider = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            provider = providers_[job->provider].get();
        }
        const auto started = Clock::now();
        Outcome outcome;
        if (job->cancel) {
            outcome.error = "cancelled";
        } else {
            std::error_code ec;
            const fs::path workDir = settings_.tempDir / ("cortex-" + job->provider + "-" + RandomSuffix());
            fs::create_directories(workDir, ec);
            if (ec) {
                outcome.error = "temp_dir_failed";
                outcome.message = workDir.u8string();
            } else {
                RunContext context;
                context.sha256 = job->sha256;
                context.workDir = workDir;
                context.cancel = &job->cancel;
                context.progress = [this, job](const std::string& text) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    job->progress = text;
                };
                Request request{job->path, job->feature, job->options};
                try {
                    outcome = provider->Run(request, context);
                } catch (const std::exception& e) {
                    outcome = Outcome{};
                    outcome.error = "provider_exception";
                    outcome.message = e.what();
                }
                fs::remove_all(workDir, ec);
                // The engine read the file by path; it must still be the
                // file that was hashed.
                if (outcome.ok && FileSha256(fs::u8path(job->path)) != job->sha256) {
                    outcome.ok = false;
                    outcome.error = "file_changed_during_run";
                    outcome.data = json();
                }
            }
        }
        if (job->cancel && !outcome.ok) outcome.error = "cancelled";
        outcome.provenance["provider"] = job->provider;
        outcome.provenance["engine_version"] = job->version;
        outcome.provenance["sha256"] = job->sha256;
        outcome.provenance["feature"] = job->feature;
        outcome.provenance["options"] = job->options;
        if (outcome.ok) {
            const fs::path cache = fs::u8path(job->cacheFile);
            std::error_code ec;
            fs::create_directories(cache.parent_path(), ec);
            const json record{{"schema", "cortex.analysis.v1"}, {"provider", job->provider}, {"version", job->version},
                              {"sha256", job->sha256},          {"feature", job->feature},   {"options", job->options},
                              {"data", outcome.data},           {"provenance", outcome.provenance}};
            std::string error;
            if (ec || !services::WriteFileAtomically(cache, record.dump(), &error)) outcome.provenance["cache_write"] = "failed";
        }
        std::lock_guard<std::mutex> lock(mutex_);
        job->milliseconds = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        job->outcome = std::move(outcome);
        job->state = job->outcome.ok ? "completed" : job->outcome.error == "cancelled" ? "cancelled" : "failed";
        // Take the next queued job on this thread.
        job.reset();
        if (!stopping_)
            for (const auto& id : order_)
                if (jobs_[id]->state == "queued") {
                    job = jobs_[id];
                    job->state = "running";
                    break;
                }
        if (!job) --running_;
        changed_.notify_all();
    }
}

json Service::JobJson(const Job& job, const json& view) const {
    json out{{"ok", true},           {"job_id", job.id},       {"provider", job.provider}, {"engine_version", job.version},
             {"feature", job.feature}, {"path", job.path},     {"sha256", job.sha256},     {"options", job.options},
             {"state", job.state},   {"cache_hit", job.cacheHit}};
    if (!job.progress.empty()) out["progress"] = job.progress;
    if (job.state == "completed") {
        out["provenance"] = job.outcome.provenance;
        out["result"] = ViewResult(job.outcome.data, view);
    } else if (job.state == "failed" || job.state == "cancelled") {
        out["error"] = job.outcome.error;
        if (!job.outcome.message.empty()) out["message"] = job.outcome.message;
        out["provenance"] = job.outcome.provenance;
    }
    if (job.milliseconds > 0) out["milliseconds"] = job.milliseconds;
    return out;
}

json Service::Get(const std::string& jobId, const json& view) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = jobs_.find(jobId);
    if (it == jobs_.end()) return {{"ok", false}, {"error", "unknown_job"}};
    return JobJson(*it->second, view.is_object() ? view : json::object());
}

json Service::Cancel(const std::string& jobId) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = jobs_.find(jobId);
    if (it == jobs_.end()) return {{"ok", false}, {"error", "unknown_job"}};
    auto& job = *it->second;
    if (job.state == "queued") {
        job.state = "cancelled";
        job.outcome.error = "cancelled";
    } else if (job.state == "running") {
        job.cancel = true;  // the runner kills the engine's process tree
    }
    return {{"ok", true}, {"job_id", jobId}, {"state", job.state}, {"cancel_requested", job.state == "running"}};
}

json Service::Jobs() {
    std::lock_guard<std::mutex> lock(mutex_);
    json rows = json::array();
    for (auto it = order_.rbegin(); it != order_.rend(); ++it) {
        const auto& job = *jobs_[*it];
        rows.push_back({{"job_id", job.id}, {"provider", job.provider}, {"feature", job.feature}, {"path", job.path},
                        {"state", job.state}, {"cache_hit", job.cacheHit}});
    }
    return {{"ok", true}, {"jobs", rows}};
}

bool Service::Wait(const std::string& jobId, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, timeout, [&] {
        const auto it = jobs_.find(jobId);
        return it == jobs_.end() || (it->second->state != "queued" && it->second->state != "running");
    });
}

json Service::RunAndWait(const std::string& provider, const Request& request, const json& view) {
    const json started = Start(provider, request);
    if (!started.value("ok", false)) return started;
    const std::string id = started["job_id"];
    Wait(id, std::chrono::hours(4));
    return Get(id, view);
}

} // namespace cortex::providers
