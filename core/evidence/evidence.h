#pragma once

// Evidence records (roadmap step 5): one record type for what a trial, a
// scan, a breakpoint or a static analysis showed, kept by the host so it
// stays readable without the target process (schema cortex.evidence.v1).
//
// The subject is an artifact identity (SHA-256, name, format) plus an
// optional location: rva:0x..., token:0x..., js:file:line:col, or a
// module+RVA. A relation links two subjects (or two records) with a type
// such as handled_by or compiled_to, so cross-runtime chains (an IPC
// channel handled by a JS function that calls a native export) are one
// graph.
//
// Classes are enforced by the store, never taken on the caller's word:
//   verified_observation  a static result Cortex recomputed and found equal
//                         (origin static/decompiled), or a completed trial
//                         Cortex found in its own results (origin observed)
//   correlation           at least two independent references or a relation
//   hypothesis            a claim to test
//   contradictory         a relation "contradicts" to another record
//   inconclusive          a test that did not decide
//   refuted               needs a reference or a verification
// A request for a class whose requirement is not met is refused with the
// reason. Nothing is upgraded silently, and a later verification that no
// longer reproduces marks the record stale instead of changing its class.

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace cortex::evidence {

using json = nlohmann::json;

// Services the store needs from the host. Each may be empty, in which case
// the matching requirement cannot be met (and says so).
struct Checks {
    // Re-runs a read-only artifact tool: {"ok": true, ...} or an error.
    std::function<json(const std::string& tool, const json& arguments)> runTool;
    // A trial by id, as the test runner returns it ({"ok", "run": {...}}).
    std::function<json(const std::string& id)> trial;
    // SHA-256, name and format of the artifact at path/child (inside the
    // operator's roots), plus a fingerprint when `location` is an rva:
    // {"ok", "sha256", "name", "format", "fingerprint"?}. With location
    // "find:<fingerprint json>" it returns {"ok", "rvas": [...]}: the
    // functions of that artifact with the same fingerprint.
    std::function<json(const std::string& path, const std::string& child, const std::string& location)> identify;
};

class Store {
public:
    explicit Store(std::filesystem::path directory) : directory_(std::move(directory)) {}

    // Creates a record. `input` is the client's request (see
    // docs/evidence.md); `recordedBy` is "agent", "desktop" or "cli".
    json Record(const json& input, const std::string& recordedBy, const Checks& checks);
    // Changes class, statement, references or relations of an existing
    // record at `expected_revision`; the same class rules apply.
    json Update(const json& input, const std::string& recordedBy, const Checks& checks);
    // Re-runs the record's static verification now. Never changes the class;
    // a result that no longer matches marks the record stale.
    json Reverify(const std::string& id, const Checks& checks);

    json Get(const std::string& id) const;
    // Filters: sha256, location, class, origin; newest first, bounded.
    json List(const json& filter, size_t limit) const;
    // {"schema", "records"} or a Markdown report of the same records.
    json Export(const json& filter, const std::string& format) const;
    // Where the records about `oldSha256` stand in another build of the
    // artifact (identified through `checks.identify` on path/child).
    json Compare(const std::string& oldSha256, const std::string& path, const std::string& child,
                 const Checks& checks) const;

    const std::filesystem::path& Directory() const { return directory_; }

private:
    std::vector<json> LoadAll() const;
    std::filesystem::path FileFor(const std::string& id) const;
    json Save(json record);
    std::filesystem::path directory_;
    mutable std::mutex mutex_;
};

// Class -> the existing knowledge status, never higher:
// verified_observation -> observed, refuted -> refuted, everything else ->
// hypothesis. A knowledge status read back is a client assertion: observed
// becomes correlation at most, refuted stays refuted, others hypothesis.
const char* KnowledgeStatusFor(const std::string& evidenceClass);
const char* ClassForKnowledgeStatus(const std::string& knowledgeStatus);

bool ValidClass(const std::string& value);
bool ValidId(const std::string& id);

} // namespace cortex::evidence
