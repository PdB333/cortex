#include "auto_assembler.h"

#include "assembler.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <sstream>

namespace cortex::services {
namespace {

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return text;
}

std::string Trim(const std::string& text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

bool Fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}

// Removes // line comments and { } block comments, keeping line breaks so
// error messages can name the right line.
std::string StripComments(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    int block = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (block > 0) {
            if (ch == '}') --block;
            else if (ch == '{') ++block;
            if (ch == '\n') out += '\n';
            continue;
        }
        if (ch == '{') {
            ++block;
            continue;
        }
        if (ch == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            while (i < text.size() && text[i] != '\n') ++i;
            if (i < text.size()) out += '\n';
            continue;
        }
        out += ch;
    }
    return out;
}

std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        const auto newline = text.find('\n', start);
        const auto end = newline == std::string::npos ? text.size() : newline;
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
        if (newline == std::string::npos) break;
        start = newline + 1;
    }
    return lines;
}

// Splits directive arguments on top-level commas.
std::vector<std::string> SplitArguments(const std::string& text) {
    std::vector<std::string> parts;
    std::string current;
    int depth = 0;
    for (const char ch : text) {
        if (ch == '(' || ch == '[') ++depth;
        if (ch == ')' || ch == ']') --depth;
        if (ch == ',' && depth == 0) {
            parts.push_back(Trim(current));
            current.clear();
            continue;
        }
        current += ch;
    }
    const std::string last = Trim(current);
    if (!last.empty() || !parts.empty()) parts.push_back(last);
    return parts;
}

bool IsIdentifier(const std::string& text) {
    if (text.empty()) return false;
    if (!std::isalpha(static_cast<unsigned char>(text[0])) && text[0] != '_') return false;
    for (const char ch : text)
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_' && ch != '.') return false;
    return true;
}

struct Directive {
    std::string name;
    std::vector<std::string> arguments;
    int line = 0;
};

struct Block {
    std::string header;                              // text before the ':'
    std::vector<std::pair<std::string, int>> lines;  // code line, source line
    int line = 0;
};

struct Item {
    bool isDirective = false;
    Directive directive;
    Block block;
};

// A contiguous run of bytes to write at one address.
struct Region {
    uint64_t base = 0;
    std::vector<uint8_t> bytes;
};

const char* const kDirectives[] = {
    "alloc", "globalalloc", "dealloc", "label", "define", "registersymbol", "unregistersymbol",
    "aobscan", "aobscanmodule", "aobscanregion", "assert", "fullaccess", "createthread",
    "createthreadandwait", "loadlibrary", "luacall", "include", "unregistersymbols"};

bool IsDirectiveName(const std::string& name) {
    return std::find(std::begin(kDirectives), std::end(kDirectives), Lower(name)) != std::end(kDirectives);
}

// Parses the lines of one section into directives and code blocks.
bool ParseItems(const std::vector<std::string>& lines, int firstLine, std::vector<Item>& items,
                std::string* error) {
    Item* current = nullptr;
    for (size_t i = 0; i < lines.size(); ++i) {
        const int number = firstLine + static_cast<int>(i);
        std::string line = Trim(lines[i]);
        if (line.empty()) continue;

        // directive(arguments)
        const auto open = line.find('(');
        if (open != std::string::npos && line.back() == ')') {
            const std::string name = Trim(line.substr(0, open));
            if (IsDirectiveName(name)) {
                Item item;
                item.isDirective = true;
                item.directive.name = Lower(name);
                item.directive.arguments = SplitArguments(line.substr(open + 1, line.size() - open - 2));
                item.directive.line = number;
                items.push_back(std::move(item));
                current = nullptr;
                continue;
            }
        }

        // A block header: "name:" or "game.exe+1234:" alone on the line.
        // A colon that belongs to a segment override (fs:[..]) or to code
        // after the header is left to the assembler.
        const auto colon = line.find(':');
        if (colon != std::string::npos && colon > 0) {
            const std::string head = Trim(line.substr(0, colon));
            const std::string tail = Trim(line.substr(colon + 1));
            const bool looksLikeHeader =
                !head.empty() && head.find_first_of(" \t[]") == std::string::npos &&
                (std::isalnum(static_cast<unsigned char>(head[0])) || head[0] == '_') &&
                head.find(',') == std::string::npos;
            if (looksLikeHeader) {
                Item item;
                item.block.header = head;
                item.block.line = number;
                items.push_back(std::move(item));
                current = &items.back();
                if (!tail.empty()) current->block.lines.emplace_back(tail, number);
                continue;
            }
        }

        if (!current) {
            return Fail(error, "line " + std::to_string(number) + ": code outside a block: " + line);
        }
        current->block.lines.emplace_back(line, number);
    }
    return true;
}

