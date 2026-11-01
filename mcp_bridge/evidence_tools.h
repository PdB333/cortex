#pragma once

// Evidence tools for agents (roadmap step 5), over core/evidence:
//
//   cortex_evidence_record   create a record (Cortex state)
//   cortex_evidence_update   change class/statement/references/relations
//                            at an expected revision (Cortex state)
//   cortex_evidence_verify   recompute a record's static check now; marks it
//                            stale if it no longer holds (Cortex state)
//   cortex_evidence_get / _list / _export / _compare   read-only
//
// The writers need mutation_permission and the operator's --allow-state,
// like the knowledge and journal tools. Classes are checked by the store:
// verified_observation is accepted only when Cortex reproduces the static
// result or finds the referenced trial completed.

#include "artifact_tools.h"

#include "../core/evidence/evidence.h"
#include "../core/services/user_data.h"

namespace mcp_bridge::evidence {

using json = nlohmann::json;
namespace fs = std::filesystem;
namespace ev = cortex::evidence;

using TrialLookup = std::function<json(const std::string&)>;

inline json ToolDefinitions() {
    const json subject = {
        {"type", "object"},
        {"properties", {{"path", {{"type", "string"}, {"maxLength", 4096}}},
                        {"child", {{"type", "string"}, {"maxLength", 4096}}},
                        {"sha256", {{"type", "string"}, {"minLength", 64}, {"maxLength", 64}}},
                        {"name", {{"type", "string"}, {"maxLength", 260}}},
                        {"location", {{"type", "string"}, {"maxLength", 512}}}}},
        {"additionalProperties", false},
        {"description", "The artifact (path inside --artifact-root, identified by Cortex; or a sha256 the caller "
                        "asserts) and an optional location: rva:0x.., token:0x.., js:file:line:col, module:name+0x.."}};
    const json references = {{"type", "array"}, {"maxItems", 16},
                             {"items", {{"type", "object"},
                                        {"properties", {{"kind", {{"type", "string"}, {"enum", json::array({"trial", "report", "artifact", "knowledge", "evidence", "scan", "breakpoint", "trace", "context"})}}},
                                                        {"id", {{"type", "string"}, {"maxLength", 128}}},
                                                        {"detail", {{"type", "string"}, {"maxLength", 1024}}}}},
                                        {"required", json::array({"kind", "id"})},
                                        {"additionalProperties", false}}}};
    const json relations = {{"type", "array"}, {"maxItems", 16},
                            {"items", {{"type", "object"},
                                       {"properties", {{"type", {{"type", "string"}, {"enum", json::array({"calls", "reads", "writes", "handled_by", "exposes", "loads", "compiled_to", "same_as", "correlates", "contradicts", "depends_on", "related_to"})}}},
                                                       {"to", {{"type", "object"}, {"description", "{evidence: id} or {sha256, location?}"}}},
                                                       {"origin", {{"type", "string"}, {"enum", json::array({"static", "decompiled", "observed"})}}},
                                                       {"note", {{"type", "string"}, {"maxLength", 512}}}}},
                                       {"required", json::array({"type", "to"})},
                                       {"additionalProperties", false}}}};
    const json classes = {{"type", "string"},
                          {"enum", json::array({"verified_observation", "correlation", "hypothesis", "contradictory", "inconclusive", "refuted"})}};
    const json permission = {{"type", "boolean"}};
    const json writer = {{"read_only", false}, {"host_control", true}, {"effect", "cortex_state"}};
    const json reader = {{"read_only", true}};
    return json::array({
        {{"name", "cortex_evidence_record"},
         {"description", "Record evidence about an artifact location (one record type for trials, scans, breakpoints and "
                         "static results; kept without the target process). Cortex enforces the class: "
                         "verified_observation needs produced_by {tool, arguments, expect:[{pointer, value}]} that Cortex "
                         "reruns and reproduces (origin static), or a completed, decided trial in references (origin "
                         "observed). An unmet class is refused with the reason, never changed silently. Needs "
                         "mutation_permission and --allow-state."},
         {"inputSchema", {{"type", "object"},
                          {"properties", {{"statement", {{"type", "string"}, {"minLength", 1}, {"maxLength", 2048}}},
                                          {"class", classes},
                                          {"origin", {{"type", "string"}, {"enum", json::array({"static", "decompiled", "observed"})}}},
                                          {"subject", subject},
                                          {"produced_by", {{"type", "object"}}},
                                          {"references", references},
                                          {"relations", relations},
                                          {"notes", {{"type", "string"}, {"maxLength", 4096}}},
                                          {"mutation_permission", permission}}},
                          {"required", json::array({"statement", "class", "origin", "subject", "mutation_permission"})},
                          {"additionalProperties", false}}},
         {"_cortex", writer}},
        {{"name", "cortex_evidence_update"},
         {"description", "Change an evidence record's class, statement, references or relations at expected_revision. "
                         "The class rules apply again. Needs mutation_permission and --allow-state."},
         {"inputSchema", {{"type", "object"},
                          {"properties", {{"id", {{"type", "string"}}},
                                          {"expected_revision", {{"type", "integer"}, {"minimum", 1}}},
                                          {"class", classes},
                                          {"statement", {{"type", "string"}, {"maxLength", 2048}}},
                                          {"references", references},
                                          {"relations", relations},
                                          {"notes", {{"type", "string"}, {"maxLength", 4096}}},
                                          {"mutation_permission", permission}}},
                          {"required", json::array({"id", "expected_revision", "mutation_permission"})},
                          {"additionalProperties", false}}},
         {"_cortex", writer}},
        {{"name", "cortex_evidence_verify"},
         {"description", "Recompute a record's check now (rerun its static tool, re-read its trials). The class never "
                         "changes; a check that no longer holds marks the record stale. Needs mutation_permission and "
                         "--allow-state (it writes the verification)."},
         {"inputSchema", {{"type", "object"},
                          {"properties", {{"id", {{"type", "string"}}}, {"mutation_permission", permission}}},
                          {"required", json::array({"id", "mutation_permission"})},
                          {"additionalProperties", false}}},
         {"_cortex", writer}},
        {{"name", "cortex_evidence_get"},
         {"description", "One evidence record with its history. Read-only."},
         {"inputSchema", {{"type", "object"}, {"properties", {{"id", {{"type", "string"}}}}},
                          {"required", json::array({"id"})}, {"additionalProperties", false}}},
         {"_cortex", reader}},
        {{"name", "cortex_evidence_list"},
         {"description", "Evidence records, newest first, filtered by sha256, location, class or origin. Read-only."},
         {"inputSchema", {{"type", "object"},
                          {"properties", {{"sha256", {{"type", "string"}}}, {"location", {{"type", "string"}}},
                                          {"class", classes},
                                          {"origin", {{"type", "string"}}},
                                          {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 1000}}}}},
                          {"additionalProperties", false}}},
         {"_cortex", reader}},
        {{"name", "cortex_evidence_export"},
         {"description", "Export evidence (all, or one artifact's by sha256) as JSON (cortex.evidence.export.v1) or a "
                         "Markdown report. Read-only."},
         {"inputSchema", {{"type", "object"},
                          {"properties", {{"format", {{"type", "string"}, {"enum", json::array({"json", "markdown"})}}},
                                          {"sha256", {{"type", "string"}}}}},
                          {"required", json::array({"format"})},
                          {"additionalProperties", false}}},
         {"_cortex", reader}},
        {{"name", "cortex_evidence_compare"},
         {"description", "Where the records about an older build (sha256) stand in another build of the artifact "
                         "(path inside --artifact-root): same_binary, unchanged_at_rva, moved (with the new RVA), "
                         "ambiguous, not_found or not_comparable, by position-independent code fingerprints. Matching "
                         "bytes do not prove unchanged behaviour. Records are not modified. Read-only."},
         {"inputSchema", {{"type", "object"},
                          {"properties", {{"sha256", {{"type", "string"}, {"minLength", 64}, {"maxLength", 64}}},
                                          {"path", {{"type", "string"}, {"maxLength", 4096}}},
                                          {"child", {{"type", "string"}, {"maxLength", 4096}}}}},
                          {"required", json::array({"sha256", "path"})},
                          {"additionalProperties", false}}},
         {"_cortex", reader}},
    });
}

