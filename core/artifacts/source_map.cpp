#include "source_map.h"

#include <algorithm>

namespace cortex::artifacts {
namespace {

using json = nlohmann::json;

int Base64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

// One Base64 VLQ value at `i`; false on a bad digit, a truncated value or
// a value outside 32 bits.
bool ReadVlq(std::string_view text, size_t& i, int64_t& value) {
    uint64_t result = 0;
    int shift = 0;
    while (true) {
        if (i >= text.size()) return false;
        const int digit = Base64Value(text[i++]);
        if (digit < 0 || text[i - 1] == '-' || text[i - 1] == '_') return false;
        result |= static_cast<uint64_t>(digit & 31) << shift;
        if (!(digit & 32)) break;
        shift += 5;
        if (shift > 30) return false;
    }
    const bool negative = result & 1;
    result >>= 1;
    if (result > 0x7FFFFFFF) return false;
    value = negative ? -static_cast<int64_t>(result) : static_cast<int64_t>(result);
    return true;
}

bool InRange(int64_t value) { return value >= 0 && value <= 0x7FFFFFFF; }

} // namespace

bool DecodeBase64(std::string_view text, std::string& out) {
    out.clear();
    out.reserve(text.size() / 4 * 3);
    uint32_t buffer = 0;
    int bits = 0;
    for (const char c : text) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        const int value = Base64Value(c);
        if (value < 0) return false;
        buffer = (buffer << 6) | static_cast<uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
        }
    }
    return true;
}

bool DecodeDataUrl(std::string_view url, std::string& text) {
    if (url.substr(0, 5) != "data:") return false;
    const size_t comma = url.find(',');
    if (comma == std::string_view::npos) return false;
    const std::string_view meta = url.substr(5, comma - 5);
    if (meta.find("json") == std::string_view::npos) return false;
    const std::string_view payload = url.substr(comma + 1);
    if (meta.size() >= 7 && meta.substr(meta.size() - 7) == ";base64") return DecodeBase64(payload, text);
    if (payload.find('%') != std::string_view::npos) return false;  // percent-encoding not decoded
    text = std::string(payload);
    return true;
}

bool ParseSourceMap(std::string_view text, SourceMap& map, std::string& error, size_t maxMappings) {
    map = SourceMap();
    // A map may start with the XSSI guard `)]}'`.
    if (text.substr(0, 4) == ")]}'") {
        const size_t newline = text.find('\n');
        text = newline == std::string_view::npos ? std::string_view() : text.substr(newline + 1);
    }
    const json root = json::parse(text.begin(), text.end(), nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        error = "sourcemap_not_json";
        return false;
    }
    if (!root.contains("version") || !root["version"].is_number_integer() || root["version"] != 3) {
        error = "sourcemap_version_unsupported";
        return false;
    }
    map.version = 3;
    if (root.contains("sections")) {
        error = "sourcemap_index_map_unsupported";
        return false;
    }
    if (!root.contains("mappings") || !root["mappings"].is_string()) {
        error = "sourcemap_mappings_missing";
        return false;
    }
    map.file = root.value("file", json("")).is_string() ? root.value("file", std::string()) : std::string();
    map.sourceRoot = root.value("sourceRoot", json("")).is_string() ? root.value("sourceRoot", std::string()) : std::string();
    if (root.contains("sources") && root["sources"].is_array())
        for (const auto& source : root["sources"]) map.sources.push_back(source.is_string() ? source.get<std::string>() : "");
    if (root.contains("names") && root["names"].is_array())
        for (const auto& name : root["names"]) map.names.push_back(name.is_string() ? name.get<std::string>() : "");
    map.sourcesContent.assign(map.sources.size(), nullptr);
    if (root.contains("sourcesContent") && root["sourcesContent"].is_array())
        for (size_t i = 0; i < map.sources.size() && i < root["sourcesContent"].size(); ++i)
            if (root["sourcesContent"][i].is_string()) map.sourcesContent[i] = root["sourcesContent"][i];

    const std::string mappings = root["mappings"].get<std::string>();
    int64_t line = 0, column = 0, source = 0, originalLine = 0, originalColumn = 0, name = 0;
    size_t i = 0;
    while (i < mappings.size()) {
        const char c = mappings[i];
        if (c == ';') {
            ++line;
            column = 0;
            ++i;
            continue;
        }
        if (c == ',') {
            ++i;
            continue;
        }
        if (map.mappings.size() >= maxMappings) {
            map.truncated = true;
            break;
        }
        int64_t delta = 0;
        if (!ReadVlq(mappings, i, delta)) {
            error = "sourcemap_vlq_invalid at mapping byte " + std::to_string(i);
            return false;
        }
        column += delta;
        SourceMapping mapping;
        if (!InRange(column) || line > 0x7FFFFFFF) {
            error = "sourcemap_position_out_of_range";
            return false;
        }
        mapping.generatedLine = static_cast<uint32_t>(line);
        mapping.generatedColumn = static_cast<uint32_t>(column);
        if (i < mappings.size() && mappings[i] != ',' && mappings[i] != ';') {
            int64_t d1 = 0, d2 = 0, d3 = 0;
            if (!ReadVlq(mappings, i, d1) || !ReadVlq(mappings, i, d2) || !ReadVlq(mappings, i, d3)) {
                error = "sourcemap_vlq_invalid at mapping byte " + std::to_string(i);
                return false;
            }
            source += d1;
            originalLine += d2;
            originalColumn += d3;
            if (!InRange(source) || !InRange(originalLine) || !InRange(originalColumn)) {
                error = "sourcemap_position_out_of_range";
                return false;
            }
            if (static_cast<size_t>(source) >= map.sources.size()) {
                if (map.warnings.size() < 8) map.warnings.push_back("mapping refers to source " + std::to_string(source) + " which is not listed");
            } else {
                mapping.source = static_cast<int32_t>(source);
            }
            mapping.originalLine = static_cast<uint32_t>(originalLine);
            mapping.originalColumn = static_cast<uint32_t>(originalColumn);
            if (i < mappings.size() && mappings[i] != ',' && mappings[i] != ';') {
                int64_t d4 = 0;
                if (!ReadVlq(mappings, i, d4)) {
                    error = "sourcemap_vlq_invalid at mapping byte " + std::to_string(i);
                    return false;
                }
                name += d4;
                if (!InRange(name)) {
                    error = "sourcemap_position_out_of_range";
                    return false;
                }
                if (static_cast<size_t>(name) < map.names.size()) mapping.name = static_cast<int32_t>(name);
            }
            if (i < mappings.size() && mappings[i] != ',' && mappings[i] != ';') {
                error = "sourcemap_segment_too_long at mapping byte " + std::to_string(i);
                return false;
            }
        }
        map.mappings.push_back(mapping);
    }
    // Segments are ordered by line by construction; columns within a line
    // are usually ordered too, but the format does not require it.
    std::stable_sort(map.mappings.begin(), map.mappings.end(), [](const SourceMapping& a, const SourceMapping& b) {
        return a.generatedLine != b.generatedLine ? a.generatedLine < b.generatedLine : a.generatedColumn < b.generatedColumn;
    });
    return true;
}

