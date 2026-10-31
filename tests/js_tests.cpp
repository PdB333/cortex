// JavaScript/TypeScript, source maps and Electron archives (step 4b).
//
//   cortex_js_tests <tests/fixtures directory>
//
// The fixture app (tests/fixtures/electron-app) is real source; its bundle
// and map come from esbuild and its archive from @electron/asar
// (tests/fixtures/make_electron_fixture.mjs). The expected source-map
// answers are Node's own (node:module SourceMap), for every column.

#include "artifacts/artifact.h"
#include "artifacts/js_analysis.h"
#include "artifacts/js_lexer.h"
#include "artifacts/js_provider.h"
#include "artifacts/source_map.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <set>
#include <sstream>

namespace {

namespace fs = std::filesystem;
namespace js = cortex::artifacts::js;
using namespace cortex::artifacts;
using json = nlohmann::json;

int failures = 0;

#define CHECK(condition)                                                              \
    do {                                                                              \
        if (!(condition)) {                                                           \
            ++failures;                                                               \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #condition \
                      << std::endl;                                                   \
        }                                                                             \
    } while (false)

fs::path g_fixtures;

std::string ReadFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

std::string App(const std::string& relative) { return ReadFile(g_fixtures / "electron-app" / fs::u8path(relative)); }

std::set<std::string> Specifiers(const js::Analysis& analysis, const std::string& kind = {}) {
    std::set<std::string> out;
    for (const auto& dependency : analysis.dependencies)
        if (kind.empty() || dependency.kind == kind) out.insert(dependency.specifier);
    return out;
}

std::set<std::string> Exports(const js::Analysis& analysis) {
    std::set<std::string> out;
    for (const auto& name : analysis.exports) out.insert(name.name);
    return out;
}

std::set<std::string> Channels(const js::Analysis& analysis, const std::string& role) {
    std::set<std::string> out;
    for (const auto& use : analysis.ipc)
        if (use.role == role) out.insert(use.channel);
    return out;
}

std::string Base64(const std::string& bytes) {
    static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const uint32_t v = (uint8_t(bytes[i]) << 16) | (uint8_t(bytes[i + 1]) << 8) | uint8_t(bytes[i + 2]);
        for (int s = 18; s >= 0; s -= 6) out.push_back(alphabet[(v >> s) & 63]);
    }
    if (i + 1 == bytes.size()) {
        const uint32_t v = uint8_t(bytes[i]) << 16;
        out += {alphabet[(v >> 18) & 63], alphabet[(v >> 12) & 63], '=', '='};
    } else if (i + 2 == bytes.size()) {
        const uint32_t v = (uint8_t(bytes[i]) << 16) | (uint8_t(bytes[i + 1]) << 8);
        out += {alphabet[(v >> 18) & 63], alphabet[(v >> 12) & 63], alphabet[(v >> 6) & 63], '='};
    }
    return out;
}

