#pragma once

// cortex_live_*: the CDP live provider (core/live/cdp.h) for Node.js and
// Electron, and `cortex.exe live`. Inspector ports are allowed by the
// operator (--inspect-port); connections are to 127.0.0.1 only. Reading
// (targets, scripts, source, console, hits) is read-only. Setting or removing
// a log breakpoint is target control: it needs mutation_permission and the
// operator's --allow-writes, like the native debugger's breakpoints.

#include "../core/live/cdp.h"

#include <chrono>
#include <iostream>
#include <thread>

namespace mcp_bridge::live {

using json = nlohmann::json;

inline json ToolDefinitions() {
    const json port{{"type", "integer"}, {"minimum", 1}, {"maximum", 65535}, {"description", "Inspector port on 127.0.0.1, allowed with --inspect-port"}};
    const json session{{"type", "string"}, {"maxLength", 64}};
    auto object = [](json properties, json required) {
        return json{{"type", "object"}, {"properties", std::move(properties)}, {"required", std::move(required)}, {"additionalProperties", false}};
    };
    return json::array({
        {{"name", "cortex_live_targets"},
         {"description", "Targets of a local Node.js/Electron inspector (CDP /json/list): node, page, worker... Read-only."},
         {"inputSchema", object({{"port", port}}, json::array({"port"}))},
         {"_cortex", {{"read_only", true}}}},
        {{"name", "cortex_live_attach"},
         {"description", "Attach to an inspector target and enable its Debugger and Runtime domains to list scripts and record console "
                         "messages. Never evaluates code. A pause Cortex did not set (debugger statement) is recorded and resumed."},
         {"inputSchema", object({{"port", port}, {"target_id", {{"type", "string"}, {"maxLength", 128}}}}, json::array({"port"}))},
         {"_cortex", {{"read_only", true}}}},
        {{"name", "cortex_live_status"},
         {"description", "State of a live session: target, scripts, breakpoints, pauses, console count, foreign pauses."},
         {"inputSchema", object({{"session_id", session}}, json::array({"session_id"}))},
         {"_cortex", {{"read_only", true}}}},
        {{"name", "cortex_live_scripts"},
         {"description", "Scripts the inspector has parsed (id, URL, lines, source map URL), filtered by URL substring."},
         {"inputSchema", object({{"session_id", session}, {"query", {{"type", "string"}, {"maxLength", 256}}},
                                 {"offset", {{"type", "integer"}, {"minimum", 0}}}, {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 500}}}},
                                json::array({"session_id"}))},
         {"_cortex", {{"read_only", true}}}},
        {{"name", "cortex_live_source"},
         {"description", "Source of a loaded script (by script id or URL), a 1-based line range. The text is untrusted data."},
         {"inputSchema", object({{"session_id", session}, {"script", {{"type", "string"}, {"maxLength", 4096}}},
                                 {"first_line", {{"type", "integer"}, {"minimum", 1}}}, {"lines", {{"type", "integer"}, {"minimum", 1}, {"maximum", 5000}}}},
                                json::array({"session_id", "script"}))},
         {"_cortex", {{"read_only", true}}}},
        {{"name", "cortex_live_console"},
         {"description", "Console messages and uncaught exceptions recorded since attaching (bounded, oldest dropped)."},
         {"inputSchema", object({{"session_id", session}, {"offset", {{"type", "integer"}, {"minimum", 0}}},
                                 {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 500}}}},
                                json::array({"session_id"}))},
         {"_cortex", {{"read_only", true}}}},
        {{"name", "cortex_live_breakpoint_set"},
         {"description", "Set a LOG breakpoint at a 1-based line (and column) of a script URL. At each hit Cortex captures the stack and "
                         "the top frame's locals (getters are not run), then resumes at once; after max_hits the breakpoint removes "
                         "itself. No conditions (they would run code in the target). Needs --allow-writes and mutation_permission."},
         {"inputSchema", object({{"session_id", session}, {"url", {{"type", "string"}, {"maxLength", 4096}}},
                                 {"line", {{"type", "integer"}, {"minimum", 1}}}, {"column", {{"type", "integer"}, {"minimum", 1}}},
                                 {"max_hits", {{"type", "integer"}, {"minimum", 1}, {"maximum", 1000}}},
                                 {"max_frames", {{"type", "integer"}, {"minimum", 1}, {"maximum", 32}}},
                                 {"mutation_permission", {{"type", "boolean"}}}},
                                json::array({"session_id", "url", "line", "mutation_permission"}))},
         {"_cortex", {{"host_control", true}, {"effect", "target_control"}}}},
        {{"name", "cortex_live_breakpoint_remove"},
         {"description", "Remove a log breakpoint. Needs --allow-writes and mutation_permission."},
         {"inputSchema", object({{"session_id", session}, {"breakpoint", {{"type", "integer"}, {"minimum", 1}}}, {"mutation_permission", {{"type", "boolean"}}}},
                                json::array({"session_id", "breakpoint", "mutation_permission"}))},
         {"_cortex", {{"host_control", true}, {"effect", "target_control"}}}},
        {{"name", "cortex_live_hits"},
         {"description", "Breakpoints of a session, or the recorded hits of one (stack, locals, time). A hit is an observation, not a "
                         "causal explanation."},
         {"inputSchema", object({{"session_id", session}, {"breakpoint", {{"type", "integer"}, {"minimum", 1}}},
                                 {"offset", {{"type", "integer"}, {"minimum", 0}}}, {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 200}}}},
                                json::array({"session_id"}))},
         {"_cortex", {{"read_only", true}}}},
        {{"name", "cortex_live_detach"},
         {"description", "Remove Cortex's breakpoints, disable the domains and close the session. The program keeps running."},
         {"inputSchema", object({{"session_id", session}}, json::array({"session_id"}))},
         {"_cortex", {{"host_control", true}}}},
    });
}