class Interpreter {
public:
    Interpreter(const AutoAssembleHost& host, const AutoAssembleOptions& options, AutoAssembleResult& result)
        : host_(host), options_(options), result_(result) {}

    bool Run(const std::vector<Item>& items, std::string* error) {
        // Directives that define names run first, as Cheat Engine does:
        // the code below them may use what they declare.
        for (const auto& item : items) {
            if (!item.isDirective) continue;
            if (!RunEarlyDirective(item.directive, error)) return false;
        }
        if (!Assemble(items, error)) return false;
        if (!WriteRegions(error)) return false;
        for (const auto& item : items) {
            if (!item.isDirective) continue;
            if (!RunLateDirective(item.directive, error)) return false;
        }
        return true;
    }

private:
    // A name or a sum of names and numbers: INJECT, game.exe+1234, newmem+8.
    bool Resolve(const std::string& text, uint64_t& value, std::string* error) {
        const std::string trimmed = Trim(text);
        if (trimmed.empty()) return Fail(error, "empty address");
        uint64_t total = 0;
        int sign = 1;
        size_t position = 0;
        if (trimmed[0] == '-') {
            sign = -1;
            position = 1;
        }
        while (position < trimmed.size()) {
            size_t next = position;
            while (next < trimmed.size() && trimmed[next] != '+' && trimmed[next] != '-') ++next;
            const std::string term = Trim(trimmed.substr(position, next - position));
            if (term.empty()) return Fail(error, "malformed address: " + trimmed);
            uint64_t part = 0;
            if (!ResolveTerm(term, part, error)) return false;
            total += sign > 0 ? part : 0 - part;
            if (next >= trimmed.size()) break;
            sign = trimmed[next] == '-' ? -1 : 1;
            position = next + 1;
        }
        value = total;
        return true;
    }

    bool ResolveTerm(const std::string& term, uint64_t& value, std::string* error) {
        if (term.empty()) return Fail(error, "empty term");
        const std::string key = Lower(term);
        const auto label = labels_.find(key);
        if (label != labels_.end()) {
            value = label->second;
            return true;
        }
        const auto symbol = symbols_.find(key);
        if (symbol != symbols_.end()) {
            value = symbol->second;
            return true;
        }
        if (ParseAsmNumber(term, value)) return true;
        if (host_.symbol && host_.symbol(term, value)) return true;
        return Fail(error, "unknown symbol: " + term);
    }

    // Replaces define() names in a code line, on word boundaries.
    std::string ApplyDefines(std::string line) const {
        for (const auto& define : defines_) {
            size_t position = 0;
            while ((position = Lower(line).find(define.first, position)) != std::string::npos) {
                const bool beforeOk = position == 0 || !std::isalnum(static_cast<unsigned char>(line[position - 1]));
                const size_t end = position + define.first.size();
                const bool afterOk = end >= line.size() || !std::isalnum(static_cast<unsigned char>(line[end]));
                if (beforeOk && afterOk) {
                    line = line.substr(0, position) + define.second + line.substr(end);
                    position += define.second.size();
                } else {
                    position = end;
                }
            }
        }
        return line;
    }