void TestLexer() {
    // Strings, templates (nested), regexes and comments hide their text.
    const std::string tricky = App("lib/tricky.js");
    const auto analysis = js::Analyze(tricky, "tricky.js");
    CHECK(Specifiers(analysis) == std::set<std::string>{"./store.cjs"});
    CHECK(analysis.lex.warnings.empty());
    CHECK(analysis.moduleKind == "cjs");
    CHECK(Exports(analysis) == std::set<std::string>{"module.exports"});
    size_t regexes = 0, divisions = 0, templates = 0;
    for (const auto& token : analysis.lex.tokens) {
        if (token.kind == js::TokenKind::Regex) ++regexes;
        if (token.kind == js::TokenKind::Template) ++templates;
        if (token.kind == js::TokenKind::Punctuator && js::TokenText(tricky, token) == "/") ++divisions;
    }
    CHECK(regexes == 4);    // re, the two in re2, the one in f
    CHECK(divisions == 2);  // 10 / 2 / 1
    CHECK(templates == 4);  // outer head, nested head, nested tail, outer tail

    // Cooked strings and constant templates; a template with ${} is not a constant.
    auto lexed = js::Lex(R"(a = 'x\tyA\u{1F600}\x41'; b = `t\`1`; c = `n${d}`;)");
    std::vector<std::string> values;
    for (const auto& token : lexed.tokens)
        if (token.kind == js::TokenKind::String || (token.kind == js::TokenKind::Template && !token.substitutions))
            values.push_back(token.value);
    CHECK(values.size() == 2 && values[0] == "x\tyA\xF0\x9F\x98\x80" "A" && values[1] == "t`1");

    // Columns are UTF-16 units: é is one, an emoji two.
    lexed = js::Lex("const s = 'é😀'; x");
    CHECK(lexed.tokens.back().line == 1 && lexed.tokens.back().column == 18);
    lexed = js::Lex("a\r\nb\rc\nd");
    CHECK(lexed.tokens.size() == 4 && lexed.tokens[3].line == 4 && lexed.tokens[1].line == 2 && lexed.tokens[2].line == 3);

    // Broken input is reported, never fatal.
    for (const char* broken : {"'abc\nrequire('x')", "/* never closed", "`a ${ b", "x = /never closed\nrequire('y')"}) {
        const auto result = js::Analyze(broken, "broken.js");
        CHECK(!result.lex.warnings.empty() || std::string(broken).rfind("x = /", 0) == 0);
    }
    // After an unterminated string the next line is lexed again.
    CHECK(Specifiers(js::Analyze("'abc\nrequire('x')", "a.js")) == std::set<std::string>{"x"});
    // A regex that cannot close is a division after all.
    CHECK(Specifiers(js::Analyze("x = a /b\nrequire('y')", "a.js")) == std::set<std::string>{"y"});

    // Source map comments.
    lexed = js::Lex("x();\n//# sourceMappingURL=out.js.map\n");
    CHECK(lexed.sourceMappingUrl == "out.js.map" && lexed.sourceMappingLine == 2);
    lexed = js::Lex("x(); /*# sourceMappingURL=a.map */ //@ sourceMappingURL=b.map");
    CHECK(lexed.sourceMappingUrl == "b.map");
    CHECK(js::Lex("s = '//# sourceMappingURL=no.map'").sourceMappingUrl.empty());
}