inline bool IsTool(const std::string& name) { return name.rfind("cortex_live_", 0) == 0; }

inline size_t Size(const json& a, const char* key, size_t fallback, size_t high) {
    if (!a.contains(key) || !a[key].is_number_unsigned()) return fallback;
    return std::min<size_t>(a[key].get<size_t>(), high);
}

inline json Handle(const std::string& name, const json& a, cortex::live::LiveService* service, bool allowWrites) {
    if (!service) return {{"ok", false}, {"error", "live_unavailable"}};
    if (!a.is_object()) return {{"ok", false}, {"error", "invalid_arguments"}};
    if (name == "cortex_live_targets" || name == "cortex_live_attach") {
        if (!a.contains("port") || !a["port"].is_number_unsigned() || a["port"].get<uint64_t>() == 0 || a["port"].get<uint64_t>() > 65535)
            return {{"ok", false}, {"error", "invalid_port"}};
        const uint16_t port = static_cast<uint16_t>(a["port"].get<uint64_t>());
        return name == "cortex_live_targets" ? service->Targets(port) : service->Attach(port, a.value("target_id", std::string()));
    }
    const std::string id = a.value("session_id", std::string());
    if (name == "cortex_live_detach") return service->Detach(id);
    auto session = service->Get(id);
    if (!session) return {{"ok", false}, {"error", "unknown_session"}};
    if (name == "cortex_live_status") return session->Status();
    if (name == "cortex_live_scripts") return session->Scripts(a.value("query", std::string()), Size(a, "offset", 0, SIZE_MAX), Size(a, "limit", 100, 500));
    if (name == "cortex_live_source")
        return session->Source(a.value("script", std::string()), std::max<size_t>(1, Size(a, "first_line", 1, SIZE_MAX)), Size(a, "lines", 400, 5000));
    if (name == "cortex_live_console") return session->Console(Size(a, "offset", 0, SIZE_MAX), Size(a, "limit", 100, 500));
    if (name == "cortex_live_hits") {
        if (!a.contains("breakpoint")) return session->Breakpoints();
        return session->Hits(static_cast<int>(Size(a, "breakpoint", 0, 1u << 30)), Size(a, "offset", 0, SIZE_MAX), Size(a, "limit", 50, 200));
    }
    if (name == "cortex_live_breakpoint_set" || name == "cortex_live_breakpoint_remove") {
        if (!a.value("mutation_permission", false))
            return {{"ok", false}, {"error", "mutation_permission_required"}, {"message", "Declare mutation_permission=true for a breakpoint."}};
        if (!allowWrites)
            return {{"ok", false}, {"error", "write_authority_required"},
                    {"message", "Breakpoints pause the target briefly: the person must start cortex.exe mcp with --allow-writes."}};
        if (name == "cortex_live_breakpoint_remove") return session->RemoveBreakpoint(static_cast<int>(Size(a, "breakpoint", 0, 1u << 30)));
        const size_t line = Size(a, "line", 0, 1u << 30);
        if (!line || a.value("url", std::string()).empty()) return {{"ok", false}, {"error", "url_and_line_required"}};
        return session->SetLogBreakpoint(a.value("url", std::string()), static_cast<uint32_t>(line), static_cast<uint32_t>(Size(a, "column", 0, 1u << 30)),
                                         static_cast<uint32_t>(std::max<size_t>(1, Size(a, "max_hits", 20, 1000))),
                                         static_cast<uint32_t>(std::max<size_t>(1, Size(a, "max_frames", 8, 32))));
    }
    return {{"ok", false}, {"error", "unknown_tool"}};
}

