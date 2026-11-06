#pragma once

// JavaScript/TypeScript files and source maps as artifacts (step 4b), and
// the static view of an Electron app inside an ASAR.

#include "artifact.h"

#include <functional>

namespace cortex::artifacts {

class JavaScriptProvider final : public Provider {
public:
    const char* Name() const override { return "javascript"; }
    const char* Version() const override { return "1"; }
    std::vector<std::string> Formats() const override { return {"javascript", "typescript"}; }
    std::vector<Capability> Capabilities() const override;
    int Detect(const ByteSource& source) const override;
    json Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const override;
};

class SourceMapProvider final : public Provider {
public:
    const char* Name() const override { return "sourcemap"; }
    const char* Version() const override { return "1"; }
    std::vector<std::string> Formats() const override { return {"sourcemap"}; }
    std::vector<Capability> Capabilities() const override;
    int Detect(const ByteSource& source) const override;
    json Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const override;
};

// Opens the map a script names in its sourceMappingURL (a sibling file or
// container entry); null with `error` set when it cannot.
using MapOpener = std::function<ByteSourcePtr(const std::string& url, std::string& error)>;

// The original position of a 1-based generated line/column, from a source
// map artifact or from a script and its map (inline, or through `open`).
// {"map": {...}, "generated", "original"} or null with `error` set.
json SourcePosition(const ByteSource& artifact, uint32_t line, uint32_t column, const MapOpener& open,
                    std::string& error);

// The static module graph and Electron view of the scripts of a container
// (an ASAR). `children` are its entries. Bounded by the options and by
// fixed byte budgets; what was skipped is counted.
json AnalyzeScriptTree(const ByteSource& container, const std::vector<Child>& children, const json& packageJson,
                       const InspectOptions& options);

// Joins `relative` onto the directory `base` ("a/b" + "../c" = "a/c").
// False if the result would climb above the root.
bool JoinArchivePath(const std::string& base, const std::string& relative, std::string& out);

} // namespace cortex::artifacts