void TestFixtureScripts() {
    // Electron main process (CommonJS).
    auto main = js::Analyze(App("main.js"), "main.js");
    CHECK(main.moduleKind == "cjs");
    CHECK(Specifiers(main, "require") == (std::set<std::string>{"electron", "node:path", "./lib/store.cjs"}));
    CHECK(Specifiers(main, "dynamic_import") == std::set<std::string>{"./lib/describe.mjs"});
    CHECK(Channels(main, "main_handler") == (std::set<std::string>{"settings:get", "settings:set"}));
    CHECK(Channels(main, "main_listener") == std::set<std::string>{"log:write"});
    CHECK(Channels(main, "main_to_renderer") == std::set<std::string>{"app:ready"});  // not did-finish-load
    CHECK(main.windows.size() == 1);
    if (main.windows.size() == 1) {
        const auto& window = main.windows[0];
        CHECK(window.constructorName == "BrowserWindow" && window.at.line == 8);
        CHECK(window.webPreferences["contextIsolation"] == true && window.webPreferences["nodeIntegration"] == false &&
              window.webPreferences["sandbox"] == true);
        CHECK(window.preloadFromDirname && window.preloadLiterals == std::vector<std::string>{"preload.js"});
        CHECK(window.preloadExpression == "path.join(__dirname, 'preload.js')");
    }
    for (const auto& use : main.ipc)
        if (use.channel == "settings:get") CHECK(use.at.line == 25 && use.at.column == 16);

    // Preload: bridge and renderer-side calls.
    auto preload = js::Analyze(App("preload.js"), "preload.js");
    CHECK(Channels(preload, "bridge_exposure") == std::set<std::string>{"cortexDemo"});
    CHECK(Channels(preload, "renderer_invoke") == (std::set<std::string>{"settings:get", "settings:set"}));
    CHECK(Channels(preload, "renderer_send") == std::set<std::string>{"log:write"});
    CHECK(Channels(preload, "renderer_listener") == std::set<std::string>{"app:ready"});

    // ES modules.
    auto describe = js::Analyze(App("lib/describe.mjs"), "describe.mjs");
    CHECK(describe.moduleKind == "esm");
    CHECK(Specifiers(describe, "import") == std::set<std::string>{"./format.mjs"});
    CHECK(Specifiers(describe, "export_from") == std::set<std::string>{"./format.mjs"});
    CHECK(Exports(describe) == (std::set<std::string>{"format", "*", "default", "describeVersion"}));
    auto format = js::Analyze(App("lib/format.mjs"), "format.mjs");
    CHECK(Exports(format) == (std::set<std::string>{"format", "Formatter"}));

    // CommonJS exports.
    auto store = js::Analyze(App("lib/store.cjs"), "store.cjs");
    CHECK(store.moduleKind == "cjs" && Exports(store) == (std::set<std::string>{"get", "set", "size"}));

    // TypeScript: `type` members do not make the import type-only.
    auto renderer = js::Analyze(App("src/renderer.ts"), "renderer.ts");
    CHECK(renderer.language == "typescript" && renderer.moduleKind == "esm");
    CHECK(Specifiers(renderer) == (std::set<std::string>{"./greet", "./math"}));
    for (const auto& dependency : renderer.dependencies) CHECK(!dependency.typeOnly);
    CHECK(Exports(renderer) == std::set<std::string>{"start"});
    auto greet = js::Analyze(App("src/greet.ts"), "greet.ts");
    bool typeExport = false;
    for (const auto& name : greet.exports) typeExport = typeExport || (name.name == "Greeting" && name.kind == "ts_type");
    CHECK(typeExport);

    // The esbuild bundle: minified, external map, no imports left.
    auto bundle = js::Analyze(App("renderer/bundle.js"), "bundle.js");
    CHECK(bundle.minified && bundle.dependencies.empty() && bundle.sourceMappingUrl == "bundle.js.map");
    CHECK(!js::Analyze(App("main.js"), "main.js").minified);

    // Minified aliases of Electron objects are followed.
    auto minified = js::Analyze(
        R"(const{ipcMain:o,BrowserWindow:n}=require("electron");o.handle("x:y",()=>1);)"
        R"(new n({webPreferences:{preload:p,nodeIntegration:!0,contextIsolation:!1}});o.on(c,f))",
        "m.js");
    CHECK(Channels(minified, "main_handler") == std::set<std::string>{"x:y"});
    CHECK(minified.computedIpcChannels == 1);
    CHECK(minified.windows.size() == 1 && minified.windows[0].webPreferences["nodeIntegration"] == true &&
          minified.windows[0].webPreferences["contextIsolation"] == false);
    auto esm = js::Analyze("import { ipcRenderer as r } from 'electron'; r.invoke('a:b'); import type { T } from './t';",
                           "r.ts");
    CHECK(Channels(esm, "renderer_invoke") == std::set<std::string>{"a:b"});
    bool typeOnly = false;
    for (const auto& dependency : esm.dependencies) typeOnly = typeOnly || (dependency.specifier == "./t" && dependency.typeOnly);
    CHECK(typeOnly);

    // Bundler signatures.
    CHECK(js::Analyze("var __webpack_require__={};(self.webpackChunkapp=self.webpackChunkapp||[]).push([])", "w.js").bundlers ==
          std::vector<std::string>{"webpack"});
    CHECK(js::Analyze("var __defProp=Object.defineProperty;var __commonJS=(cb,mod)=>()=>mod;", "e.js").bundlers ==
          std::vector<std::string>{"esbuild"});
    // Computed specifiers are counted, not invented.
    auto computed = js::Analyze("import(name); require(base + '/x'); require('ok')", "c.js");
    CHECK(computed.computedDynamicImports == 1 && computed.computedRequires == 1 && Specifiers(computed) == std::set<std::string>{"ok"});
    // TypeScript import-equals.
    CHECK(Specifiers(js::Analyze("import fs = require('fs');", "f.ts"), "import_equals") == std::set<std::string>{"fs"});
    // Limits.
    std::string many;
    for (int i = 0; i < 50; ++i) many += "require('m" + std::to_string(i) + "');";
    auto limited = js::Analyze(many, "l.js", 10);
    CHECK(limited.dependencies.size() == 10 && limited.truncated);
}