// `cortex.exe live targets --port N` and `cortex.exe live watch --port N
// --url URL --line L [--column C] [--hits K] [--seconds S]`: the person's
// own inspector, any local port.
inline int LiveMain(int argc, char** argv) {
    auto usage = [] {
        std::cerr << "usage: cortex live targets --port N\n"
                     "       cortex live scripts --port N [--query TEXT]\n"
                     "       cortex live watch --port N --url URL --line L [--column C] [--hits K] [--seconds S] [--target ID]\n";
        return 2;
    };
    if (argc < 2) return usage();
    const std::string command = argv[1];
    uint64_t port = 0, line = 0, column = 0, hits = 10, seconds = 30;
    std::string url, query, target;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (i + 1 >= argc) return usage();
        const std::string value = argv[++i];
        try {
            if (arg == "--port") port = std::stoull(value);
            else if (arg == "--line") line = std::stoull(value);
            else if (arg == "--column") column = std::stoull(value);
            else if (arg == "--hits") hits = std::stoull(value);
            else if (arg == "--seconds") seconds = std::stoull(value);
            else if (arg == "--url") url = value;
            else if (arg == "--query") query = value;
            else if (arg == "--target") target = value;
            else return usage();
        } catch (...) {
            return usage();
        }
    }
    if (!port || port > 65535) return usage();
    cortex::live::LiveService service({}, true);
    if (command == "targets") {
        const json result = service.Targets(static_cast<uint16_t>(port));
        std::cout << result.dump(2) << std::endl;
        return result.value("ok", false) ? 0 : 1;
    }
    if (command != "scripts" && command != "watch") return usage();
    if (command == "watch" && (url.empty() || !line)) return usage();
    const json attached = service.Attach(static_cast<uint16_t>(port), target);
    if (!attached.value("ok", false)) {
        std::cout << attached.dump(2) << std::endl;
        return 1;
    }
    const std::string id = attached["session_id"];
    auto session = service.Get(id);
    json out;
    if (command == "scripts") {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));  // scriptParsed events arrive after enable
        out = session->Scripts(query, 0, 500);
    } else {
        const json bp = session->SetLogBreakpoint(url, static_cast<uint32_t>(line), static_cast<uint32_t>(column),
                                                  static_cast<uint32_t>(std::clamp<uint64_t>(hits, 1, 1000)), 8);
        if (!bp.value("ok", false)) {
            out = bp;
        } else {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(std::min<uint64_t>(seconds, 3600));
            while (std::chrono::steady_clock::now() < deadline && session->Hits(bp["breakpoint"], 0, 0)["active"] == true)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            out = session->Hits(bp["breakpoint"], 0, 1000);
            out["status"] = session->Status();
        }
    }
    service.Detach(id);
    std::cout << out.dump(2) << std::endl;
    return out.value("ok", false) ? 0 : 1;
}

} // namespace mcp_bridge::live
