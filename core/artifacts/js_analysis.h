#pragma once

// Static facts about one JavaScript/TypeScript file, read from its tokens
// (js_lexer.h). Everything here is what the text says, not what runs: a
// `require` behind a condition is still listed, a computed specifier is
// only counted, and an IPC channel found in the source is a static name,
// never observed traffic.

#include "js_lexer.h"

#include <nlohmann/json.hpp>

#include <map>
#include <string>
#include <vector>

namespace cortex::artifacts::js {

struct Location {
    uint32_t line = 0;
    uint32_t column = 0;
};

struct Dependency {
    std::string specifier;
    // import | export_from | dynamic_import | require | import_equals
    std::string kind;
    bool typeOnly = false;     // TypeScript `import type` / `export type`
    Location at;
};

struct ExportName {
    std::string name;          // "default", "*", or the exported name
    std::string kind;          // esm | esm_reexport | cjs | ts_type
    Location at;
};

// One use of an Electron IPC or bridge API with a constant channel.
struct IpcUse {
    std::string object;        // ipcMain | ipcRenderer | webContents | contextBridge
    std::string method;        // handle, invoke, send, exposeInMainWorld, ...
    std::string channel;       // the channel (or the exposed key)
    std::string role;          // main_handler | main_listener | renderer_invoke | renderer_send |
                               // renderer_listener | main_to_renderer | bridge_exposure
    Location at;
};

// `new BrowserWindow({...})` (or BrowserView / WebContentsView).
struct WindowConfig {
    std::string constructorName;
    Location at;
    bool hasWebPreferences = false;
    nlohmann::json webPreferences = nlohmann::json::object();  // key -> literal or {"expression": text}
    // The preload expression: its string literals in order, and whether it
    // starts from __dirname (path.join(__dirname, 'preload.js')).
    std::string preloadExpression;
    std::vector<std::string> preloadLiterals;
    bool preloadFromDirname = false;
};

struct Analysis {
    std::string language;      // javascript | typescript
    std::string moduleKind;    // esm | cjs | mixed | none
    std::vector<Dependency> dependencies;
    size_t computedDynamicImports = 0;  // import(expr) without a constant
    size_t computedRequires = 0;
    std::vector<ExportName> exports;
    std::vector<IpcUse> ipc;
    size_t computedIpcChannels = 0;
    std::vector<WindowConfig> windows;
    std::map<std::string, size_t> bundlerSignatures;  // identifier -> occurrences
    std::vector<std::string> bundlers;                // webpack, esbuild, ... (heuristic)
    bool minified = false;
    std::string sourceMappingUrl;
    LexResult lex;             // tokens and statistics
    size_t bytes = 0;
    bool truncated = false;    // entries dropped at the limit
};

// `name` decides the language (.ts/.tsx/.mts/.cts are TypeScript).
Analysis Analyze(std::string_view source, const std::string& name, size_t maxEntries = 4096);

nlohmann::json ToJson(const Analysis& analysis, size_t maxEntries);

// True for names this analysis reads: .js .mjs .cjs .jsx .ts .tsx .mts .cts
// (a .d.ts declaration file too).
bool IsScriptName(const std::string& name);

// Node.js built-in module names (without "node:").
bool IsNodeBuiltin(const std::string& specifier);

} // namespace cortex::artifacts::js