void TestSourceMap() {
    SourceMap map;
    std::string error;
    const std::string mapText = App("renderer/bundle.js.map");
    CHECK(ParseSourceMap(mapText, map, error));
    CHECK(map.sources == (std::vector<std::string>{"../src/greet.ts", "../src/math.ts", "../src/renderer.ts"}));
    CHECK(std::all_of(map.sourcesContent.begin(), map.sourcesContent.end(), [](const json& c) { return c.is_string(); }));

    const json expected = json::parse(ReadFile(g_fixtures / "electron-app-expected.json"));
    // Node's parser keeps the previous segment's name on a 4-field segment
    // (no name); the format says such a segment has none. Names are compared
    // whenever the segment carries one.
    size_t compared = 0, mismatched = 0, inheritedNames = 0;
    for (const auto& row : expected["columns"]) {
        const json cell = json::parse(row.get<std::string>());
        const json got = LookupJson(map, cell[0], cell[1]);
        const bool match = cell[2].is_null()
            ? got["original"].is_null()
            : (!got["original"].is_null() && got["original"]["source"] == cell[2] && got["original"]["line"] == cell[3] &&
               got["original"]["column"] == cell[4] &&
               (got["original"]["name"] == cell[5] || (got["original"]["name"].is_null() && ++inheritedNames)));
        ++compared;
        if (!match && ++mismatched <= 5) std::cerr << "lookup differs from node at " << cell.dump() << ": " << got.dump() << "\n";
    }
    CHECK(compared > 200 && mismatched == 0 && inheritedNames < compared / 4);
    for (const auto& lookup : expected["lookups"]) {
        const json got = LookupJson(map, lookup["generated"]["line"], lookup["generated"]["column"]);
        CHECK(got["original"]["line"] == lookup["original"]["line"]);
    }
    // The original line comes from sourcesContent.
    const json greet = LookupJson(map, 1, 36);
    CHECK(greet["original"].value("line_text", std::string()).find("Hello from the") != std::string::npos);

    // Malformed maps are refused with a reason.
    SourceMap bad;
    CHECK(!ParseSourceMap("{", bad, error) && error == "sourcemap_not_json");
    CHECK(!ParseSourceMap(R"({"version":2,"mappings":""})", bad, error) && error == "sourcemap_version_unsupported");
    CHECK(!ParseSourceMap(R"({"version":3,"sections":[]})", bad, error) && error == "sourcemap_index_map_unsupported");
    CHECK(!ParseSourceMap(R"({"version":3,"sources":[],"mappings":"A!"})", bad, error) &&
          error.rfind("sourcemap_vlq_invalid", 0) == 0);
    CHECK(!ParseSourceMap(R"({"version":3,"sources":[],"mappings":"gggggggggggB"})", bad, error));
    CHECK(!ParseSourceMap(R"({"version":3,"sources":["a"],"mappings":"AAAAAA"})", bad, error) &&
          error.rfind("sourcemap_segment_too_long", 0) == 0);
    CHECK(ParseSourceMap(R"()]}')" "\n" R"({"version":3,"sources":["a"],"mappings":"AAAA;AACA"})", bad, error) &&
          bad.mappings.size() == 2);
    // A source index beyond the list is a warning, not a crash.
    CHECK(ParseSourceMap(R"({"version":3,"sources":["a"],"mappings":"ACAA"})", bad, error) && !bad.warnings.empty() &&
          LookupJson(bad, 1, 1)["original"].is_null());

    // A script with an inline map gives the same answers.
    const std::string inlineScript =
        App("renderer/bundle.js").substr(0, App("renderer/bundle.js").find("//# sourceMappingURL")) +
        "//# sourceMappingURL=data:application/json;charset=utf-8;base64," + Base64(mapText) + "\n";
    const auto script = FromBytes(std::vector<uint8_t>(inlineScript.begin(), inlineScript.end()), "inline.js");
    json position = SourcePosition(*script, 1, 97, nullptr, error);
    CHECK(position.value("ok", false) && position["original"]["source"] == "../src/math.ts" &&
          position["original"]["name"] == "value" && position["map"]["inline"] == true);
    const json inspected = DefaultRegistry().Inspect(*script);
    CHECK(inspected["format"] == "javascript" && inspected["data"]["source_map"]["inline"] == true &&
          inspected["data"]["source_map"]["summary"]["sources_count"] == 3);
    // Without a reachable map it says so.
    const auto noMap = FromBytes({'x', '(', ')', ';'}, "plain.js");
    CHECK(SourcePosition(*noMap, 1, 1, nullptr, error).is_null() && error == "no_source_map");
}

