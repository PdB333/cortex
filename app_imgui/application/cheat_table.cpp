#include "cheat_table.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace cortex::application {
namespace {

// A small XML reader for cheat tables: elements, text, CDATA, comments and
// the standard entities. Attributes are skipped; Cheat Engine keeps the
// data in child elements.
struct XmlNode {
    std::string name;
    std::string text;
    std::vector<XmlNode> children;

    const XmlNode* Child(const char* childName) const {
        for (const auto& child : children)
            if (child.name == childName) return &child;
        return nullptr;
    }
    std::string ChildText(const char* childName) const {
        const XmlNode* child = Child(childName);
        return child ? child->text : std::string();
    }
};

class XmlReader {
public:
    explicit XmlReader(const std::string& text) : s_(text) {}

    bool ParseDocument(XmlNode& root, std::string* error) {
        SkipMisc();
        if (!ParseElement(root, 0)) {
            if (error) *error = error_.empty() ? "Invalid XML" : error_;
            return false;
        }
        return true;
    }

private:
    bool StartsWith(const char* prefix) const { return s_.compare(pos_, std::strlen(prefix), prefix) == 0; }

    void SkipSpace() {
        while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) ++pos_;
    }

    // Declarations, comments and doctype between elements.
    void SkipMisc() {
        for (;;) {
            SkipSpace();
            if (StartsWith("<?")) {
                const auto end = s_.find("?>", pos_);
                pos_ = end == std::string::npos ? s_.size() : end + 2;
            } else if (StartsWith("<!--")) {
                const auto end = s_.find("-->", pos_);
                pos_ = end == std::string::npos ? s_.size() : end + 3;
            } else if (StartsWith("<!DOCTYPE")) {
                const auto end = s_.find('>', pos_);
                pos_ = end == std::string::npos ? s_.size() : end + 1;
            } else {
                return;
            }
        }
    }

    static std::string Decode(const std::string& raw) {
        std::string out;
        out.reserve(raw.size());
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] != '&') {
                out += raw[i];
                continue;
            }
            const auto end = raw.find(';', i);
            if (end == std::string::npos || end - i > 10) {
                out += raw[i];
                continue;
            }
            const std::string entity = raw.substr(i + 1, end - i - 1);
            if (entity == "lt") out += '<';
            else if (entity == "gt") out += '>';
            else if (entity == "amp") out += '&';
            else if (entity == "quot") out += '"';
            else if (entity == "apos") out += '\'';
            else if (!entity.empty() && entity[0] == '#') {
                const unsigned long code = entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X')
                    ? std::strtoul(entity.c_str() + 2, nullptr, 16) : std::strtoul(entity.c_str() + 1, nullptr, 10);
                if (code < 0x80) {
                    out += static_cast<char>(code);
                } else if (code < 0x800) {
                    out += static_cast<char>(0xC0 | (code >> 6));
                    out += static_cast<char>(0x80 | (code & 0x3F));
                } else {
                    out += static_cast<char>(0xE0 | ((code >> 12) & 0x0F));
                    out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (code & 0x3F));
                }
            } else {
                out += raw.substr(i, end - i + 1);
            }
            i = end;
        }
        return out;
    }

    bool ParseElement(XmlNode& node, int depth) {
        if (depth > 64) {
            error_ = "XML nested too deeply";
            return false;
        }
        if (pos_ >= s_.size() || s_[pos_] != '<') {
            error_ = "Expected an element";
            return false;
        }
        ++pos_;
        const size_t nameStart = pos_;
        while (pos_ < s_.size() && !std::isspace(static_cast<unsigned char>(s_[pos_])) && s_[pos_] != '>' &&
               s_[pos_] != '/')
            ++pos_;
        node.name = s_.substr(nameStart, pos_ - nameStart);
        // Skip attributes, honoring quoted values.
        while (pos_ < s_.size() && s_[pos_] != '>' && !(s_[pos_] == '/' && pos_ + 1 < s_.size() && s_[pos_ + 1] == '>')) {
            if (s_[pos_] == '"' || s_[pos_] == '\'') {
                const char quote = s_[pos_];
                const auto end = s_.find(quote, pos_ + 1);
                pos_ = end == std::string::npos ? s_.size() : end + 1;
            } else {
                ++pos_;
            }
        }
        if (pos_ >= s_.size()) {
            error_ = "Unterminated <" + node.name + ">";
            return false;
        }
        if (s_[pos_] == '/') {
            pos_ += 2;
            return true;
        }
        ++pos_;  // '>'

        std::string text;
        for (;;) {
            if (pos_ >= s_.size()) {
                error_ = "Missing </" + node.name + ">";
                return false;
            }
            if (StartsWith("</")) {
                const auto end = s_.find('>', pos_);
                if (end == std::string::npos) {
                    error_ = "Unterminated closing tag";
                    return false;
                }
                const std::string closing = s_.substr(pos_ + 2, end - pos_ - 2);
                pos_ = end + 1;
                if (closing.substr(0, closing.find_last_not_of(" \t\r\n") + 1) != node.name) {
                    error_ = "Mismatched </" + closing + "> for <" + node.name + ">";
                    return false;
                }
                node.text = Decode(text);
                return true;
            }
            if (StartsWith("<![CDATA[")) {
                const auto end = s_.find("]]>", pos_);
                if (end == std::string::npos) {
                    error_ = "Unterminated CDATA";
                    return false;
                }
                text += s_.substr(pos_ + 9, end - pos_ - 9);
                pos_ = end + 3;
                continue;
            }
            if (StartsWith("<!--")) {
                const auto end = s_.find("-->", pos_);
                pos_ = end == std::string::npos ? s_.size() : end + 3;
                continue;
            }
            if (s_[pos_] == '<') {
                XmlNode child;
                if (!ParseElement(child, depth + 1)) return false;
                node.children.push_back(std::move(child));
                continue;
            }
            const auto next = s_.find('<', pos_);
            text += s_.substr(pos_, (next == std::string::npos ? s_.size() : next) - pos_);
            pos_ = next == std::string::npos ? s_.size() : next;
        }
    }

    const std::string& s_;
    size_t pos_ = 0;
    std::string error_;
};