inline bool IsTool(const std::string& name) {
    return name == "cortex_evidence_record" || name == "cortex_evidence_update" || name == "cortex_evidence_verify" ||
           name == "cortex_evidence_get" || name == "cortex_evidence_list" || name == "cortex_evidence_export" ||
           name == "cortex_evidence_compare";
}

inline bool IsWriter(const std::string& name) {
    return name == "cortex_evidence_record" || name == "cortex_evidence_update" || name == "cortex_evidence_verify";
}

// The checks the store uses, bound to this server's roots and trials.
inline ev::Checks MakeChecks(const std::vector<fs::path>& roots, TrialLookup trials) {
    ev::Checks checks;
    checks.trial = std::move(trials);
    if (roots.empty()) return checks;
    checks.runTool = [roots](const std::string& tool, const json& arguments) {
        return artifacts::Handle(tool, arguments, roots, nullptr);
    };
    checks.identify = [roots](const std::string& path, const std::string& child, const std::string& location) -> json {
        fs::path resolved;
        std::string error;
        if (!artifacts::ResolveInsideRoots(path, roots, resolved, error)) return {{"ok", false}, {"error", error}};
        auto source = artifacts::OpenChain(resolved.u8string(), child, error);
        if (!source) return {{"ok", false}, {"error", error}};
        const auto& registry = artifacts::core::DefaultRegistry();
        const auto provider = registry.Detect(*source);
        json identity = {{"ok", true}, {"sha256", artifacts::core::Sha256Of(*source)}, {"name", source->Name()},
                         {"format", provider ? provider->Name() : "unknown"}};
        const bool find = location.rfind("find:", 0) == 0;
        if (!find && location.rfind("rva:0x", 0) != 0) return identity;
        artifacts::NativeView view;
        if (!artifacts::OpenNative(*source, nullptr, view, error)) return identity;  // not native code: no fingerprint
        if (find) {
            const json fingerprint = json::parse(location.substr(5), nullptr, false);
            json rvas = json::array();
            if (fingerprint.is_object())
                for (uint32_t rva : artifacts::core::native::FindFingerprint(*source, view.pe, *view.index, fingerprint))
                    rvas.push_back(artifacts::core::HexU64(rva));
            identity["rvas"] = rvas;
            return identity;
        }
        uint32_t rva = 0;
        if (artifacts::core::native::ResolveLocation(*view.index, location.substr(4), {}, rva)) {
            const json fingerprint = artifacts::core::native::Fingerprint(*source, view.pe, *view.index, rva);
            if (!fingerprint.is_null()) identity["fingerprint"] = fingerprint;
        }
        return identity;
    };
    return checks;
}

