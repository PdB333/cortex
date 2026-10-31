#include "js_analysis.h"

#include <algorithm>
#include <set>

namespace cortex::artifacts::js {
namespace {

using json = nlohmann::json;

// Electron exports this analysis follows through aliases.
const std::set<std::string>& ElectronNames() {
    static const std::set<std::string> names = {"ipcMain",     "ipcRenderer",     "contextBridge", "BrowserWindow",
                                                "BrowserView", "WebContentsView", "webContents"};
    return names;
}

struct IpcMethod {
    const char* object;
    const char* method;
    const char* role;
};

const IpcMethod kIpcMethods[] = {
    {"ipcMain", "handle", "main_handler"},          {"ipcMain", "handleOnce", "main_handler"},
    {"ipcMain", "on", "main_listener"},             {"ipcMain", "once", "main_listener"},
    {"ipcMain", "addListener", "main_listener"},    {"ipcRenderer", "invoke", "renderer_invoke"},
    {"ipcRenderer", "send", "renderer_send"},       {"ipcRenderer", "sendSync", "renderer_send"},
    {"ipcRenderer", "sendToHost", "renderer_send"}, {"ipcRenderer", "postMessage", "renderer_send"},
    {"ipcRenderer", "on", "renderer_listener"},     {"ipcRenderer", "once", "renderer_listener"},
    {"ipcRenderer", "addListener", "renderer_listener"},
    // webContents.on(...) is a page event, not IPC: only send is listed.
    {"webContents", "send", "main_to_renderer"},    {"webContents", "postMessage", "main_to_renderer"},
    {"sender", "send", "main_to_renderer"},
};

const char* const kBundlerSignatures[] = {
    "__webpack_require__", "__webpack_modules__", "__webpack_exports__", "webpackJsonp", "__commonJS",
    "__toESM",             "__toCommonJS",        "__esm",               "__defProp",    "parcelRequire",
    "__vitePreload",       "__vite__mapDeps",     "commonjsGlobal",      "getDefaultExportFromCjs",
    "__awaiter",           "__importDefault",     "__createBinding",
};

class Walker {
public:
    Walker(std::string_view source, Analysis& out, size_t maxEntries)
        : s_(source), t_(out.lex.tokens), out_(out), max_(maxEntries) {}

    void Run() {
        CollectElectronAliases();
        for (size_t i = 0; i < t_.size(); ++i) {
            const Token& token = t_[i];
            if (token.kind == TokenKind::String && Text(i).find("webpackChunk") != std::string_view::npos)
                ++out_.bundlerSignatures["webpackChunk"];
            if (token.kind != TokenKind::Identifier) continue;
            const std::string_view word = Text(i);
            for (const char* signature : kBundlerSignatures)
                if (word == signature) ++out_.bundlerSignatures[signature];
            if (word.substr(0, 12) == "webpackChunk") ++out_.bundlerSignatures["webpackChunk"];
            if (Member(i)) {
                Ipc(i);
                continue;
            }
            if (word == "import") Import(i);
            else if (word == "export") Export(i);
            else if (word == "require") Require(i);
            else if (word == "module" || word == "exports") CommonJsExport(i);
            else if (word == "new") New(i);
            else if (word == "Object") DefineProperty(i);
        }
        Bundlers();
        if (esm_ && cjs_) out_.moduleKind = "mixed";
        else if (esm_) out_.moduleKind = "esm";
        else if (cjs_) out_.moduleKind = "cjs";
        else out_.moduleKind = "none";
    }

private:
    std::string_view Text(size_t i) const { return i < t_.size() ? TokenText(s_, t_[i]) : std::string_view(); }
    bool Is(size_t i, std::string_view text) const {
        return i < t_.size() && t_[i].kind != TokenKind::String && t_[i].kind != TokenKind::Template && Text(i) == text;
    }
    bool Ident(size_t i) const { return i < t_.size() && t_[i].kind == TokenKind::Identifier; }
    // A constant string: '...', "..." or a template without ${}.
    bool Literal(size_t i) const {
        return i < t_.size() && (t_[i].kind == TokenKind::String ||
                                 (t_[i].kind == TokenKind::Template && !t_[i].substitutions));
    }
    bool Member(size_t i) const { return i > 0 && (Is(i - 1, ".") || Is(i - 1, "?.")); }
    Location At(size_t i) const { return {t_[i].line, t_[i].column}; }
    bool Room(size_t count) {
        if (count < max_) return true;
        out_.truncated = true;
        return false;
    }