    bool RunEarlyDirective(const Directive& directive, std::string* error) {
        const auto& name = directive.name;
        const auto& args = directive.arguments;
        auto need = [&](size_t count) {
            if (args.size() >= count) return true;
            Fail(error, "line " + std::to_string(directive.line) + ": " + name + " needs " +
                            std::to_string(count) + " argument(s)");
            return false;
        };

        if (name == "define") {
            if (!need(2)) return false;
            defines_.emplace_back(Lower(args[0]), args[1]);
            return true;
        }
        if (name == "label") {
            if (!need(1)) return false;
            declared_.insert(Lower(args[0]));
            return true;
        }
        if (name == "alloc" || name == "globalalloc") {
            if (!need(2)) return false;
            uint64_t size = 0;
            if (!ParseAsmNumber(args[1], size) || size == 0)
                return Fail(error, "line " + std::to_string(directive.line) + ": invalid size " + args[1]);
            uint64_t nearAddress = 0;
            if (args.size() >= 3 && !Resolve(args[2], nearAddress, error)) return false;
            uint64_t address = 0;
            if (options_.dryRun) {
                // A plausible address so the rest of the script assembles.
                address = 0x10000000 + 0x10000 * static_cast<uint64_t>(result_.allocations.size() + 1);
            } else {
                std::string message;
                if (!host_.allocate || !host_.allocate(static_cast<size_t>(size), nearAddress, address, message))
                    return Fail(error, "line " + std::to_string(directive.line) + ": alloc failed: " + message);
            }
            symbols_[Lower(args[0])] = address;
            result_.allocations.emplace_back(args[0], address);
            result_.log.push_back("alloc " + args[0] + " = " + Hex(address));
            if (name == "globalalloc") result_.registered.emplace_back(args[0], address);
            return true;
        }
        if (name == "aobscan" || name == "aobscanmodule" || name == "aobscanregion") {
            return RunScan(directive, error);
        }
        if (name == "assert") {
            if (!need(2)) return false;
            uint64_t address = 0;
            if (!Resolve(args[0], address, error)) return false;
            std::vector<uint8_t> expected;
            std::vector<uint8_t> mask;
            if (!ParsePattern(args[1], expected, mask))
                return Fail(error, "line " + std::to_string(directive.line) + ": invalid byte pattern");
            if (options_.dryRun) return true;
            std::vector<uint8_t> actual(expected.size());
            if (!host_.read || !host_.read(address, actual.data(), actual.size()))
                return Fail(error, "line " + std::to_string(directive.line) + ": assert cannot read " + Hex(address));
            for (size_t i = 0; i < expected.size(); ++i) {
                if (mask[i] && actual[i] != expected[i])
                    return Fail(error, "line " + std::to_string(directive.line) + ": assert failed at " + Hex(address));
            }
            return true;
        }
        if (name == "fullaccess") {
            if (!need(2)) return false;
            uint64_t address = 0;
            uint64_t size = 0;
            if (!Resolve(args[0], address, error)) return false;
            if (!ParseAsmNumber(args[1], size)) return Fail(error, "invalid size " + args[1]);
            std::string message;
            if (!options_.dryRun && host_.fullAccess && !host_.fullAccess(address, static_cast<size_t>(size), message))
                result_.log.push_back("fullaccess failed: " + message);
            return true;
        }
        // include / loadlibrary / luacall are accepted and reported.
        if (name == "include" || name == "loadlibrary" || name == "luacall") {
            result_.log.push_back(name + " is not supported and was skipped (line " +
                                  std::to_string(directive.line) + ")");
            return true;
        }
        return true;  // late directives run after the code is written
    }

    bool RunScan(const Directive& directive, std::string* error) {
        const auto& args = directive.arguments;
        const std::string where = "line " + std::to_string(directive.line) + ": " + directive.name;
        uint64_t start = 0;
        uint64_t stop = ~0ull;
        std::string pattern;
        if (directive.name == "aobscan") {
            if (args.size() < 2) return Fail(error, where + " needs a name and a pattern");
            pattern = args[1];
        } else if (directive.name == "aobscanmodule") {
            if (args.size() < 3) return Fail(error, where + " needs a name, a module and a pattern");
            uint64_t base = 0;
            uint64_t size = 0;
            if (!host_.moduleRange || !host_.moduleRange(args[1], base, size))
                return Fail(error, where + ": module " + args[1] + " is not loaded");
            start = base;
            stop = base + size;
            pattern = args[2];
        } else {
            if (args.size() < 4) return Fail(error, where + " needs a name, a range and a pattern");
            if (!Resolve(args[1], start, error) || !Resolve(args[2], stop, error)) return false;
            pattern = args[3];
        }
        if (options_.dryRun) {
            symbols_[Lower(args[0])] = start ? start : 0x140001000ull;
            return true;
        }
        std::vector<uint64_t> matches;
        std::string message;
        if (!host_.scan || !host_.scan(pattern, start, stop, matches, message))
            return Fail(error, where + " failed: " + message);
        if (matches.empty()) return Fail(error, where + ": pattern not found");
        symbols_[Lower(args[0])] = matches.front();
        result_.log.push_back(directive.name + " " + args[0] + " = " + Hex(matches.front()) +
                              (matches.size() > 1 ? " (" + std::to_string(matches.size()) + " matches)" : ""));
        return true;
    }

