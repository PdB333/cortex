#pragma once

// Investigations (roadmap step 6): one object that holds a testable
// question, a visible plan of tool calls, limits, human approvals and the
// evidence it produced (schema cortex.investigation.v1). It is a journal,
// not a second agent: the AI client stays outside Cortex and runs the
// tools itself through the normal MCP gates. The store only
//   - enforces the state machine
//       planned -> running <-> awaiting_approval
//       running -> completed | failed | inconclusive;  any open -> cancelled
//   - runs the plan's read-only static steps itself on request,
//   - refuses to record a live, input or mutating step that a person has
//     not approved (an approval never comes from MCP: only the desktop or
//     the CLI, i.e. the person at the machine, can give one),
//   - enforces limits (steps, live steps, deadline),
//   - checks that the trials and evidence a step or a conclusion cites
//     exist, and
//   - keeps an append-only journal of every transition with its actor.

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <mutex>
#include <string>

namespace cortex::investigation {

using json = nlohmann::json;

struct Services {
    // Runs a read-only artifact tool for a static step.
    std::function<json(const std::string& tool, const json& arguments)> runStaticTool;
    // {"ok", "run"} for a trial id (the test runner).
    std::function<json(const std::string& id)> trial;
    // {"ok", "record"} for an evidence id (the evidence store).
    std::function<json(const std::string& id)> evidence;
};

// Built-in workflows that compose existing tools. Each step names the tool,
// its capability (static | decompile | live) and its effect
// (read | state | target_input | mutation).
json Templates();

class Store {
public:
    explicit Store(std::filesystem::path directory) : directory_(std::move(directory)) {}

    // From a template ({"workflow", "parameters"}) or an explicit "plan".
    json Create(const json& input, const std::string& actor);
    json Get(const std::string& id) const;
    json List(const std::string& state, size_t limit) const;

    json Start(const std::string& id, int64_t expectedRevision, const std::string& actor);
    // Runs step `index` server-side; only read-only static steps.
    json RunStep(const std::string& id, size_t index, int64_t expectedRevision, const std::string& actor,
                 const Services& services);
    // Records the result of a step the agent ran itself: references to
    // trials, evidence or a tool call summary. Live/input/mutation steps
    // need a person's approval first.
    json StepResult(const json& input, const std::string& actor, const Services& services);
    // A person approves (or rejects) a step. `actor` must be "desktop" or
    // "cli"; the MCP layer never calls this.
    json Approve(const std::string& id, size_t index, bool approve, const std::string& note, const std::string& actor);
    json Skip(const std::string& id, size_t index, int64_t expectedRevision, const std::string& reason,
              const std::string& actor);
    // completed | failed | inconclusive, with a conclusion and evidence ids.
    json Conclude(const json& input, const std::string& actor, const Services& services);
    json Cancel(const std::string& id, int64_t expectedRevision, const std::string& reason, const std::string& actor);

    // A Markdown report of one investigation.
    json Report(const std::string& id) const;

    const std::filesystem::path& Directory() const { return directory_; }

private:
    json Load(const std::string& id) const;
    json Save(json record);
    std::filesystem::path directory_;
    mutable std::mutex mutex_;
};

bool ValidId(const std::string& id);
// Effects that need a person's approval before their result is recorded.
bool NeedsApproval(const std::string& effect);

} // namespace cortex::investigation
