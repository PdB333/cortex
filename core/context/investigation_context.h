#pragma once

#include "caller.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace context {

using json = nlohmann::json;

// The process this runtime lives in. generation is the process creation
// time (FILETIME on Windows), the same value target::TargetDescriptor carries,
// so a recycled PID never matches an older context.
struct Identity {
    uint64_t pid = 0;
    uint64_t generation = 0;
    std::string architecture;  // "x86" | "x64"
    std::string processName;
    std::string executablePath;
};

struct Module {
    std::string name;
    uint64_t base = 0;
    uint64_t size = 0;
};

struct Limits {
    size_t maxEntries = 512;    // selections + observations kept
    size_t maxHistory = 256;    // operation history rows kept
    size_t maxLabel = 128;
    size_t maxText = 2048;
    size_t maxDataBytes = 4096; // serialized size of an observation's data
    size_t maxEvidence = 16;
};

// The shared investigation state of one target process (roadmap step 2.2).
//
// It lives in the runtime, inside the target, because that is the one place
// the desktop and every MCP client already reach: an address the person
// selects in the desktop and an observation an agent records are the same
// records, read through the same routes, with no second source of truth.
// Everything here is bounded and in memory; it dies with the process, which
// is exactly the lifetime of the generation it describes. Durable claims
// belong in the project knowledge store.
//
// Records are provenance-carrying claims, never verified facts: a selection
// says "this caller pointed here", an observation says "this caller states
// this". Nothing is promoted automatically.
class InvestigationContext {
public:
    using Publisher = std::function<void(const std::string& type, const json& data)>;
    using Clock = std::function<uint64_t()>;

    explicit InvestigationContext(Limits limits = {});

    void SetPublisher(Publisher publisher);
    void SetClock(Clock clock);

    void SetIdentity(Identity identity);
    Identity GetIdentity() const;
    // {"pid", "generation"} for result envelopes and event data.
    json TargetJson() const;

    // Replaces the module list and publishes module.loaded / module.unloaded
    // for the differences. A change is seen when the list is refreshed, not
    // at the moment the loader mapped the module. The first call establishes
    // the baseline and publishes nothing. Returns {"loaded":[...], "unloaded":[...]}.
    json UpdateModules(std::vector<Module> modules);

    // {"module", "rva", "expression"} for an address inside a known module,
    // or null.
    json Locate(uint64_t address) const;

    // A caller points at an address. The newest selection per caller is the
    // "current" one. Returns an api::response::OperationResult.
    json SelectAddress(const CallerInfo& caller, uint64_t address, const std::string& label,
                       const std::string& note);

    // A caller states something about the target, optionally tied to an
    // address, with bounded structured data and evidence references.
    // input: {"statement", "kind"?, "address"?, "data"?, "evidence"?: [{"source","reference"}]}
    json AddObservation(const CallerInfo& caller, const json& input);

    // Records one completed tool call (name, risk, status, duration).
    void RecordOperation(const CallerInfo& caller, const std::string& tool, const std::string& risk,
                         const std::string& status, uint64_t durationMs);

    // Identity, modules, current selections, entries newer than `sinceEntry`
    // (newest last, at most `limit`) and the operation history.
    json Snapshot(size_t limit = 100, uint64_t sinceEntry = 0) const;

    // True if `expected` is 0 (not checked) or equals this process's
    // generation.
    bool GenerationMatches(uint64_t expected) const;

    void Clear();

private:
    json LocateLocked(uint64_t address) const;
    json AppendEntryLocked(json entry);
    void Publish(const std::string& type, json data) const;
    uint64_t Now() const;

    Limits limits_;
    mutable std::mutex mutex_;
    Publisher publisher_;
    Clock clock_;
    Identity identity_;
    std::vector<Module> modules_;
    bool modulesKnown_ = false;
    std::deque<json> entries_;
    std::deque<json> history_;
    uint64_t nextEntry_ = 1;
    uint64_t nextOperation_ = 1;
    uint64_t droppedEntries_ = 0;
    uint64_t droppedHistory_ = 0;
};

// The process-wide context used by the runtime routes.
InvestigationContext& Global();

// "0x1234" -> 0x1234; accepts decimal and 0x-hex strings and JSON numbers.
bool ParseAddress(const json& value, uint64_t& address);

} // namespace context