    void AddDependency(size_t literal, const char* kind, bool typeOnly = false) {
        if (!Room(out_.dependencies.size())) return;
        out_.dependencies.push_back({t_[literal].value, kind, typeOnly, At(literal)});
    }
    void AddExport(const std::string& name, const char* kind, size_t at) {
        if (!Room(out_.exports.size())) return;
        out_.exports.push_back({name, kind, At(at)});
    }

    // Index of the token closing the bracket opened at `open`, or t_.size().
    size_t Close(size_t open) const {
        int depth = 0;
        for (size_t j = open; j < t_.size(); ++j) {
            if (t_[j].kind != TokenKind::Punctuator) continue;
            const std::string_view p = Text(j);
            if (p == "{" || p == "(" || p == "[") ++depth;
            else if (p == "}" || p == ")" || p == "]") {
                if (--depth == 0) return j;
            }
        }
        return t_.size();
    }

    std::string Canonical(std::string_view name) const {
        const auto alias = aliases_.find(std::string(name));
        return alias == aliases_.end() ? std::string(name) : alias->second;
    }

    // `const { ipcMain, BrowserWindow: W } = require('electron')` and
    // `import { ipcMain as m } from 'electron'`: m and W are followed.
    void CollectElectronAliases() {
        for (size_t i = 0; i + 3 < t_.size(); ++i) {
            if (!Literal(i) || t_[i].value != "electron") continue;
            if (Is(i - 1, "(") && i >= 2 && Is(i - 2, "require") && i >= 3 && Is(i - 3, "=") && i >= 4 && Is(i - 4, "}")) {
                size_t open = i - 4;
                int depth = 0;
                while (open > 0) {
                    if (Is(open, "}")) ++depth;
                    else if (Is(open, "{") && --depth == 0) break;
                    --open;
                }
                Bindings(open + 1, i - 4, ":");
            } else if (i >= 2 && Is(i - 1, "from")) {
                size_t close = i - 2;
                if (!Is(close, "}")) continue;
                size_t open = close;
                while (open > 0 && !Is(open, "{")) --open;
                Bindings(open + 1, close, "as");
            }
        }
    }
    void Bindings(size_t begin, size_t end, std::string_view separator) {
        for (size_t j = begin; j < end; ++j) {
            if (!Ident(j)) continue;
            const std::string name(Text(j));
            if (!ElectronNames().count(name)) continue;
            if (Is(j + 1, separator) && Ident(j + 2)) aliases_[std::string(Text(j + 2))] = name;
        }
    }

    void Import(size_t i) {
        if (Member(i) || Is(i + 1, ":")) return;
        const size_t n = i + 1;
        if (Is(n, "(")) {
            if (Literal(n + 1) && (Is(n + 2, ")") || Is(n + 2, ","))) AddDependency(n + 1, "dynamic_import");
            else ++out_.computedDynamicImports;
            return;
        }
        if (Is(n, ".")) return;  // import.meta
        esm_ = true;
        if (Literal(n)) {
            AddDependency(n, "import");
            return;
        }
        const bool typeOnly = Is(n, "type") && !Is(n + 1, "from") && !Is(n + 1, ",");
        for (size_t j = n; j < t_.size() && j < n + 4096; ++j) {
            if (Is(j, "from") && Literal(j + 1)) {
                AddDependency(j + 1, "import", typeOnly);
                return;
            }
            if (Is(j, ";") || Is(j, "import") || Is(j, "export")) return;
            if (Is(j, "=")) {
                // TypeScript: import fs = require('fs')
                esm_ = false;
                if (Is(j + 1, "require") && Is(j + 2, "(") && Literal(j + 3)) {
                    AddDependency(j + 3, "import_equals", typeOnly);
                    importEquals_.insert(j + 1);
                    cjs_ = true;
                }
                return;
            }
        }
    }

