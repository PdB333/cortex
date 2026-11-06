// Whole-registry fuzzer: detection, inspection and container children (ZIP,
// ASAR, nested entries) exactly as `cortex.exe inspect` and the
// cortex_artifact_* tools run them. The first byte picks the file name, since
// some providers (JavaScript, source maps) also look at the extension.
#include "artifacts/artifact.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace cortex::artifacts;

namespace {
const char* const kNames[] = {"input.bin", "input.dll", "input.exe", "app.asar", "bundle.js",
                              "bundle.js.map", "module.mjs", "archive.zip", "package.jar", "code.ts"};

void Walk(const Registry& registry, ByteSourcePtr source, int depth) {
    InspectOptions options;
    options.maxEntries = 64;
    registry.Inspect(*source, options);
    if (depth >= 2) return;
    json children = registry.ListChildren(*source, options);
    if (!children.is_object() || !children.contains("children") || !children["children"].is_array()) return;
    size_t opened = 0;
    for (const auto& child : children["children"]) {
        if (opened++ >= 6) break;
        if (!child.is_object() || !child.contains("name") || !child["name"].is_string()) continue;
        std::string error;
        ByteSourcePtr inner = registry.OpenChild(source, child["name"].get<std::string>(), error, options);
        if (inner) Walk(registry, inner, depth + 1);
    }
}
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) return 0;
    const std::string name = kNames[data[0] % (sizeof(kNames) / sizeof(kNames[0]))];
    std::vector<uint8_t> bytes(data + 1, data + size);
    Walk(DefaultRegistry(), FromBytes(std::move(bytes), name), 0);
    return 0;
}
