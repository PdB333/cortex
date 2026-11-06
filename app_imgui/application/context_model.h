#pragma once

#include "debug_provider.h"
#include "services/runtime_transport.h"
#include "services/value_scanner.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace cortex::application {

// One record of the runtime's shared investigation context (core/context):
// a selection the person made in the desktop, or an observation the person
// or an agent recorded.
struct ContextEntry {
    std::string id;
    uint64_t sequence = 0;
    std::string kind;        // selection | observation | correlation | hypothesis | note
    std::string source;      // desktop | agent | unattributed
    std::string session;
    std::string address;     // "0x..." or empty
    std::string location;    // "module+0xRVA" or empty
    std::string text;        // label for a selection, statement otherwise
    std::string status;      // pointer_only | client_assertion
    uint64_t recordedMs = 0;
};

struct ContextOperation {
    uint64_t sequence = 0;
    std::string tool;
    std::string risk;
    std::string status;
    std::string source;
    uint64_t durationMs = 0;
};

// The desktop's view of the investigation context it shares with MCP
// clients. It talks to an already-connected runtime only: selecting an
// address never loads the runtime into the target.
class InvestigationContextModel {
public:
    explicit InvestigationContextModel(services::RuntimeTransport& payload) : payload_(payload) {}

    void Reset();

    // Publishes the person's selection. `expectedGeneration` is the active
    // target's process generation (0 = unchecked); the runtime refuses the
    // call with stale_target_generation if it belongs to another process.
    bool SelectAddress(uint64_t address, const std::string& label, const std::string& note,
                       uint64_t expectedGeneration, std::string* error = nullptr);

    // Records an observation the person makes from the desktop (for example
    // an artifact they inspected). `evidence` holds {source, reference}
    // objects. The record says what the person saw, never that it is true.
    bool Observe(const std::string& statement, const nlohmann::json& data, const nlohmann::json& evidence,
                 uint64_t expectedGeneration, std::string* error = nullptr);

    // Shares desktop-side state that the runtime cannot see by itself: the
    // value scanner's results or the external debugger's breakpoints.
    // `expectedGeneration` is the generation of the target the state was
    // produced on; a share for another process is refused by the runtime.
    bool Share(const std::string& kind, const nlohmann::json& data, uint64_t expectedGeneration,
               std::string* error = nullptr);

    // What Share("scan") sends: the query, totals and at most `maxRows`
    // listed results with their current and first-scan values (hex bytes).
    static nlohmann::json ScanSummary(const services::ScanState& state, const services::ScanQuery& query,
                                      size_t maxRows = 256);
    // What Share("debugger") sends: breakpoints with hit counters and the
    // paused threads.
    static nlohmann::json DebuggerSummary(const std::string& backend,
                                          const std::vector<DebugBreakpointInfo>& breakpoints,
                                          const std::vector<DebugPausedThread>& paused, size_t maxRows = 128);

    // Reloads the context. Entries are replaced, not appended.
    bool Refresh(uint64_t expectedGeneration, std::string* error = nullptr);

    const std::vector<ContextEntry>& Entries() const { return entries_; }
    const std::vector<ContextOperation>& History() const { return history_; }
    uint64_t Pid() const { return pid_; }
    uint64_t Generation() const { return generation_; }
    uint64_t DroppedEntries() const { return droppedEntries_; }
    const std::string& LastSelectedEntry() const { return lastSelected_; }

private:
    bool Route(const std::string& method, const std::string& path, const nlohmann::json& body,
               nlohmann::json& result, std::string* error);

    services::RuntimeTransport& payload_;
    std::vector<ContextEntry> entries_;
    std::vector<ContextOperation> history_;
    uint64_t pid_ = 0;
    uint64_t generation_ = 0;
    uint64_t droppedEntries_ = 0;
    std::string lastSelected_;
    std::map<std::string, std::string> lastShared_;  // kind -> last payload sent
};

} // namespace cortex::application
