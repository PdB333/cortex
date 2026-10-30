// MCP artifact tools (mcp_bridge/artifact_tools.h): files only inside the
// operator's --artifact-root directories, bounded reads, nested containers.
//
//   cortex_artifact_tools_tests [pe fixture]

#include "../mcp_bridge/artifact_tools.h"

#include <chrono>
#include <fstream>
#include <iostream>

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;
namespace tools = mcp_bridge::artifacts;

int failures = 0;

#define CHECK(condition)                                                              \
    do {                                                                              \
        if (!(condition)) {                                                           \
            ++failures;                                                               \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #condition \
                      << std::endl;                                                   \
        }                                                                             \
    } while (false)

void WriteBytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::vector<uint8_t> Asar(const json& tree, const std::string& payload) {
    const std::string text = json{{"files", tree}}.dump();
    const uint32_t length = static_cast<uint32_t>(text.size()), padded = (length + 3) & ~3u;
    std::vector<uint8_t> out;
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i))); };
    u32(4); u32(8 + padded); u32(4 + padded); u32(length);
    out.insert(out.end(), text.begin(), text.end());
    out.resize(16 + padded, 0);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

} // namespace

int main(int argc, char** argv) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path base = fs::temp_directory_path() / ("cortex-artifact-tools-" + std::to_string(stamp));
    const fs::path root = base / "root";
    const fs::path lookalike = base / "root2";  // shares the prefix "root"
    const fs::path outside = base / "outside";
    fs::create_directories(root / "app");
    fs::create_directories(lookalike);
    fs::create_directories(outside);

    // An Electron-like layout: app/resources.asar holding an inner asar.
    const std::string mainJs = "require('./preload.js');\n";
    const auto inner = Asar({{"index.js", {{"size", 7}, {"offset", "0"}}}}, "exports");
    const std::string innerText(inner.begin(), inner.end());
    WriteBytes(root / "app" / "app.asar",
               Asar({{"main.js", {{"size", mainJs.size()}, {"offset", "0"}}},
                     {"vendor.asar", {{"size", inner.size()}, {"offset", std::to_string(mainJs.size())}}}},
                    mainJs + innerText));
    WriteBytes(lookalike / "secret.bin", {1, 2, 3});
    WriteBytes(outside / "secret.bin", {4, 5, 6});
    std::vector<uint8_t> large(200000, 'x');
    WriteBytes(root / "large.bin", large);

    std::vector<std::string> problems;
    const auto roots = tools::CanonicalRoots({root.u8string(), (base / "missing").u8string()}, problems);
    CHECK(roots.size() == 1 && problems.size() == 1);

    auto call = [&](const char* name, json args, const std::vector<fs::path>& useRoots) {
        return tools::Handle(name, args, useRoots);
    };
    const std::string asarPath = (root / "app" / "app.asar").u8string();

    // No root configured: nothing is reachable.
    CHECK(call("cortex_artifact_inspect", {{"path", asarPath}}, {})["error"] == "artifact_roots_not_configured");

    // Inside the root.
    json result = call("cortex_artifact_inspect", {{"path", asarPath}}, roots);
    CHECK(result["ok"] == true && result["format"] == "asar" && result["identity"]["sha256"].get<std::string>().size() == 64);
    result = call("cortex_artifact_children", {{"path", asarPath}}, roots);
    CHECK(result["ok"] == true && result["children"].size() == 2);

    // Escapes are refused before anything is opened.
    CHECK(call("cortex_artifact_read", {{"path", (root / ".." / "outside" / "secret.bin").u8string()}}, roots)["error"] ==
          "path_outside_artifact_roots");
    CHECK(call("cortex_artifact_read", {{"path", (lookalike / "secret.bin").u8string()}}, roots)["error"] ==
          "path_outside_artifact_roots");
    CHECK(call("cortex_artifact_read", {{"path", (outside / "secret.bin").u8string()}}, roots)["error"] ==
          "path_outside_artifact_roots");
    CHECK(call("cortex_artifact_read", {{"path", ""}}, roots)["error"] == "invalid_path");
    std::error_code linkError;
    fs::create_symlink(outside / "secret.bin", root / "link.bin", linkError);
    if (!linkError)
        CHECK(call("cortex_artifact_read", {{"path", (root / "link.bin").u8string()}}, roots)["error"] ==
              "path_outside_artifact_roots");
    else
        std::cout << "note: symlinks unavailable here, link escape not exercised" << std::endl;

    // Reads are bounded and encoded.
    result = call("cortex_artifact_read", {{"path", (root / "large.bin").u8string()}, {"length", 1000000}}, roots);
    CHECK(result["ok"] == true && result["length"] == tools::kMaxReadBytes && result["eof"] == false);
    CHECK(result["hex"].get<std::string>().size() == tools::kMaxReadBytes * 2);
    result = call("cortex_artifact_read",
                  {{"path", (root / "large.bin").u8string()}, {"offset", 199990}, {"length", 100}, {"encoding", "utf8"}}, roots);
    CHECK(result["length"] == 10 && result["eof"] == true && result["text"] == std::string(10, 'x'));
    CHECK(call("cortex_artifact_read", {{"path", (root / "large.bin").u8string()}, {"offset", 300000}}, roots)["error"] ==
          "offset_beyond_end");

    // Container entries, nested with '!'.
    result = call("cortex_artifact_read", {{"path", asarPath}, {"child", "main.js"}, {"encoding", "utf8"}}, roots);
    CHECK(result["ok"] == true && result["text"] == mainJs && result["name"].get<std::string>().find("!main.js") != std::string::npos);
    result = call("cortex_artifact_read", {{"path", asarPath}, {"child", "vendor.asar!index.js"}, {"encoding", "utf8"}}, roots);
    CHECK(result["ok"] == true && result["text"] == "exports");
    result = call("cortex_artifact_inspect", {{"path", asarPath}, {"child", "vendor.asar"}}, roots);
    CHECK(result["ok"] == true && result["format"] == "asar" && result["data"]["files"] == 1);
    CHECK(call("cortex_artifact_read", {{"path", asarPath}, {"child", "missing.js"}}, roots)["error"] == "child_not_found");
    CHECK(call("cortex_artifact_read", {{"path", asarPath}, {"child", "a!b!c!d!e!f!g!h!i"}}, roots)["error"] ==
          "child_not_found");  // the first missing link stops the chain

    // A PE inside the root, when the fixture is given.
    if (argc > 1) {
        fs::copy_file(fs::u8path(argv[1]), root / "target.exe", fs::copy_options::overwrite_existing);
        result = call("cortex_artifact_inspect", {{"path", (root / "target.exe").u8string()}, {"max_entries", 5}}, roots);
        CHECK(result["ok"] == true && result["provider"]["name"] == "pe" && result["data"]["exports_truncated"] == true);
        result = call("cortex_artifact_read", {{"path", (root / "target.exe").u8string()}, {"length", 2}}, roots);
        CHECK(result["hex"] == "4d5a");
    }

    // The definitions agents see: read-only and path-required.
    const json definitions = tools::ToolDefinitions();
    CHECK(definitions.size() == 3);
    for (const auto& tool : definitions) {
        CHECK(tools::IsTool(tool["name"]));
        CHECK(tool["_cortex"]["read_only"] == true);
        CHECK(tool["inputSchema"]["required"] == json::array({"path"}));
        CHECK(tool["inputSchema"]["additionalProperties"] == false);
    }
    CHECK(!tools::IsTool("cortex_targets"));

    std::error_code ignored;
    fs::remove_all(base, ignored);
    if (failures) {
        std::cerr << failures << " artifact tool check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "artifact tool tests passed" << std::endl;
    return 0;
}