    void ExportList(size_t open, bool typeOnly) {
        const size_t close = Close(open);
        std::vector<std::pair<std::string, size_t>> names;
        for (size_t j = open + 1; j < close; ++j) {
            if (!Ident(j)) continue;
            bool memberType = typeOnly;
            if (Is(j, "type") && Ident(j + 1) && !Is(j + 1, "as")) {
                memberType = true;
                ++j;
            }
            std::string name(Text(j));
            size_t at = j;
            if (Is(j + 1, "as") && j + 2 < close) {
                name = Literal(j + 2) ? t_[j + 2].value : std::string(Text(j + 2));
                at = j + 2;
                j += 2;
            }
            names.emplace_back(memberType ? "type:" + name : name, at);
            while (j + 1 < close && !Is(j + 1, ",")) ++j;
        }
        const bool reexport = Is(close + 1, "from") && Literal(close + 2);
        if (reexport) AddDependency(close + 2, "export_from", typeOnly);
        for (const auto& [name, at] : names) {
            const bool isType = name.rfind("type:", 0) == 0;
            AddExport(isType ? name.substr(5) : name, isType ? "ts_type" : reexport ? "esm_reexport" : "esm", at);
        }
    }

    void Export(size_t i) {
        if (Member(i) || Is(i + 1, ":")) return;
        size_t n = i + 1;
        esm_ = true;
        if (Is(n, "*")) {
            std::string name = "*";
            size_t j = n + 1;
            if (Is(j, "as") && j + 1 < t_.size()) {
                name = Literal(j + 1) ? t_[j + 1].value : std::string(Text(j + 1));
                j += 2;
            }
            if (Is(j, "from") && Literal(j + 1)) AddDependency(j + 1, "export_from");
            AddExport(name, "esm_reexport", n);
            return;
        }
        if (Is(n, "{")) {
            ExportList(n, false);
            return;
        }
        if (Is(n, "type") && Is(n + 1, "{")) {
            ExportList(n + 1, true);
            return;
        }
        if (Is(n, "=")) {
            esm_ = false;
            cjs_ = true;
            AddExport("export=", "cjs", n);
            return;
        }
        if (Is(n, "default")) {
            AddExport("default", "esm", n);
            return;
        }
        if (Is(n, "import") || Is(n, "as")) return;
        bool typeDeclaration = false;
        while (Is(n, "declare") || Is(n, "abstract") || Is(n, "async") || (Is(n, "const") && Is(n + 1, "enum"))) ++n;
        if (Is(n, "type") || Is(n, "interface")) typeDeclaration = true;
        if (Is(n, "function") || Is(n, "class") || Is(n, "const") || Is(n, "let") || Is(n, "var") || Is(n, "enum") ||
            Is(n, "namespace") || Is(n, "module") || typeDeclaration) {
            size_t j = n + 1;
            if (Is(j, "*")) ++j;  // generator
            if (Ident(j)) AddExport(std::string(Text(j)), typeDeclaration ? "ts_type" : "esm", j);
        }
    }

    void Require(size_t i) {
        if (Member(i) || (i > 0 && Is(i - 1, "function")) || !Is(i + 1, "(")) return;
        if (importEquals_.count(i)) return;
        if (Literal(i + 2) && Is(i + 3, ")")) {
            AddDependency(i + 2, "require");
            cjs_ = true;
        } else if (!Is(i + 2, ")")) {
            ++out_.computedRequires;
        }
    }

    void CommonJsExport(size_t i) {
        if (Member(i)) return;
        size_t j = i;
        if (Is(i, "module")) {
            if (!Is(i + 1, ".") || !Is(i + 2, "exports")) return;
            j = i + 2;
            if (Is(j + 1, "=")) {
                cjs_ = true;
                AddExport("module.exports", "cjs", j);
                return;
            }
        }
        if (Is(j + 1, ".") && Ident(j + 2) && Is(j + 3, "=")) {
            cjs_ = true;
            AddExport(std::string(Text(j + 2)), "cjs", j + 2);
        } else if (Is(j + 1, "[") && Literal(j + 2) && Is(j + 3, "]") && Is(j + 4, "=")) {
            cjs_ = true;
            AddExport(t_[j + 2].value, "cjs", j + 2);
        }
    }

    // Object.defineProperty(exports, "name", ...)
    void DefineProperty(size_t i) {
        if (Member(i) || !Is(i + 1, ".") || !Is(i + 2, "defineProperty") || !Is(i + 3, "(") || !Is(i + 4, "exports") ||
            !Is(i + 5, ",") || !Literal(i + 6))
            return;
        cjs_ = true;
        if (t_[i + 6].value != "__esModule") AddExport(t_[i + 6].value, "cjs", i + 6);
    }