void TestArchive() {
    const std::string asarPath = (g_fixtures / "electron-app.asar").u8string();
    std::string error;
    const auto archive = OpenFile(asarPath, &error);
    CHECK(archive != nullptr);
    if (!archive) return;
    const auto& registry = DefaultRegistry();
    const json result = registry.Inspect(*archive);
    CHECK(result["format"] == "asar" && result["provider"]["version"] == "2");
    const json& data = result["data"];
    CHECK(data["header_sha256"].get<std::string>().size() == 64);
    CHECK(data["integrity"]["checked"] == data["files_with_integrity"] && data["integrity"]["matched"] == data["files_with_integrity"] &&
          data["integrity"]["mismatches"].empty());
    CHECK(data["package_json"]["main"] == "main.js");

    const json& electron = data["electron"];
    CHECK(electron["main_resolved"] == "main.js");
    CHECK(electron["windows"].size() == 1 && electron["windows"][0]["preload"]["resolved"] == "preload.js");
    CHECK(electron["bridge"].size() == 1 && electron["bridge"][0]["key"] == "cortexDemo");
    std::map<std::string, json> channels;
    for (const auto& row : electron["ipc"]["channels"]) channels[row["channel"].get<std::string>()] = row;
    CHECK(channels.size() == 4);
    CHECK(channels["settings:get"]["handled_in_main"] == true && channels["settings:get"]["called_from_renderer"] == true);
    CHECK(channels["app:ready"]["sent_to_renderer"] == true && channels["app:ready"]["renderer_listens"] == true);
    CHECK(electron["ipc"]["called_without_handler"].empty() && electron["ipc"]["handled_without_caller"].empty());
    const auto has = [](const json& list, const char* item) {
        return std::find(list.begin(), list.end(), json(item)) != list.end();
    };
    const json& processes = electron["processes"];
    CHECK(has(processes["main"], "main.js") && has(processes["main"], "lib/store.cjs") &&
          has(processes["main"], "lib/describe.mjs") && has(processes["main"], "lib/format.mjs"));
    CHECK(processes["preload"] == json::array({"preload.js"}));
    CHECK(processes["renderer"] == json::array({"renderer/bundle.js"}));
    CHECK(!has(processes["main"], "lib/tricky.js"));  // nothing requires it

    // Module graph: relative and TypeScript-style resolution, packages, builtins.
    std::set<std::pair<std::string, std::string>> edges;
    for (const auto& edge : data["modules"]["edges"]) edges.insert({edge["from"].get<std::string>(), edge["to"].get<std::string>()});
    CHECK(edges.count({"main.js", "lib/store.cjs"}) && edges.count({"src/renderer.ts", "src/greet.ts"}) &&
          edges.count({"lib/tricky.js", "lib/store.cjs"}) && edges.count({"lib/describe.mjs", "lib/format.mjs"}));
    CHECK(data["modules"]["unresolved"].empty());
    CHECK(data["modules"]["packages"]["electron"]["provided_by_runtime"] == true);
    CHECK(data["modules"]["builtins"]["path"] == 1);
    CHECK(data["source_maps"].size() == 1 && data["source_maps"][0]["resolved"] == "renderer/bundle.js.map");

    // A nested script and its sibling map, as the MCP tool resolves them.
    const auto bundle = registry.OpenChild(archive, "renderer/bundle.js", error);
    CHECK(bundle != nullptr);
    if (bundle) {
        CHECK(registry.Inspect(*bundle)["data"]["bundle"]["minified"] == true);
        const json position = SourcePosition(*bundle, 1, 171, [&](const std::string& url, std::string& openError) {
            std::string sibling;
            JoinArchivePath("renderer", url, sibling);
            return registry.OpenChild(archive, sibling, openError);
        }, error);
        CHECK(position.value("ok", false) && position["original"]["source"] == "../src/renderer.ts" &&
              position["original"]["line"] == 15 && position["original"]["column"] == 52);
    }
    const auto mapChild = registry.OpenChild(archive, "renderer/bundle.js.map", error);
    CHECK(mapChild && registry.Inspect(*mapChild)["format"] == "sourcemap");

    // One changed byte in main.js breaks its integrity record, and only its.
    auto bytes = archive->ReadSome(0, static_cast<size_t>(archive->Size()));
    std::string mainOffset;
    for (const auto& child : registry.Find("asar")->Children(*archive, {}, error))
        if (child.name == "main.js") bytes[static_cast<size_t>(child.offset + 10)] ^= 0x20;
    const auto tampered = FromBytes(bytes, "tampered.asar");
    const json damaged = registry.Inspect(*tampered)["data"]["integrity"];
    CHECK(damaged["mismatches"].size() == 1 && damaged["mismatches"][0]["file"] == "main.js" &&
          damaged["mismatches"][0]["result"] == "hash_mismatch");

    // Path joining never climbs out.
    std::string joined;
    CHECK(JoinArchivePath("a/b", "../c/./d.js", joined) && joined == "a/c/d.js");
    CHECK(!JoinArchivePath("a", "../../x", joined));
}

