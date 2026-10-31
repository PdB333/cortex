#pragma once

// Source maps (revision 3): decoding of the Base64 VLQ `mappings` and the
// lookup of a generated position, with the semantics of Node's
// node:module SourceMap.findEntry (the greatest mapping at or before the
// position). Index maps (`sections`) are recognised and reported as
// unsupported rather than guessed.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cortex::artifacts {

struct SourceMapping {
    uint32_t generatedLine = 0;    // 0-based, as in the format
    uint32_t generatedColumn = 0;  // 0-based UTF-16 units
    int32_t source = -1;           // -1: a segment without source
    uint32_t originalLine = 0;
    uint32_t originalColumn = 0;
    int32_t name = -1;
};

struct SourceMap {
    int version = 0;
    std::string file;
    std::string sourceRoot;
    std::vector<std::string> sources;
    std::vector<std::string> names;
    std::vector<nlohmann::json> sourcesContent;  // null or string, per source
    std::vector<SourceMapping> mappings;         // ordered by generated position
    std::vector<std::string> warnings;
    bool truncated = false;
};

// False with `error` set if the text is not a usable revision-3 map.
bool ParseSourceMap(std::string_view text, SourceMap& map, std::string& error,
                    size_t maxMappings = 8u << 20);

// Greatest mapping at or before the 0-based position, or none.
std::optional<SourceMapping> LookupMapping(const SourceMap& map, uint32_t line, uint32_t column);

// {"generated": {line, column}, "original": {source, line, column, name,
// line_text?} | null}; lines and columns 1-based. `line_text` is the
// original line from sourcesContent, cut to 400 bytes.
nlohmann::json LookupJson(const SourceMap& map, uint32_t line1, uint32_t column1);

// Version, file, sources (with whether content is embedded), counts.
nlohmann::json SourceMapSummary(const SourceMap& map, size_t maxEntries);

// RFC 4648 Base64 (also accepts the URL-safe alphabet and missing padding).
bool DecodeBase64(std::string_view text, std::string& out);

// The map text of a `data:application/json;base64,` (or percent-free
// plain `data:application/json,`) URL; false if `url` is not one.
bool DecodeDataUrl(std::string_view url, std::string& text);

} // namespace cortex::artifacts