    void Ipc(size_t methodIndex) {
        // object . method ( "channel"
        if (methodIndex < 2 || !Ident(methodIndex - 2) || !Is(methodIndex + 1, "(")) return;
        const std::string object = Canonical(Text(methodIndex - 2));
        const std::string_view method = Text(methodIndex);
        if (object == "contextBridge" && (method == "exposeInMainWorld" || method == "exposeInIsolatedWorld")) {
            size_t key = methodIndex + 2;
            if (method == "exposeInIsolatedWorld") {
                // exposeInIsolatedWorld(worldId, key, api): skip one argument
                while (key < t_.size() && !Is(key, ",") && !Is(key, ")")) ++key;
                ++key;
            }
            if (Literal(key)) AddIpc(object, method, t_[key].value, "bridge_exposure", key);
            else ++out_.computedIpcChannels;
            return;
        }
        for (const auto& known : kIpcMethods) {
            if (object != known.object || method != known.method) continue;
            if (Literal(methodIndex + 2)) AddIpc(object, method, t_[methodIndex + 2].value, known.role, methodIndex + 2);
            else ++out_.computedIpcChannels;
            return;
        }
    }
    void AddIpc(const std::string& object, std::string_view method, const std::string& channel, const char* role,
                size_t at) {
        if (!Room(out_.ipc.size())) return;
        out_.ipc.push_back({object, std::string(method), channel, role, At(at)});
    }

    // Value tokens of each key of the object literal opened at `open`.
    std::vector<std::pair<std::string, std::pair<size_t, size_t>>> ObjectKeys(size_t open) const {
        std::vector<std::pair<std::string, std::pair<size_t, size_t>>> keys;
        const size_t close = Close(open);
        size_t j = open + 1;
        while (j < close) {
            if (Is(j, "...")) {  // spread: skip to the next property
                while (j < close && !Is(j, ",")) j = Is(j, "{") || Is(j, "(") || Is(j, "[") ? Close(j) + 1 : j + 1;
                ++j;
                continue;
            }
            std::string key = Literal(j) ? t_[j].value : std::string(Text(j));
            size_t valueBegin = j, valueEnd = j + 1;  // shorthand `key,`
            if (Is(j + 1, ":")) {
                valueBegin = j + 2;
                valueEnd = valueBegin;
                while (valueEnd < close && !Is(valueEnd, ","))
                    valueEnd = Is(valueEnd, "{") || Is(valueEnd, "(") || Is(valueEnd, "[") ? Close(valueEnd) + 1 : valueEnd + 1;
            } else if (Is(j + 1, "(")) {  // method shorthand
                valueEnd = j + 1;
                while (valueEnd < close && !Is(valueEnd, ","))
                    valueEnd = Is(valueEnd, "{") || Is(valueEnd, "(") || Is(valueEnd, "[") ? Close(valueEnd) + 1 : valueEnd + 1;
            }
            keys.push_back({key, {valueBegin, std::min(valueEnd, close)}});
            j = std::min(valueEnd, close) + 1;
        }
        return keys;
    }

    std::string Source(size_t begin, size_t end, size_t limit = 200) const {
        if (begin >= end || end > t_.size()) return {};
        std::string text(s_.substr(t_[begin].begin, t_[end - 1].end - t_[begin].begin));
        if (text.size() > limit) text = text.substr(0, limit) + "...";
        return text;
    }

    json Value(size_t begin, size_t end) const {
        if (end == begin + 1) {
            if (Is(begin, "true")) return true;
            if (Is(begin, "false")) return false;
            if (Literal(begin)) return t_[begin].value;
            if (t_[begin].kind == TokenKind::Number) return json{{"number", std::string(Text(begin))}};
        }
        // Minifiers write true as !0 and false as !1.
        if (end == begin + 2 && Is(begin, "!") && Is(begin + 1, "0")) return true;
        if (end == begin + 2 && Is(begin, "!") && Is(begin + 1, "1")) return false;
        return json{{"expression", Source(begin, end)}};
    }