std::string Trim(const std::string& text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

std::string Escape(const std::string& text) {
    std::string out;
    for (const char ch : text) {
        switch (ch) {
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '&': out += "&amp;"; break;
            default: out += ch;
        }
    }
    return out;
}

bool IsValueType(const std::string& type) {
    static const char* const kTypes[] = {"Byte", "2 Bytes", "4 Bytes", "8 Bytes", "Float", "Double", "String",
                                         "Array of byte"};
    return std::find(std::begin(kTypes), std::end(kTypes), type) != std::end(kTypes);
}

void ReadEntries(const XmlNode& list, int depth, CheatTable& table) {
    for (const auto& node : list.children) {
        if (node.name != "CheatEntry") continue;
        CheatTableEntry entry;
        entry.depth = depth;
        entry.description = Trim(node.ChildText("Description"));
        if (entry.description.size() >= 2 && entry.description.front() == '"' && entry.description.back() == '"')
            entry.description = entry.description.substr(1, entry.description.size() - 2);
        entry.variableType = Trim(node.ChildText("VariableType"));
        entry.groupHeader = Trim(node.ChildText("GroupHeader")) == "1";
        entry.address = Trim(node.ChildText("Address"));
        entry.showAsHex = Trim(node.ChildText("ShowAsHex")) == "1";
        entry.showAsSigned = Trim(node.ChildText("ShowAsSigned")) != "0";
        entry.unicode = Trim(node.ChildText("Unicode")) == "1";
        entry.length = std::atoi(Trim(node.ChildText("Length")).c_str());
        if (const XmlNode* offsets = node.Child("Offsets")) {
            // Cheat Engine lists the last offset first.
            for (auto it = offsets->children.rbegin(); it != offsets->children.rend(); ++it) {
                if (it->name != "Offset") continue;
                entry.offsets.push_back(static_cast<uint32_t>(std::strtoul(Trim(it->text).c_str(), nullptr, 16)));
            }
        }
        if (entry.variableType == "Auto Assembler Script" || node.Child("AssemblerScript")) {
            entry.script = true;
            ++table.scripts;
        } else if (!entry.groupHeader && !IsValueType(entry.variableType)) {
            ++table.unsupported;
        } else {
            table.entries.push_back(entry);
        }
        if (const XmlNode* children = node.Child("CheatEntries")) ReadEntries(*children, depth + 1, table);
    }
}

} // namespace

bool ParseCheatTable(const std::string& xml, CheatTable& table, std::string* error) {
    table = CheatTable{};
    XmlNode root;
    XmlReader reader(xml);
    if (!reader.ParseDocument(root, error)) return false;
    if (root.name != "CheatTable") {
        if (error) *error = "Not a Cheat Engine table (the root element is <" + root.name + ">)";
        return false;
    }
    if (const XmlNode* entries = root.Child("CheatEntries")) ReadEntries(*entries, 0, table);
    table.luaScript = root.ChildText("LuaScript");
    return true;
}

