#include "investigation_context.h"

#include "../api/response_contract.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <map>
#include <utility>

namespace context {
namespace {

using api::response::Operation;
using api::response::OperationResult;

std::string Hex(uint64_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0x%llx", static_cast<unsigned long long>(value));
    return buffer;
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool BoundedString(const json& value, size_t max, bool required) {
    if (value.is_null()) return !required;
    if (!value.is_string()) return false;
    const auto& text = value.get_ref<const std::string&>();
    return text.size() <= max && (!required || !text.empty());
}

json Provenance(const CallerInfo& caller, uint64_t now) {
    json provenance = {{"source", CallerName(caller.caller)}, {"recorded_ms", now}};
    if (!caller.session.empty()) provenance["session"] = caller.session;
    return provenance;
}

Operation Invalid(const std::string& code, const std::string& cause, json target) {
    Operation operation;
    operation.status = "failed";
    operation.error = code;
    operation.cause = cause;
    operation.target = std::move(target);
    return operation;
}

} // namespace

bool ParseAddress(const json& value, uint64_t& address) {
    try {
        if (value.is_number_unsigned()) { address = value.get<uint64_t>(); return true; }
        if (value.is_number_integer()) {
            const auto signedValue = value.get<int64_t>();
            if (signedValue < 0) return false;
            address = static_cast<uint64_t>(signedValue);
            return true;
        }
        if (!value.is_string()) return false;
        const std::string text = value.get<std::string>();
        // stoull accepts leading blanks and a minus sign (which wraps).
        if (text.empty() || text.size() > 24 || !std::isdigit(static_cast<unsigned char>(text[0]))) return false;
        size_t used = 0;
        address = std::stoull(text, &used, 0);
        return used == text.size();
    } catch (...) {
        return false;
    }
}

InvestigationContext::InvestigationContext(Limits limits) : limits_(limits) {}

void InvestigationContext::SetPublisher(Publisher publisher) {
    std::lock_guard<std::mutex> lock(mutex_);
    publisher_ = std::move(publisher);
}

void InvestigationContext::SetClock(Clock clock) {
    std::lock_guard<std::mutex> lock(mutex_);
    clock_ = std::move(clock);
}

uint64_t InvestigationContext::Now() const {
    if (clock_) return clock_();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count());
}

void InvestigationContext::Publish(const std::string& type, json data) const {
    // Called with mutex_ held; the publisher must not call back into this
    // context (the runtime publisher only appends to the event ring).
    if (!publisher_) return;
    data["pid"] = identity_.pid;
    data["generation"] = identity_.generation;
    publisher_(type, data);
}

void InvestigationContext::SetIdentity(Identity identity) {
    std::lock_guard<std::mutex> lock(mutex_);
    identity_ = std::move(identity);
}

Identity InvestigationContext::GetIdentity() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return identity_;
}

json InvestigationContext::TargetJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {{"pid", identity_.pid}, {"generation", identity_.generation}};
}

bool InvestigationContext::GenerationMatches(uint64_t expected) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return expected == 0 || expected == identity_.generation;
}

json InvestigationContext::UpdateModules(std::vector<Module> modules) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto key = [](const Module& module) { return Lower(module.name) + "@" + Hex(module.base); };
    std::map<std::string, const Module*> before, after;
    for (const auto& module : modules_) before[key(module)] = &module;
    for (const auto& module : modules) after[key(module)] = &module;

    json loaded = json::array(), unloaded = json::array();
    if (modulesKnown_) {
        for (const auto& [name, module] : after)
            if (!before.count(name))
                loaded.push_back({{"name", module->name}, {"base", Hex(module->base)}, {"size", module->size}});
        for (const auto& [name, module] : before)
            if (!after.count(name))
                unloaded.push_back({{"name", module->name}, {"base", Hex(module->base)}, {"size", module->size}});
        for (const auto& row : loaded) Publish("module.loaded", row);
        for (const auto& row : unloaded) Publish("module.unloaded", row);
    }
    std::sort(modules.begin(), modules.end(), [](const Module& a, const Module& b) { return a.base < b.base; });
    modules_ = std::move(modules);
    modulesKnown_ = true;
    return {{"loaded", std::move(loaded)}, {"unloaded", std::move(unloaded)}};
}