    bool RunLateDirective(const Directive& directive, std::string* error) {
        const auto& name = directive.name;
        const auto& args = directive.arguments;
        if (name == "registersymbol") {
            if (args.empty()) return Fail(error, "registersymbol needs a name");
            uint64_t value = 0;
            if (!Resolve(args[0], value, error)) return false;
            result_.registered.emplace_back(args[0], value);
            return true;
        }
        if (name == "unregistersymbol" || name == "unregistersymbols") {
            for (const auto& argument : args)
                if (!argument.empty()) result_.unregistered.push_back(argument);
            return true;
        }
        if (name == "dealloc") {
            if (args.empty()) return Fail(error, "dealloc needs a name");
            uint64_t address = 0;
            if (!Resolve(args[0], address, nullptr)) {
                // Nothing to free: the allocation is already gone.
                result_.log.push_back("dealloc " + args[0] + ": unknown, skipped");
                return true;
            }
            std::string message;
            if (!options_.dryRun && host_.release && !host_.release(address, message))
                result_.log.push_back("dealloc " + args[0] + " failed: " + message);
            result_.freed.push_back(args[0]);
            return true;
        }
        if (name == "createthread" || name == "createthreadandwait") {
            if (args.empty()) return Fail(error, "createthread needs an address");
            uint64_t address = 0;
            if (!Resolve(args[0], address, error)) return false;
            std::string message;
            if (!options_.dryRun && host_.createThread && !host_.createThread(address, message))
                return Fail(error, "createthread failed: " + message);
            result_.log.push_back("createthread " + Hex(address));
            return true;
        }
        return true;
    }

    // Lays the blocks out into regions, assembling until the labels settle.
    bool Assemble(const std::vector<Item>& items, std::string* error) {
        for (int pass = 0; pass < 12; ++pass) {
            const auto previous = labels_;
            regions_.clear();
            bool failed = false;
            std::string message;
            if (!Layout(items, false, &message, failed)) continue;  // labels not settled yet
            if (!failed && pass > 0 && labels_ == previous) return true;
        }
        // One last pass that reports the first real error.
        regions_.clear();
        bool failed = false;
        return Layout(items, true, error, failed);
    }

    bool Layout(const std::vector<Item>& items, bool report, std::string* error, bool& failed) {
        failed = false;
        Region* region = nullptr;
        for (const auto& item : items) {
            if (item.isDirective) continue;
            const auto& block = item.block;
            const std::string key = Lower(block.header);
            uint64_t base = 0;
            const bool declaredLabel = declared_.count(key) != 0 && symbols_.count(key) == 0;
            if (declaredLabel && region) {
                // A label inside the current region: it marks a position.
                labels_[key] = region->base + region->bytes.size();
            } else {
                std::string message;
                if (!Resolve(block.header, base, &message))
                    return Fail(error, "line " + std::to_string(block.line) + ": " + message);
                regions_.push_back(Region{base, {}});
                region = &regions_.back();
                labels_[key] = base;
            }
            for (const auto& line : block.lines) {
                AssembleRequest request;
                request.text = ApplyDefines(line.first);
                request.address = region->base + region->bytes.size();
                request.x64 = options_.x64;
                request.evaluate = [this](const std::string& text, uint64_t& value, std::string& message) {
                    std::string local;
                    if (Resolve(text, value, &local)) return true;
                    message = local;
                    return false;
                };
                std::vector<uint8_t> bytes;
                std::string message;
                if (!AssembleLine(request, bytes, &message)) {
                    if (report) return Fail(error, "line " + std::to_string(line.second) + ": " + message);
                    // A label defined further down: keep a plausible length
                    // and let the next pass settle it.
                    failed = true;
                    bytes.assign(5, 0x90);
                }
                region->bytes.insert(region->bytes.end(), bytes.begin(), bytes.end());
            }
        }
        return true;
    }