std::string WriteCheatTable(const CheatTable& table) {
    std::ostringstream out;
    out << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    out << "<CheatTable CheatEngineTableVersion=\"45\">\n  <CheatEntries>\n";
    int id = 0;
    int open = 0;  // group headers still open
    auto indent = [&](int level) { return std::string(static_cast<size_t>(4 + level * 4), ' '); };
    for (size_t i = 0; i < table.entries.size(); ++i) {
        const auto& entry = table.entries[i];
        while (open > entry.depth) {
            --open;
            out << indent(open * 2 + 1) << "</CheatEntries>\n" << indent(open * 2) << "</CheatEntry>\n";
        }
        const std::string in = indent(entry.depth * 2);
        const std::string field = indent(entry.depth * 2 + 1);
        out << in << "<CheatEntry>\n";
        out << field << "<ID>" << id++ << "</ID>\n";
        out << field << "<Description>\"" << Escape(entry.description) << "\"</Description>\n";
        if (entry.groupHeader) {
            out << field << "<GroupHeader>1</GroupHeader>\n";
        } else {
            if (entry.showAsHex) out << field << "<ShowAsHex>1</ShowAsHex>\n";
            if (!entry.showAsSigned) out << field << "<ShowAsSigned>0</ShowAsSigned>\n";
            out << field << "<VariableType>" << Escape(entry.variableType) << "</VariableType>\n";
            if (entry.variableType == "String" || entry.variableType == "Array of byte")
                out << field << "<Length>" << entry.length << "</Length>\n";
            if (entry.variableType == "String") {
                out << field << "<Unicode>" << (entry.unicode ? 1 : 0) << "</Unicode>\n";
                out << field << "<CodePage>0</CodePage>\n";
                out << field << "<ZeroTerminate>1</ZeroTerminate>\n";
            }
            out << field << "<Address>" << Escape(entry.address) << "</Address>\n";
            if (!entry.offsets.empty()) {
                out << field << "<Offsets>\n";
                for (auto it = entry.offsets.rbegin(); it != entry.offsets.rend(); ++it) {
                    char buffer[16] = {};
                    std::snprintf(buffer, sizeof(buffer), "%X", *it);
                    out << field << "  <Offset>" << buffer << "</Offset>\n";
                }
                out << field << "</Offsets>\n";
            }
        }
        const bool hasChildren = i + 1 < table.entries.size() && table.entries[i + 1].depth > entry.depth;
        if (hasChildren) {
            out << field << "<CheatEntries>\n";
            ++open;
        } else {
            out << in << "</CheatEntry>\n";
        }
    }
    while (open > 0) {
        --open;
        out << indent(open * 2 + 1) << "</CheatEntries>\n" << indent(open * 2) << "</CheatEntry>\n";
    }
    out << "  </CheatEntries>\n  <UserdefinedSymbols/>\n";
    if (!table.luaScript.empty()) out << "  <LuaScript>" << Escape(table.luaScript) << "</LuaScript>\n";
    out << "</CheatTable>\n";
    return out.str();
}

bool LoadCheatTable(const std::string& path, CheatTable& table, std::string* error) {
    std::ifstream input(std::filesystem::u8path(path), std::ios::binary);
    if (!input) {
        if (error) *error = "Cannot open " + path;
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    std::string text = buffer.str();
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF) text.erase(0, 3);  // UTF-8 BOM
    return ParseCheatTable(text, table, error);
}

bool SaveCheatTable(const std::string& path, const CheatTable& table, std::string* error) {
    std::ofstream output(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
    if (!output) {
        if (error) *error = "Cannot write " + path;
        return false;
    }
    output << WriteCheatTable(table);
    return static_cast<bool>(output);
}

bool ParseCheatAddress(const std::string& raw, std::string& module, uint64_t& offset) {
    std::string text = Trim(raw);
    module.clear();
    offset = 0;
    if (text.empty()) return false;
    const auto plus = text.find_last_of('+');
    std::string number = text;
    if (plus != std::string::npos && plus > 0) {
        module = Trim(text.substr(0, plus));
        if (module.size() >= 2 && module.front() == '"' && module.back() == '"')
            module = module.substr(1, module.size() - 2);
        number = Trim(text.substr(plus + 1));
    }
    if (number.size() > 2 && number[0] == '0' && (number[1] == 'x' || number[1] == 'X')) number.erase(0, 2);
    if (number.empty() || number.size() > 16) return false;
    for (const char ch : number)
        if (!std::isxdigit(static_cast<unsigned char>(ch))) return false;
    offset = std::strtoull(number.c_str(), nullptr, 16);
    return !module.empty() || offset != 0;
}

std::string FormatCheatAddress(const std::string& module, uint64_t offset) {
    char buffer[40] = {};
    if (module.empty()) {
        std::snprintf(buffer, sizeof(buffer), offset > 0xFFFFFFFFull ? "%016llX" : "%08llX",
                      static_cast<unsigned long long>(offset));
        return buffer;
    }
    std::snprintf(buffer, sizeof(buffer), "%08llX", static_cast<unsigned long long>(offset));
    return "\"" + module + "\"+" + buffer;
}

} // namespace cortex::application