json InvestigationContext::LocateLocked(uint64_t address) const {
    for (const auto& module : modules_) {
        if (address < module.base || address - module.base >= module.size) continue;
        const uint64_t rva = address - module.base;
        return {{"module", module.name}, {"rva", Hex(rva)}, {"expression", module.name + "+" + Hex(rva)}};
    }
    return nullptr;
}

json InvestigationContext::Locate(uint64_t address) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return LocateLocked(address);
}

json InvestigationContext::AppendEntryLocked(json entry) {
    entry["id"] = "ctx-" + std::to_string(nextEntry_);
    entry["sequence"] = nextEntry_++;
    entries_.push_back(entry);
    while (entries_.size() > limits_.maxEntries) {
        entries_.pop_front();
        ++droppedEntries_;
    }
    return entry;
}

json InvestigationContext::SelectAddress(const CallerInfo& caller, uint64_t address, const std::string& label,
                                         const std::string& note) {
    std::lock_guard<std::mutex> lock(mutex_);
    const json target = {{"pid", identity_.pid}, {"generation", identity_.generation}};
    if (address == 0) return OperationResult(Invalid("invalid_address", "address must be non-zero", target));
    if (label.size() > limits_.maxLabel || note.size() > limits_.maxText)
        return OperationResult(Invalid("text_too_long", "label or note exceeds its limit", target));

    const uint64_t now = Now();
    json entry = {{"kind", "selection"},
                  {"address", Hex(address)},
                  {"location", LocateLocked(address)},
                  {"label", label},
                  {"note", note},
                  {"status", "pointer_only"},
                  {"provenance", Provenance(caller, now)}};
    entry = AppendEntryLocked(std::move(entry));
    Publish("context.address_selected", {{"entry", entry["id"]},
                                         {"address", entry["address"]},
                                         {"location", entry["location"]},
                                         {"source", CallerName(caller.caller)}});

    Operation operation;
    operation.target = target;
    operation.data = {{"entry", entry}};
    operation.limits = {{"max_entries", limits_.maxEntries}};
    operation.evidence = json::array({entry["id"]});
    return OperationResult(operation);
}

json InvestigationContext::AddObservation(const CallerInfo& caller, const json& input) {
    std::lock_guard<std::mutex> lock(mutex_);
    const json target = {{"pid", identity_.pid}, {"generation", identity_.generation}};
    if (!input.is_object()) return OperationResult(Invalid("invalid_observation", "body must be an object", target));
    for (auto it = input.begin(); it != input.end(); ++it) {
        static const char* allowed[] = {"statement", "kind", "address", "data", "evidence",
                                        "expected_generation", "mutation_permission"};
        if (std::none_of(std::begin(allowed), std::end(allowed), [&](const char* key) { return it.key() == key; }))
            return OperationResult(Invalid("unknown_field", it.key(), target));
    }
    if (!BoundedString(input.value("statement", json()), limits_.maxText, true))
        return OperationResult(Invalid("invalid_statement", "statement is required, at most " +
                                                                std::to_string(limits_.maxText) + " bytes", target));
    // These kinds are what the caller claims the record is; the context
    // never upgrades them. "observation" means "the caller says it saw this".
    const std::string kind = input.value("kind", std::string("observation"));
    if (kind != "observation" && kind != "correlation" && kind != "hypothesis" && kind != "note")
        return OperationResult(Invalid("invalid_kind", "kind is observation, correlation, hypothesis or note", target));

    json location = nullptr;
    json addressText = nullptr;
    if (input.contains("address") && !input["address"].is_null()) {
        uint64_t address = 0;
        if (!ParseAddress(input["address"], address) || address == 0)
            return OperationResult(Invalid("invalid_address", "address must be a non-zero integer or hex string", target));
        addressText = Hex(address);
        location = LocateLocked(address);
    }
    const json data = input.value("data", json::object());
    if (data.dump().size() > limits_.maxDataBytes)
        return OperationResult(Invalid("data_too_large", "data exceeds " + std::to_string(limits_.maxDataBytes) +
                                                             " serialized bytes", target));
    const json evidence = input.value("evidence", json::array());
    if (!evidence.is_array() || evidence.size() > limits_.maxEvidence)
        return OperationResult(Invalid("invalid_evidence", "evidence is an array of at most " +
                                                               std::to_string(limits_.maxEvidence) + " references", target));
    for (const auto& reference : evidence) {
        if (!reference.is_object() || !BoundedString(reference.value("source", json()), 64, true) ||
            !BoundedString(reference.value("reference", json()), 256, true))
            return OperationResult(Invalid("invalid_evidence", "each reference needs source and reference strings", target));
    }

    const uint64_t now = Now();
    json entry = {{"kind", kind},
                  {"statement", input["statement"]},
                  {"address", addressText},
                  {"location", location},
                  {"data", data},
                  {"evidence", evidence},
                  {"status", "client_assertion"},
                  {"provenance", Provenance(caller, now)}};
    entry = AppendEntryLocked(std::move(entry));
    Publish("context.observation_added", {{"entry", entry["id"]},
                                          {"kind", kind},
                                          {"address", entry["address"]},
                                          {"source", CallerName(caller.caller)}});

    Operation operation;
    operation.target = target;
    operation.data = {{"entry", entry}};
    operation.limits = {{"max_entries", limits_.maxEntries}, {"max_text", limits_.maxText},
                        {"max_data_bytes", limits_.maxDataBytes}, {"max_evidence", limits_.maxEvidence}};
    operation.evidence = json::array({entry["id"]});
    operation.permissionsRequired = json::array({"state"});
    return OperationResult(operation);
}