    void New(size_t i) {
        size_t j = i + 1;
        while (Ident(j) && Is(j + 1, ".") && Ident(j + 2)) j += 2;
        if (!Ident(j)) return;
        const std::string name = Canonical(Text(j));
        if (name != "BrowserWindow" && name != "BrowserView" && name != "WebContentsView") return;
        if (!Is(j + 1, "(") || !Room(out_.windows.size())) return;
        WindowConfig window;
        window.constructorName = name;
        window.at = At(j);
        if (Is(j + 2, "{")) {
            for (const auto& [key, range] : ObjectKeys(j + 2)) {
                if (key != "webPreferences") continue;
                window.hasWebPreferences = true;
                if (!Is(range.first, "{")) {
                    window.webPreferences = json{{"expression", Source(range.first, range.second)}};
                    break;
                }
                for (const auto& [preference, value] : ObjectKeys(range.first)) {
                    window.webPreferences[preference] = Value(value.first, value.second);
                    if (preference != "preload") continue;
                    window.preloadExpression = Source(value.first, value.second, 300);
                    for (size_t k = value.first; k < value.second; ++k) {
                        if (Literal(k)) window.preloadLiterals.push_back(t_[k].value);
                        if (Is(k, "__dirname")) window.preloadFromDirname = true;
                    }
                }
            }
        }
        out_.windows.push_back(std::move(window));
    }

    void Bundlers() {
        const auto has = [&](const char* name) { return out_.bundlerSignatures.count(name) > 0; };
        if (has("__webpack_require__") || has("webpackChunk") || has("webpackJsonp") || has("__webpack_modules__"))
            out_.bundlers.push_back("webpack");
        if ((has("__commonJS") || has("__toESM") || has("__toCommonJS") || has("__esm")) && has("__defProp"))
            out_.bundlers.push_back("esbuild");
        if (has("parcelRequire")) out_.bundlers.push_back("parcel");
        if (has("commonjsGlobal") || has("getDefaultExportFromCjs")) out_.bundlers.push_back("rollup");
        if (has("__vitePreload") || has("__vite__mapDeps")) out_.bundlers.push_back("vite");
        if (has("__awaiter") || has("__importDefault") || has("__createBinding"))
            out_.bundlers.push_back("typescript_helpers");
    }