    bool WriteRegions(std::string* error) {
        for (const auto& region : regions_) {
            if (region.bytes.empty()) continue;
            AutoAssemblePatch patch;
            patch.address = region.base;
            patch.written = region.bytes;
            patch.original.assign(region.bytes.size(), 0);
            if (!options_.dryRun) {
                if (host_.read) host_.read(region.base, patch.original.data(), patch.original.size());
                if (!host_.write || !host_.write(region.base, region.bytes.data(), region.bytes.size()))
                    return Fail(error, "cannot write " + std::to_string(region.bytes.size()) + " byte(s) at " +
                                           Hex(region.base));
            }
            result_.patches.push_back(std::move(patch));
        }
        return true;
    }

    static std::string Hex(uint64_t value) {
        char buffer[32] = {};
        std::snprintf(buffer, sizeof(buffer), "%llX", static_cast<unsigned long long>(value));
        return buffer;
    }

    static bool ParsePattern(const std::string& text, std::vector<uint8_t>& bytes, std::vector<uint8_t>& mask) {
        bytes.clear();
        mask.clear();
        std::istringstream stream(text);
        std::string token;
        while (stream >> token) {
            if (token == "??" || token == "?" || token == "*") {
                bytes.push_back(0);
                mask.push_back(0);
                continue;
            }
            if (token.size() != 2 || !std::isxdigit(static_cast<unsigned char>(token[0])) ||
                !std::isxdigit(static_cast<unsigned char>(token[1])))
                return false;
            bytes.push_back(static_cast<uint8_t>(std::strtoul(token.c_str(), nullptr, 16)));
            mask.push_back(0xFF);
        }
        return !bytes.empty();
    }

    const AutoAssembleHost& host_;
    const AutoAssembleOptions& options_;
    AutoAssembleResult& result_;
    std::map<std::string, uint64_t> symbols_;   // alloc, aobscan
    std::map<std::string, uint64_t> labels_;    // block headers and label()
    std::set<std::string> declared_;            // label()
    std::vector<std::pair<std::string, std::string>> defines_;
    std::vector<Region> regions_;
};

} // namespace

bool SplitAutoAssemblerSections(const std::string& script, std::string& enable, std::string& disable) {
    enable.clear();
    disable.clear();
    const auto lines = SplitLines(StripComments(script));
    int section = 0;  // 0 = shared, 1 = enable, 2 = disable
    bool found = false;
    std::string shared;
    for (const auto& line : lines) {
        const std::string trimmed = Lower(Trim(line));
        if (trimmed == "[enable]") {
            section = 1;
            found = true;
            continue;
        }
        if (trimmed == "[disable]") {
            section = 2;
            found = true;
            continue;
        }
        if (section == 1) enable += line + "\n";
        else if (section == 2) disable += line + "\n";
        else shared += line + "\n";
    }
    if (!found) {
        enable = shared;
        return false;
    }
    enable = shared + enable;
    disable = shared + disable;
    return true;
}

bool LooksLikeAutoAssembler(const std::string& text) {
    const std::string lower = Lower(text);
    if (lower.find("[enable]") != std::string::npos || lower.find("[disable]") != std::string::npos) return true;
    for (const char* directive : kDirectives) {
        const std::string needle = std::string(directive) + "(";
        if (lower.find(needle) != std::string::npos) return true;
    }
    return false;
}

bool RunAutoAssembler(const std::string& script, const AutoAssembleHost& host,
                      const AutoAssembleOptions& options, AutoAssembleResult& result, std::string* error) {
    result = AutoAssembleResult{};
    std::string enable;
    std::string disable;
    SplitAutoAssemblerSections(script, enable, disable);
    const std::string& section = options.enable ? enable : disable;
    if (Trim(section).empty()) {
        if (options.enable) return Fail(error, "the script has no [ENABLE] section");
        return true;  // nothing to undo
    }

    std::vector<Item> items;
    if (!ParseItems(SplitLines(section), 1, items, error)) return false;

    Interpreter interpreter(host, options, result);
    return interpreter.Run(items, error);
}

} // namespace cortex::services