void InvestigationContext::RecordOperation(const CallerInfo& caller, const std::string& tool, const std::string& risk,
                                           const std::string& status, uint64_t durationMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    json row = {{"sequence", nextOperation_++},
                {"tool", tool.substr(0, limits_.maxLabel)},
                {"risk", risk},
                {"status", status},
                {"duration_ms", durationMs},
                {"provenance", Provenance(caller, Now())}};
    history_.push_back(std::move(row));
    while (history_.size() > limits_.maxHistory) {
        history_.pop_front();
        ++droppedHistory_;
    }
}

json InvestigationContext::Snapshot(size_t limit, uint64_t sinceEntry) const {
    std::lock_guard<std::mutex> lock(mutex_);
    limit = std::clamp<size_t>(limit, 1, limits_.maxEntries);

    json modules = json::array();
    for (const auto& module : modules_)
        modules.push_back({{"name", module.name}, {"base", Hex(module.base)}, {"size", module.size}});

    // Newest selection per source.
    json selections = json::object();
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        if ((*it)["kind"] != "selection") continue;
        const std::string source = (*it)["provenance"].value("source", std::string());
        if (!selections.contains(source)) selections[source] = *it;
    }

    std::vector<const json*> newer;
    for (const auto& entry : entries_)
        if (entry.value("sequence", uint64_t{0}) > sinceEntry) newer.push_back(&entry);
    const bool truncated = newer.size() > limit;
    json entries = json::array();
    for (size_t i = truncated ? newer.size() - limit : 0; i < newer.size(); ++i) entries.push_back(*newer[i]);

    json history = json::array();
    for (const auto& row : history_) history.push_back(row);

    return {{"schema", "cortex.context.v1"},
            {"target", {{"pid", identity_.pid},
                        {"generation", identity_.generation},
                        {"architecture", identity_.architecture},
                        {"process_name", identity_.processName},
                        {"executable_path", identity_.executablePath}}},
            {"modules", std::move(modules)},
            {"selections", std::move(selections)},
            {"entries", std::move(entries)},
            {"entries_truncated", truncated},
            {"latest_entry", nextEntry_ - 1},
            {"dropped_entries", droppedEntries_},
            {"history", std::move(history)},
            {"dropped_history", droppedHistory_},
            {"limits", {{"max_entries", limits_.maxEntries}, {"max_history", limits_.maxHistory}}},
            {"notice", "Selections and observations are caller assertions with provenance, not verified facts."}};
}

void InvestigationContext::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    history_.clear();
    modules_.clear();
    modulesKnown_ = false;
    nextEntry_ = 1;
    nextOperation_ = 1;
    droppedEntries_ = 0;
    droppedHistory_ = 0;
}

InvestigationContext& Global() {
    static InvestigationContext instance;
    return instance;
}

} // namespace context