std::optional<SourceMapping> LookupMapping(const SourceMap& map, uint32_t line, uint32_t column) {
    const auto after = std::upper_bound(map.mappings.begin(), map.mappings.end(), std::make_pair(line, column),
                                        [](const std::pair<uint32_t, uint32_t>& position, const SourceMapping& m) {
                                            return position.first != m.generatedLine ? position.first < m.generatedLine
                                                                                     : position.second < m.generatedColumn;
                                        });
    if (after == map.mappings.begin()) return std::nullopt;
    return *(after - 1);
}

json LookupJson(const SourceMap& map, uint32_t line1, uint32_t column1) {
    json result = {{"generated", {{"line", line1}, {"column", column1}}}, {"original", nullptr}};
    if (line1 == 0 || column1 == 0) return result;
    const auto mapping = LookupMapping(map, line1 - 1, column1 - 1);
    if (!mapping || mapping->source < 0) return result;
    json original = {{"source", map.sources[static_cast<size_t>(mapping->source)]},
                     {"line", mapping->originalLine + 1},
                     {"column", mapping->originalColumn + 1},
                     {"name", mapping->name >= 0 ? json(map.names[static_cast<size_t>(mapping->name)]) : json(nullptr)},
                     {"mapping_generated", {{"line", mapping->generatedLine + 1}, {"column", mapping->generatedColumn + 1}}}};
    const json& content = map.sourcesContent[static_cast<size_t>(mapping->source)];
    if (content.is_string()) {
        const std::string& text = content.get_ref<const std::string&>();
        size_t start = 0;
        for (uint32_t l = 0; l < mapping->originalLine && start != std::string::npos; ++l) {
            start = text.find('\n', start);
            if (start != std::string::npos) ++start;
        }
        if (start != std::string::npos && start <= text.size()) {
            size_t end = text.find('\n', start);
            if (end == std::string::npos) end = text.size();
            std::string lineText = text.substr(start, std::min<size_t>(end - start, 400));
            if (!lineText.empty() && lineText.back() == '\r') lineText.pop_back();
            original["line_text"] = lineText;
        }
    }
    result["original"] = std::move(original);
    return result;
}

json SourceMapSummary(const SourceMap& map, size_t maxEntries) {
    json sources = json::array();
    size_t embedded = 0;
    for (size_t i = 0; i < map.sources.size(); ++i) {
        const bool hasContent = map.sourcesContent[i].is_string();
        if (hasContent) ++embedded;
        if (sources.size() < maxEntries) sources.push_back({{"source", map.sources[i]}, {"content_embedded", hasContent}});
    }
    uint32_t lines = map.mappings.empty() ? 0 : map.mappings.back().generatedLine + 1;
    size_t withSource = 0;
    for (const auto& mapping : map.mappings)
        if (mapping.source >= 0) ++withSource;
    json summary = {{"format", "sourcemap"},
                    {"version", map.version},
                    {"file", map.file},
                    {"source_root", map.sourceRoot},
                    {"sources", sources},
                    {"sources_count", map.sources.size()},
                    {"sources_truncated", map.sources.size() > maxEntries},
                    {"sources_with_content", embedded},
                    {"names_count", map.names.size()},
                    {"mappings", map.mappings.size()},
                    {"mappings_with_source", withSource},
                    {"generated_lines", lines},
                    {"mappings_truncated", map.truncated}};
    if (!map.warnings.empty()) summary["warnings"] = map.warnings;
    return summary;
}

} // namespace cortex::artifacts
