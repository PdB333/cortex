#include "js_provider.h"

#include "js_analysis.h"
#include "sha256.h"
#include "source_map.h"

#include <algorithm>
#include <deque>
#include <map>
#include <set>

namespace cortex::artifacts {
namespace {

constexpr uint64_t kMaxScriptBytes = 32ull << 20;     // one script
constexpr uint64_t kMaxMapBytes = 64ull << 20;        // one source map
constexpr uint64_t kTreeScriptBytes = 16ull << 20;    // one script inside a tree
constexpr uint64_t kTreeTotalBytes = 96ull << 20;     // all scripts of a tree
constexpr size_t kTreeMaxScripts = 2000;

std::string ReadText(const ByteSource& source, uint64_t limit, bool& truncated) {
    const uint64_t size = std::min<uint64_t>(source.Size(), limit);
    truncated = source.Size() > limit;
    std::string text(static_cast<size_t>(size), '\0');
    if (size && !source.Read(0, text.data(), text.size())) return {};
    return text;
}

bool LooksBinary(const std::string& text) {
    const size_t probe = std::min<size_t>(text.size(), 4096);
    return std::find(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(probe), '\0') !=
           text.begin() + static_cast<std::ptrdiff_t>(probe);
}

std::string BaseName(const std::string& name) {
    const size_t cut = name.find_last_of("/\\!");
    return cut == std::string::npos ? name : name.substr(cut + 1);
}

std::string DirName(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

bool EndsWith(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

json Located(const std::string& file, const js::Location& at) {
    return {{"file", file}, {"line", at.line}, {"column", at.column}};
}

} // namespace

bool JoinArchivePath(const std::string& base, const std::string& relative, std::string& out) {
    std::vector<std::string> parts;
    auto push = [&](const std::string& text) {
        size_t start = 0;
        while (start <= text.size()) {
            size_t end = text.find_first_of("/\\", start);
            if (end == std::string::npos) end = text.size();
            const std::string segment = text.substr(start, end - start);
            if (segment == "..") {
                if (parts.empty()) return false;
                parts.pop_back();
            } else if (!segment.empty() && segment != ".") {
                parts.push_back(segment);
            }
            start = end + 1;
        }
        return true;
    };
    if (!(relative.rfind('/', 0) == 0 ? true : push(base)) || !push(relative)) return false;
    out.clear();
    for (const auto& part : parts) out += (out.empty() ? "" : "/") + part;
    return true;
}

std::vector<Capability> JavaScriptProvider::Capabilities() const {
    return {{"static", CapabilityStatus::Validated,
             "cortex_js_tests (electron-app fixture, esbuild bundle, lexer traps, fuzzing)",
             "tokens (strings, templates, regexes, comments), ESM/CommonJS/dynamic imports, exports, Electron IPC "
             "and BrowserWindow options, bundler signatures, source map URL"},
            {"decompile", CapabilityStatus::Unavailable, "",
             "the file is already source; no beautifier or deobfuscator is provided"},
            {"live", CapabilityStatus::Unavailable, "", "not from the file: a running Node.js or Electron process is followed by the CDP live provider (cortex_live_attach)"}};
}

int JavaScriptProvider::Detect(const ByteSource& source) const {
    if (js::IsScriptName(BaseName(source.Name()))) return 30;
    // A script without extension: a Node shebang.
    const auto head = source.ReadSome(0, 64);
    const std::string text(head.begin(), head.end());
    if (text.rfind("#!", 0) == 0 && text.find("node") != std::string::npos && text.find('\n') != std::string::npos)
        return 20;
    return 0;
}

json JavaScriptProvider::Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const {
    bool truncated = false;
    const std::string text = ReadText(source, kMaxScriptBytes, truncated);
    if (source.Size() && text.empty()) {
        error = "read_failed";
        return nullptr;
    }
    if (LooksBinary(text)) {
        error = "not_text";
        return nullptr;
    }
    const auto analysis = js::Analyze(text, BaseName(source.Name()), options.maxEntries);
    json data = js::ToJson(analysis, options.maxEntries);
    if (truncated) data["warnings"].push_back("only the first " + std::to_string(kMaxScriptBytes) + " bytes were analysed");
    if (analysis.sourceMappingUrl.rfind("data:", 0) == 0) {
        std::string mapText, mapError;
        SourceMap map;
        if (!DecodeDataUrl(analysis.sourceMappingUrl, mapText)) data["source_map"]["error"] = "data_url_not_decoded";
        else if (!ParseSourceMap(mapText, map, mapError)) data["source_map"]["error"] = mapError;
        else data["source_map"]["summary"] = SourceMapSummary(map, options.maxEntries);
    }
    return data;
}

std::vector<Capability> SourceMapProvider::Capabilities() const {
    return {{"static", CapabilityStatus::Validated,
             "cortex_js_tests (esbuild map: every column checked against node:module SourceMap)",
             "revision 3 maps: sources, embedded content, VLQ mappings, position lookup; index maps are refused"},
            {"decompile", CapabilityStatus::Unavailable, "", "not applicable"},
            {"live", CapabilityStatus::Unavailable, "", "not applicable"}};
}

int SourceMapProvider::Detect(const ByteSource& source) const {
    const auto head = source.ReadSome(0, 4096);
    const std::string text(head.begin(), head.end());
    const size_t start = text.find_first_not_of(" \t\r\n");
    const bool object = start != std::string::npos && (text[start] == '{' || text.compare(start, 4, ")]}'") == 0);
    if (!object) return 0;
    if (EndsWith(BaseName(source.Name()), ".map")) return 45;
    return text.find("\"mappings\"") != std::string::npos && text.find("\"version\"") != std::string::npos ? 25 : 0;
}

json SourceMapProvider::Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const {
    bool truncated = false;
    const std::string text = ReadText(source, kMaxMapBytes, truncated);
    if (truncated) {
        error = "sourcemap_too_large";
        return nullptr;
    }
    SourceMap map;
    if (!ParseSourceMap(text, map, error)) return nullptr;
    return SourceMapSummary(map, options.maxEntries);
}

json SourcePosition(const ByteSource& artifact, uint32_t line, uint32_t column, const MapOpener& open,
                    std::string& error) {
    if (line == 0 || column == 0) {
        error = "position_is_1_based";
        return nullptr;
    }
    bool truncated = false;
    std::string text = ReadText(artifact, kMaxMapBytes, truncated);
    if (truncated) {
        error = "artifact_too_large";
        return nullptr;
    }
    json mapInfo;
    SourceMap map;
    // A source map itself, or a script naming one.
    if (SourceMapProvider().Detect(artifact) > 0) {
        if (!ParseSourceMap(text, map, error)) return nullptr;
        mapInfo = {{"name", artifact.Name()}, {"inline", false}};
    } else {
        const auto lex = js::Lex(text);
        const std::string url = lex.sourceMappingUrl;
        if (url.empty()) {
            error = "no_source_map";
            return nullptr;
        }
        std::string mapText;
        if (url.rfind("data:", 0) == 0) {
            if (!DecodeDataUrl(url, mapText)) {
                error = "data_url_not_decoded";
                return nullptr;
            }
            mapInfo = {{"name", artifact.Name()}, {"inline", true}};
        } else {
            if (!open) {
                error = "source_map_not_reachable";
                return nullptr;
            }
            const auto mapSource = open(url, error);
            if (!mapSource) return nullptr;
            mapText = ReadText(*mapSource, kMaxMapBytes, truncated);
            if (truncated) {
                error = "sourcemap_too_large";
                return nullptr;
            }
            mapInfo = {{"name", mapSource->Name()}, {"inline", false}, {"url", url}};
        }
        if (!ParseSourceMap(mapText, map, error)) return nullptr;
        text.swap(mapText);
    }
    Sha256 digest;
    digest.Update(text.data(), text.size());
    mapInfo["sha256"] = Sha256::Hex(digest.Final());
    json result = LookupJson(map, line, column);
    result["map"] = mapInfo;
    result["ok"] = true;
    return result;
}

namespace {

struct Script {
    std::string file;
    js::Analysis analysis;
};

struct Resolved {
    std::string target;   // archive entry, when found
    std::string reason;   // why not, otherwise
};

class Tree {
public:
    Tree(const ByteSource& container, const std::vector<Child>& children) : container_(container) {
        for (const auto& child : children) entries_[child.name] = &child;
    }

    bool Read(const Child& child, uint64_t limit, std::string& text) const {
        if (!child.readable || child.size > limit) return false;
        text.assign(static_cast<size_t>(child.size), '\0');
        return child.size == 0 || container_.Read(child.offset, text.data(), text.size());
    }

    bool Has(const std::string& name) const { return entries_.count(name) > 0; }
    const Child* Find(const std::string& name) const {
        const auto it = entries_.find(name);
        return it == entries_.end() ? nullptr : it->second;
    }
    const std::map<std::string, const Child*>& Entries() const { return entries_; }

    // Node's resolution, restricted to what the archive holds.
    Resolved ResolveFile(const std::string& path) const {
        static const char* const extensions[] = {".js", ".mjs", ".cjs", ".ts", ".tsx", ".mts", ".cts", ".jsx", ".json", ".node"};
        if (Has(path)) return {path, {}};
        for (const char* extension : extensions)
            if (Has(path + extension)) return {path + extension, {}};
        // TypeScript sources import "./x.js" for "./x.ts".
        for (const char* from : {".js", ".mjs", ".cjs"}) {
            if (!EndsWith(path, from)) continue;
            const std::string stem = path.substr(0, path.size() - std::string(from).size());
            for (const char* to : {".ts", ".tsx", ".mts", ".cts"})
                if (Has(stem + to)) return {stem + to, {}};
        }
        if (const Child* package = Find(path + "/package.json")) {
            std::string text;
            if (Read(*package, 1u << 20, text)) {
                const json manifest = json::parse(text, nullptr, false);
                if (manifest.is_object() && manifest.contains("main") && manifest["main"].is_string()) {
                    std::string main;
                    if (JoinArchivePath(path, manifest["main"].get<std::string>(), main)) {
                        const auto resolved = ResolveFile(main);
                        if (!resolved.target.empty()) return resolved;
                    }
                }
            }
        }
        for (const char* extension : extensions)
            if (Has(path + "/index" + extension)) return {path + "/index" + extension, {}};
        return {{}, "not_in_archive"};
    }

    Resolved Resolve(const std::string& from, const std::string& specifier) const {
        if (specifier.rfind("./", 0) == 0 || specifier.rfind("../", 0) == 0 || specifier == "." || specifier == "..") {
            std::string path;
            if (!JoinArchivePath(DirName(from), specifier, path)) return {{}, "outside_archive"};
            return ResolveFile(path);
        }
        if (specifier.rfind('/', 0) == 0) return {{}, "absolute_path"};
        return {{}, "package"};
    }

private:
    const ByteSource& container_;
    std::map<std::string, const Child*> entries_;
};

std::string PackageName(const std::string& specifier) {
    const size_t first = specifier.find('/');
    if (specifier.rfind('@', 0) == 0 && first != std::string::npos) {
        const size_t second = specifier.find('/', first + 1);
        return specifier.substr(0, second);
    }
    return specifier.substr(0, first);
}

// <script src="..."> of an HTML page.
std::vector<std::string> ScriptSources(const std::string& html) {
    std::vector<std::string> sources;
    std::string lower(html.size(), '\0');
    std::transform(html.begin(), html.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    size_t at = 0;
    while ((at = lower.find("<script", at)) != std::string::npos && sources.size() < 64) {
        const size_t close = lower.find('>', at);
        if (close == std::string::npos) break;
        const std::string tag = lower.substr(at, close - at);
        const size_t src = tag.find("src=");
        if (src != std::string::npos && src + 4 < tag.size()) {
            const char quote = tag[src + 4];
            size_t begin = src + 4, end;
            if (quote == '"' || quote == '\'') {
                ++begin;
                end = tag.find(quote, begin);
            } else {
                end = tag.find_first_of(" \t\r\n", begin);
            }
            if (end == std::string::npos) end = tag.size();
            sources.push_back(html.substr(at + begin, end - begin));  // original case
        }
        at = close;
    }
    return sources;
}

} // namespace

json AnalyzeScriptTree(const ByteSource& container, const std::vector<Child>& children, const json& packageJson,
                       const InspectOptions& options) {
    Tree tree(container, children);
    std::vector<Script> scripts;
    uint64_t analysedBytes = 0;
    size_t nodeModules = 0, skippedLarge = 0, skippedBudget = 0, unreadable = 0;
    for (const auto& child : children) {
        if (!js::IsScriptName(child.name)) continue;
        if (child.name.rfind("node_modules/", 0) == 0 || child.name.find("/node_modules/") != std::string::npos) {
            ++nodeModules;  // packages are listed, their code is not analysed here
            continue;
        }
        if (!child.readable) { ++unreadable; continue; }
        if (child.size > kTreeScriptBytes) { ++skippedLarge; continue; }
        if (analysedBytes + child.size > kTreeTotalBytes || scripts.size() >= kTreeMaxScripts) { ++skippedBudget; continue; }
        std::string text;
        if (!tree.Read(child, kTreeScriptBytes, text)) { ++unreadable; continue; }
        analysedBytes += child.size;
        Script script{child.name, js::Analyze(text, child.name, options.maxEntries)};
        script.analysis.lex.tokens.clear();
        script.analysis.lex.tokens.shrink_to_fit();
        scripts.push_back(std::move(script));
    }

    // Module graph.
    json nodes = json::array(), edges = json::array(), unresolved = json::array();
    json packages = json::object(), builtins = json::object();
    std::map<std::string, std::vector<std::string>> adjacency;
    bool usesElectron = false, truncated = false;
    for (const auto& script : scripts) {
        const auto& a = script.analysis;
        if (nodes.size() < options.maxEntries)
            nodes.push_back({{"file", script.file},
                             {"language", a.language},
                             {"module_kind", a.moduleKind},
                             {"minified", a.minified},
                             {"bundlers", a.bundlers},
                             {"bytes", a.bytes},
                             {"dependencies", a.dependencies.size()},
                             {"exports", a.exports.size()}});
        else truncated = true;
        for (const auto& dependency : a.dependencies) {
            const std::string& specifier = dependency.specifier;
            if (specifier == "electron") usesElectron = true;
            if (js::IsNodeBuiltin(specifier)) {
                const std::string name = specifier.rfind("node:", 0) == 0 ? specifier.substr(5) : specifier;
                builtins[name] = builtins.value(name, 0) + 1;
                continue;
            }
            const auto resolved = tree.Resolve(script.file, specifier);
            if (resolved.reason == "package") {
                const std::string name = PackageName(specifier);
                json& package = packages[name];
                if (!package.is_object()) package = {{"imports", 0}};
                package["imports"] = package["imports"].get<int>() + 1;
                const auto inArchive = tree.ResolveFile("node_modules/" + name);
                package["in_archive"] = !inArchive.target.empty();
                if (name == "electron") package["provided_by_runtime"] = true;
                continue;
            }
            if (!resolved.target.empty()) {
                adjacency[script.file].push_back(resolved.target);
                if (edges.size() < options.maxEntries)
                    edges.push_back({{"from", script.file}, {"to", resolved.target}, {"kind", dependency.kind},
                                     {"line", dependency.at.line}});
                else truncated = true;
            } else if (unresolved.size() < options.maxEntries) {
                unresolved.push_back({{"from", script.file}, {"specifier", specifier}, {"kind", dependency.kind},
                                      {"line", dependency.at.line}, {"reason", resolved.reason}});
            } else {
                truncated = true;
            }
        }
    }

    auto reach = [&](const std::vector<std::string>& roots) {
        std::set<std::string> seen;
        std::deque<std::string> queue(roots.begin(), roots.end());
        while (!queue.empty() && seen.size() < 100000) {
            const std::string file = queue.front();
            queue.pop_front();
            if (!seen.insert(file).second) continue;
            const auto next = adjacency.find(file);
            if (next != adjacency.end())
                for (const auto& target : next->second) queue.push_back(target);
        }
        return seen;
    };

    json result = {{"scripts", {{"analysed", scripts.size()},
                                {"analysed_bytes", analysedBytes},
                                {"in_node_modules", nodeModules},
                                {"skipped_too_large", skippedLarge},
                                {"skipped_budget", skippedBudget},
                                {"unreadable", unreadable}}},
                   {"modules", {{"nodes", nodes},
                                {"edges", edges},
                                {"unresolved", unresolved},
                                {"packages", packages},
                                {"builtins", builtins},
                                {"truncated", truncated},
                                {"note", "static: every import/require with a constant specifier, whether or not it runs"}}}};

    // Source maps named by the scripts.
    json maps = json::array();
    for (const auto& script : scripts) {
        const std::string& url = script.analysis.sourceMappingUrl;
        if (url.empty() || maps.size() >= options.maxEntries) continue;
        json row = {{"file", script.file}, {"inline", url.rfind("data:", 0) == 0}};
        if (!row["inline"].get<bool>()) {
            row["url"] = url;
            std::string target;
            if (url.find("://") == std::string::npos && JoinArchivePath(DirName(script.file), url, target) && tree.Has(target))
                row["resolved"] = target;
            else
                row["resolved"] = nullptr;
        }
        maps.push_back(std::move(row));
    }
    if (!maps.empty()) result["source_maps"] = maps;

    if (!usesElectron && !packageJson.is_object()) return result;

    // Electron: processes are approximated statically. Main is what the
    // package's entry reaches; a preload is a file a BrowserWindow names;
    // renderer pages are the HTML files and the scripts they load.
    std::string main = packageJson.is_object() ? packageJson.value("main", std::string("index.js")) : std::string("index.js");
    std::string mainPath;
    Resolved mainResolved;
    if (JoinArchivePath("", main, mainPath)) mainResolved = tree.ResolveFile(mainPath);
    const auto mainSet = mainResolved.target.empty() ? std::set<std::string>() : reach({mainResolved.target});

    json windows = json::array(), bridge = json::array();
    std::vector<std::string> preloadRoots;
    std::map<std::string, std::vector<json>> channels;
    size_t computedChannels = 0;
    for (const auto& script : scripts) {
        computedChannels += script.analysis.computedIpcChannels;
        for (const auto& window : script.analysis.windows) {
            json row = Located(script.file, window.at);
            row["constructor"] = window.constructorName;
            row["web_preferences"] = window.hasWebPreferences ? window.webPreferences : json(nullptr);
            if (!window.preloadExpression.empty()) {
                json preload = {{"expression", window.preloadExpression}, {"resolved", nullptr}};
                // path.join(__dirname, 'a', 'b.js') joins onto this file's folder.
                if (window.preloadFromDirname && !window.preloadLiterals.empty()) {
                    std::string joined = DirName(script.file), next;
                    bool ok = true;
                    for (const auto& literal : window.preloadLiterals) {
                        ok = ok && JoinArchivePath(joined, literal, next);
                        joined = next;
                    }
                    const auto resolved = ok ? tree.ResolveFile(joined) : Resolved{};
                    if (!resolved.target.empty()) {
                        preload["resolved"] = resolved.target;
                        preload["resolution"] = "path.join(__dirname, literals)";
                        preloadRoots.push_back(resolved.target);
                    }
                }
                row["preload"] = std::move(preload);
            }
            if (windows.size() < options.maxEntries) windows.push_back(std::move(row));
        }
        for (const auto& use : script.analysis.ipc) {
            json row = Located(script.file, use.at);
            row["object"] = use.object;
            row["method"] = use.method;
            row["role"] = use.role;
            if (use.role == "bridge_exposure") {
                row["key"] = use.channel;
                if (bridge.size() < options.maxEntries) bridge.push_back(std::move(row));
                continue;
            }
            auto& list = channels[use.channel];
            if (list.size() < 64) list.push_back(std::move(row));
        }
    }
    const auto preloadSet = reach(preloadRoots);

    json pages = json::array();
    std::vector<std::string> rendererRoots;
    for (const auto& [name, child] : tree.Entries()) {
        if (!(EndsWith(name, ".html") || EndsWith(name, ".htm")) || name.rfind("node_modules/", 0) == 0) continue;
        std::string html;
        if (!tree.Read(*child, 4u << 20, html)) continue;
        json scriptsOfPage = json::array();
        for (const auto& source : ScriptSources(html)) {
            std::string path;
            json row = {{"src", source}, {"resolved", nullptr}};
            if (source.find("://") == std::string::npos && JoinArchivePath(DirName(name), source, path)) {
                const auto resolved = tree.ResolveFile(path);
                if (!resolved.target.empty()) {
                    row["resolved"] = resolved.target;
                    rendererRoots.push_back(resolved.target);
                }
            }
            scriptsOfPage.push_back(std::move(row));
        }
        if (pages.size() < options.maxEntries) pages.push_back({{"page", name}, {"scripts", scriptsOfPage}});
    }
    const auto rendererSet = reach(rendererRoots);

    json channelRows = json::array(), withoutHandler = json::array(), withoutCaller = json::array();
    for (const auto& [channel, uses] : channels) {
        bool handled = false, called = false, toRenderer = false, rendererListens = false;
        for (const auto& use : uses) {
            const std::string role = use["role"];
            handled = handled || role == "main_handler" || role == "main_listener";
            called = called || role == "renderer_invoke" || role == "renderer_send";
            toRenderer = toRenderer || role == "main_to_renderer";
            rendererListens = rendererListens || role == "renderer_listener";
        }
        if (called && !handled) withoutHandler.push_back(channel);
        if (handled && !called) withoutCaller.push_back(channel);
        if (channelRows.size() < options.maxEntries)
            channelRows.push_back({{"channel", channel},
                                   {"handled_in_main", handled},
                                   {"called_from_renderer", called},
                                   {"sent_to_renderer", toRenderer},
                                   {"renderer_listens", rendererListens},
                                   {"uses", uses}});
    }

    auto setJson = [](const std::set<std::string>& set) { return json(std::vector<std::string>(set.begin(), set.end())); };
    result["electron"] = {
        {"main", main},
        {"main_resolved", mainResolved.target.empty() ? json(nullptr) : json(mainResolved.target)},
        {"windows", windows},
        {"bridge", bridge},
        {"ipc", {{"channels", channelRows},
                 {"computed_channels", computedChannels},
                 {"called_without_handler", withoutHandler},
                 {"handled_without_caller", withoutCaller}}},
        {"renderer_pages", pages},
        {"processes", {{"main", setJson(mainSet)}, {"preload", setJson(preloadSet)}, {"renderer", setJson(rendererSet)}}},
        {"note", "static view: channels and processes are what the source names, not observed behaviour. A channel "
                 "built at run time is only counted (computed_channels)."},
    };
    return result;
}

} // namespace cortex::artifacts