// Runs one evidence tool. Writers check mutation_permission here; the host
// checks --allow-state before calling (IsCortexStateTool).
inline json Handle(const std::string& name, const json& arguments, ev::Store* store, const std::vector<fs::path>& roots,
                   const TrialLookup& trials, const std::string& recordedBy = "agent") {
    if (!store) return artifacts::Failure("evidence_store_unavailable");
    if (!arguments.is_object()) return artifacts::Failure("invalid_arguments");
    json input = arguments;
    if (IsWriter(name)) {
        const auto permission = input.find("mutation_permission");
        if (permission == input.end() || !permission->is_boolean() || !permission->get<bool>())
            return artifacts::Failure("mutation_permission_required", "Evidence is Cortex state; pass mutation_permission=true.");
        input.erase("mutation_permission");
    }
    const ev::Checks checks = MakeChecks(roots, trials);
    if (name == "cortex_evidence_record") return store->Record(input, recordedBy, checks);
    if (name == "cortex_evidence_update") return store->Update(input, recordedBy, checks);
    if (name == "cortex_evidence_verify") return store->Reverify(input.value("id", std::string()), checks);
    if (name == "cortex_evidence_get") return store->Get(input.value("id", std::string()));
    if (name == "cortex_evidence_list") {
        json filter = input;
        filter.erase("limit");
        return store->List(filter, static_cast<size_t>(std::clamp<int64_t>(input.value("limit", int64_t{100}), 1, 1000)));
    }
    if (name == "cortex_evidence_export") {
        json filter = json::object();
        if (input.contains("sha256")) filter["sha256"] = input["sha256"];
        json result = store->Export(filter, input.value("format", std::string()));
        // Large exports are cut for the transport; the CLI writes the whole file.
        if (result.value("ok", false) && result["document"].dump().size() > (512u << 10)) {
            result["document"] = nullptr;
            result["error_detail"] = "export larger than 512 KiB; use cortex.exe evidence export";
            result["ok"] = false;
            result["error"] = "export_too_large";
        }
        return result;
    }
    if (name == "cortex_evidence_compare")
        return store->Compare(input.value("sha256", std::string()), input.value("path", std::string()),
                              input.value("child", std::string()), checks);
    return artifacts::Failure("unknown_tool");
}

// `cortex.exe evidence export|list [--store DIR] [--format json|markdown]
// [--sha256 X] [--out FILE]`: reads the person's own store.
inline int EvidenceMain(int argc, char** argv, const fs::path& defaultStore) {
    std::string command = argc > 1 && argv[1] ? argv[1] : "", format = "markdown", sha, out;
    fs::path directory = defaultStore;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i] ? argv[i] : "";
        if (arg == "--store" && i + 1 < argc) directory = fs::u8path(argv[++i]);
        else if (arg == "--format" && i + 1 < argc) format = argv[++i];
        else if (arg == "--sha256" && i + 1 < argc) sha = argv[++i];
        else if (arg == "--out" && i + 1 < argc) out = argv[++i];
        else {
            std::cerr << "cortex evidence: unexpected argument " << arg << "\n";
            return 2;
        }
    }
    if (command != "export" && command != "list") {
        std::cout << "Usage: cortex.exe evidence export|list [--store DIR] [--format json|markdown] [--sha256 X] [--out FILE]\n"
                     "Reads the evidence store (default: the user data folder). Changes nothing.\n";
        return command.empty() || command == "--help" ? 0 : 2;
    }
    ev::Store store(directory);
    json filter = json::object();
    if (!sha.empty()) filter["sha256"] = sha;
    const json result = command == "list" ? store.List(filter, 100000) : store.Export(filter, format);
    if (!result.value("ok", false)) {
        std::cerr << "cortex evidence: " << result.value("error", std::string("failed")) << "\n";
        return 1;
    }
    const std::string text = command == "list" ? result.dump(2)
                             : result["document"].is_string() ? result["document"].get<std::string>()
                                                              : result["document"].dump(2);
    if (out.empty()) {
        std::cout << text << "\n";
        return 0;
    }
    std::string error;
    if (!cortex::services::WriteFileAtomically(fs::u8path(out), text, &error)) {
        std::cerr << "cortex evidence: " << error << "\n";
        return 1;
    }
    return 0;
}

} // namespace mcp_bridge::evidence