void Fuzz() {
    std::mt19937 random(4242);
    const std::vector<std::string> seeds = {App("main.js"), App("lib/tricky.js"), App("renderer/bundle.js"),
                                            App("src/renderer.ts")};
    const std::string mapText = App("renderer/bundle.js.map");
    const char alphabet[] = "`'\"/\\*{}()[]$#!=<>.,;: \n\rrequireimportexport";
    for (int round = 0; round < 3000; ++round) {
        std::string text = seeds[round % seeds.size()];
        const int edits = 1 + static_cast<int>(random() % 16);
        for (int e = 0; e < edits && !text.empty(); ++e) {
            const size_t at = random() % text.size();
            switch (random() % 3) {
                case 0: text[at] = alphabet[random() % (sizeof(alphabet) - 1)]; break;
                case 1: text.erase(at, 1 + random() % 8); break;
                default: text.insert(at, 1, static_cast<char>(random() % 256)); break;
            }
        }
        const auto analysis = js::Analyze(text, round % 2 ? "f.ts" : "f.js", 64);
        (void)js::ToJson(analysis, 64);
        std::string mutated = mapText;
        for (int e = 0; e < 4; ++e) mutated[random() % mutated.size()] = alphabet[random() % (sizeof(alphabet) - 1)];
        SourceMap map;
        std::string error;
        if (ParseSourceMap(mutated, map, error)) (void)LookupJson(map, 1 + random() % 3, 1 + random() % 400);
    }
}

} // namespace

int main(int argc, char** argv) {
    g_fixtures = argc > 1 ? fs::u8path(argv[1]) : fs::path("tests/fixtures");
    if (!fs::exists(g_fixtures / "electron-app" / "main.js")) {
        std::cerr << "fixtures not found under " << g_fixtures.u8string() << std::endl;
        return 2;
    }
    TestLexer();
    TestFixtureScripts();
    TestSourceMap();
    TestArchive();
    Fuzz();
    if (failures) {
        std::cerr << failures << " JavaScript check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "JavaScript, source map and Electron archive tests passed" << std::endl;
    return 0;
}