    std::string_view s_;
    const std::vector<Token>& t_;
    Analysis& out_;
    size_t max_;
    std::map<std::string, std::string> aliases_;
    std::set<size_t> importEquals_;
    bool esm_ = false;
    bool cjs_ = false;
};

std::string Extension(const std::string& name) {
    const size_t slash = name.find_last_of("/\\!");
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
    std::string extension = name.substr(dot);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension;
}

json LocationJson(const Location& at) { return {{"line", at.line}, {"column", at.column}}; }

} // namespace

bool IsScriptName(const std::string& name) {
    static const std::set<std::string> extensions = {".js", ".mjs", ".cjs", ".jsx", ".ts", ".tsx", ".mts", ".cts"};
    return extensions.count(Extension(name)) > 0;
}

bool IsNodeBuiltin(const std::string& specifier) {
    static const std::set<std::string> builtins = {
        "assert", "assert/strict", "async_hooks", "buffer", "child_process", "cluster", "console", "constants",
        "crypto", "dgram", "diagnostics_channel", "dns", "dns/promises", "domain", "events", "fs", "fs/promises",
        "http", "http2", "https", "inspector", "module", "net", "os", "path", "path/posix", "path/win32",
        "perf_hooks", "process", "punycode", "querystring", "readline", "readline/promises", "repl", "stream",
        "stream/promises", "stream/web", "string_decoder", "sys", "timers", "timers/promises", "tls",
        "trace_events", "tty", "url", "util", "util/types", "v8", "vm", "wasi", "worker_threads", "zlib"};
    if (specifier.rfind("node:", 0) == 0) return true;
    return builtins.count(specifier) > 0;
}

Analysis Analyze(std::string_view source, const std::string& name, size_t maxEntries) {
    Analysis analysis;
    analysis.bytes = source.size();
    const std::string extension = Extension(name);
    analysis.language = extension == ".ts" || extension == ".tsx" || extension == ".mts" || extension == ".cts"
        ? "typescript" : "javascript";
    analysis.lex = Lex(source);
    analysis.sourceMappingUrl = analysis.lex.sourceMappingUrl;
    Walker(source, analysis, maxEntries).Run();
    // Minified: long lines and almost no whitespace. A heuristic, labelled
    // as such in the output.
    if (analysis.bytes >= 200 && analysis.lex.lines) {
        const double average = static_cast<double>(analysis.bytes) / analysis.lex.lines;
        const double whitespace = static_cast<double>(analysis.lex.whitespaceBytes) / static_cast<double>(analysis.bytes);
        // One line holding most of the file counts too (a bundle plus its
        // sourceMappingURL comment is two lines).
        const bool oneLine = analysis.lex.longestLine >= 200 && analysis.lex.longestLine * 10 >= analysis.bytes * 8;
        analysis.minified = whitespace < 0.12 && (average > 250 || oneLine || analysis.lex.longestLine > 10000);
    }
    return analysis;
}

json ToJson(const Analysis& analysis, size_t maxEntries) {
    json dependencies = json::array();
    for (const auto& dependency : analysis.dependencies) {
        json row = {{"specifier", dependency.specifier}, {"kind", dependency.kind}, {"at", LocationJson(dependency.at)}};
        if (dependency.typeOnly) row["type_only"] = true;
        if (!IsNodeBuiltin(dependency.specifier) && dependency.specifier.rfind("./", 0) != 0 &&
            dependency.specifier.rfind("../", 0) != 0 && dependency.specifier.rfind('/', 0) != 0)
            row["external"] = true;
        if (IsNodeBuiltin(dependency.specifier)) row["builtin"] = true;
        dependencies.push_back(std::move(row));
    }
    json exports = json::array();
    for (const auto& name : analysis.exports)
        exports.push_back({{"name", name.name}, {"kind", name.kind}, {"at", LocationJson(name.at)}});
    json ipc = json::array();
    for (const auto& use : analysis.ipc)
        ipc.push_back({{"object", use.object}, {"method", use.method}, {"channel", use.channel}, {"role", use.role},
                       {"at", LocationJson(use.at)}});
    json windows = json::array();
    for (const auto& window : analysis.windows) {
        json row = {{"constructor", window.constructorName},
                    {"at", LocationJson(window.at)},
                    {"web_preferences", window.hasWebPreferences ? window.webPreferences : json(nullptr)}};
        if (!window.preloadExpression.empty())
            row["preload"] = {{"expression", window.preloadExpression},
                              {"literals", window.preloadLiterals},
                              {"from_dirname", window.preloadFromDirname}};
        windows.push_back(std::move(row));
    }
    json signatures = json::object();
    for (const auto& [name, count] : analysis.bundlerSignatures) signatures[name] = count;
    const auto& lex = analysis.lex;
    json data = {
        {"format", "javascript"},
        {"language", analysis.language},
        {"module_kind", analysis.moduleKind},
        {"dependencies", dependencies},
        {"computed_dynamic_imports", analysis.computedDynamicImports},
        {"computed_requires", analysis.computedRequires},
        {"exports", exports},
        {"electron", {{"ipc", ipc},
                      {"computed_channels", analysis.computedIpcChannels},
                      {"windows", windows},
                      {"note", "static names found in the source, not observed traffic"}}},
        {"bundle", {{"bundlers", analysis.bundlers},
                    {"signatures", signatures},
                    {"minified", analysis.minified},
                    {"note", "heuristics: identifier signatures, line length and whitespace"}}},
        {"stats", {{"bytes", analysis.bytes},
                   {"lines", lex.lines},
                   {"longest_line", lex.longestLine},
                   {"tokens", lex.tokens.size()},
                   {"comment_bytes", lex.commentBytes},
                   {"whitespace_ratio", analysis.bytes ? static_cast<double>(lex.whitespaceBytes) / analysis.bytes : 0.0}}},
        {"truncated", analysis.truncated || lex.truncated},
        {"max_entries", maxEntries},
    };
    if (!analysis.sourceMappingUrl.empty()) {
        const bool inline_ = analysis.sourceMappingUrl.rfind("data:", 0) == 0;
        std::string shown = analysis.sourceMappingUrl;
        if (shown.size() > 200) shown = shown.substr(0, 200) + "...";
        data["source_map"] = {{"url", shown}, {"inline", inline_}, {"line", lex.sourceMappingLine},
                              {"url_bytes", analysis.sourceMappingUrl.size()}};
    }
    if (!lex.warnings.empty()) data["warnings"] = lex.warnings;
    return data;
}

} // namespace cortex::artifacts::js
