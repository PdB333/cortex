#pragma once

// cortex_process_tree: a process and its descendants with their
// Chromium/Electron roles (core/target/process_tree.h). Read-only: it lists
// processes and reads their command lines; it attaches to nothing.

#include "../core/target/process_tree.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>

namespace mcp_bridge::process_tree {

using json = nlohmann::json;

inline json ToolDefinitions() {
    return json::array({{{"name", "cortex_process_tree"},
                         {"description", "A process and its descendants with parent links, command lines and, for "
                                         "Chromium/Electron apps, each process's role (main, renderer, gpu-process, "
                                         "utility:<service>, crashpad-handler). Use it to choose which Electron process to "
                                         "attach to. Read-only; nothing is attached or injected."},
                         {"inputSchema", {{"type", "object"},
                                          {"properties", {{"pid", {{"type", "integer"}, {"minimum", 1}}},
                                                          {"max_processes", {{"type", "integer"}, {"minimum", 1}, {"maximum", 2048}}}}},
                                          {"required", json::array({"pid"})},
                                          {"additionalProperties", false}}},
                         {"_cortex", {{"read_only", true}}}}});
}

inline bool IsTool(const std::string& name) { return name == "cortex_process_tree"; }

inline json Handle(const json& arguments) {
    if (!arguments.is_object() || !arguments.value("pid", json()).is_number_integer() || arguments["pid"].get<int64_t>() <= 0)
        return {{"ok", false}, {"error", "invalid_pid"}};
    const size_t limit = static_cast<size_t>(std::clamp<int64_t>(arguments.value("max_processes", int64_t{512}), 1, 2048));
    return cortex::target::ProcessTreeJson(cortex::target::SnapshotProcesses(), arguments["pid"].get<uint64_t>(), limit);
}

} // namespace mcp_bridge::process_tree
