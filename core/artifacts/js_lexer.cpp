#include "js_lexer.h"

#include <algorithm>
#include <cstring>

namespace cortex::artifacts::js {
namespace {

constexpr size_t kMaxWarnings = 32;

bool IdentStart(unsigned char c) { return std::isalpha(c) || c == '_' || c == '$' || c >= 0x80; }
bool IdentPart(unsigned char c) { return IdentStart(c) || std::isdigit(c); }

// Keywords after which a `/` starts a regular expression, not a division.
bool RegexAfterKeyword(std::string_view word) {
    static const char* const words[] = {"return", "typeof", "instanceof", "in",    "of",   "new",
                                        "delete", "void",   "throw",      "case",  "do",   "else",
                                        "yield",  "await",  "extends",    "default"};
    return std::any_of(std::begin(words), std::end(words), [&](const char* w) { return word == w; });
}

// Longest first, so ">>>=" wins over ">>".
const char* const kPunctuators[] = {
    ">>>=", "...", "===", "!==", "**=", "<<=", ">>=", ">>>", "&&=", "||=", "?\?=", "=>", "==", "!=",
    "<=",   ">=",  "&&",  "||",  "??",  "?.",  "++",  "--",  "+=",  "-=",  "*=",  "%=", "&=", "|=",
    "^=",   "**",  "<<",  ">>",  "{",   "}",   "(",   ")",   "[",   "]",   ";",   ",",  "<",  ">",
    "+",    "-",   "*",   "%",   "&",   "|",   "^",   "!",   "~",   "?",   ":",   "=",  ".",  "@",
};

void AppendUtf8(std::string& out, uint32_t code) {
    if (code > 0x10FFFF) code = 0xFFFD;
    if (code < 0x80) {
        out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
}

int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// The text between the quotes (or backticks) with escapes resolved.
std::string Cook(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        const char c = raw[i];
        if (c != '\\' || i + 1 >= raw.size()) {
            out.push_back(c);
            continue;
        }
        const char e = raw[++i];
        switch (e) {
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'v': out.push_back('\v'); break;
            case '0': out.push_back('\0'); break;
            case '\r':
                if (i + 1 < raw.size() && raw[i + 1] == '\n') ++i;
                break;  // line continuation
            case '\n': break;
            case 'x':
                if (i + 2 < raw.size() && HexDigit(raw[i + 1]) >= 0 && HexDigit(raw[i + 2]) >= 0) {
                    AppendUtf8(out, static_cast<uint32_t>(HexDigit(raw[i + 1]) * 16 + HexDigit(raw[i + 2])));
                    i += 2;
                } else {
                    out.push_back(e);
                }
                break;
            case 'u': {
                uint32_t code = 0;
                if (i + 1 < raw.size() && raw[i + 1] == '{') {
                    size_t j = i + 2;
                    int digits = 0;
                    while (j < raw.size() && raw[j] != '}' && HexDigit(raw[j]) >= 0 && digits < 8) {
                        code = code * 16 + static_cast<uint32_t>(HexDigit(raw[j]));
                        ++j;
                        ++digits;
                    }
                    if (j < raw.size() && raw[j] == '}' && digits) {
                        AppendUtf8(out, code);
                        i = j;
                        break;
                    }
                } else if (i + 4 < raw.size()) {
                    bool ok = true;
                    for (size_t k = 1; k <= 4; ++k) {
                        const int d = HexDigit(raw[i + k]);
                        if (d < 0) ok = false;
                        else code = code * 16 + static_cast<uint32_t>(d);
                    }
                    if (ok) {
                        AppendUtf8(out, code);
                        i += 4;
                        break;
                    }
                }
                out.push_back(e);
                break;
            }
            default: out.push_back(e);
        }
    }
    return out;
}

// Line/column of increasing byte offsets, in one pass over the source.
class Positions {
public:
    explicit Positions(std::string_view source) : source_(source) {}
    void At(size_t target, uint32_t& line, uint32_t& column) {
        while (pos_ < target && pos_ < source_.size()) {
            const auto c = static_cast<unsigned char>(source_[pos_]);
            if (c == '\n') {
                ++line_;
                column_ = 1;
            } else if (c == '\r') {
                if (pos_ + 1 >= source_.size() || source_[pos_ + 1] != '\n') {
                    ++line_;
                    column_ = 1;
                }
            } else if ((c & 0xC0) == 0x80) {
                // UTF-8 continuation byte: same UTF-16 unit
            } else if ((c & 0xF8) == 0xF0) {
                column_ += 2;  // outside the BMP: a surrogate pair
            } else {
                ++column_;
            }
            ++pos_;
        }
        line = line_;
        column = column_;
    }

private:
    std::string_view source_;
    size_t pos_ = 0;
    uint32_t line_ = 1;
    uint32_t column_ = 1;
};

class Lexer {
public:
    Lexer(std::string_view source, size_t maxTokens) : s_(source), positions_(source), maxTokens_(maxTokens) {}

    LexResult Run() {
        Stats();
        size_t i = 0;
        if (s_.size() >= 3 && static_cast<unsigned char>(s_[0]) == 0xEF && static_cast<unsigned char>(s_[1]) == 0xBB &&
            static_cast<unsigned char>(s_[2]) == 0xBF)
            i = 3;
        if (s_.compare(i, 2, "#!") == 0) i = LineEnd(i);
        while (i < s_.size()) {
            if (result_.tokens.size() >= maxTokens_) {
                result_.truncated = true;
                break;
            }
            const auto c = static_cast<unsigned char>(s_[i]);
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f') {
                ++i;
                continue;
            }
            if (c == 0xC2 && i + 1 < s_.size() && static_cast<unsigned char>(s_[i + 1]) == 0xA0) {
                i += 2;  // no-break space
                continue;
            }
            if (c == 0xE2 && i + 2 < s_.size() && static_cast<unsigned char>(s_[i + 1]) == 0x80 &&
                (static_cast<unsigned char>(s_[i + 2]) == 0xA8 || static_cast<unsigned char>(s_[i + 2]) == 0xA9)) {
                i += 3;  // line/paragraph separator
                continue;
            }
            if (c == '/' && i + 1 < s_.size() && s_[i + 1] == '/') {
                const size_t end = LineEnd(i);
                Comment(i, end, i + 2);
                i = end;
                continue;
            }
            if (c == '/' && i + 1 < s_.size() && s_[i + 1] == '*') {
                const size_t close = s_.find("*/", i + 2);
                const size_t end = close == std::string_view::npos ? s_.size() : close + 2;
                if (close == std::string_view::npos) Warn("unterminated_comment", i);
                Comment(i, end, i + 2);
                i = end;
                continue;
            }
            if (c == '\'' || c == '"') {
                i = String(i);
                continue;
            }
            if (c == '`') {
                i = Template(i, i + 1);
                continue;
            }
            if (c == '}' && !braces_.empty() && braces_.back() == 'T') {
                braces_.pop_back();
                i = Template(i, i + 1);
                continue;
            }
            if (std::isdigit(c) || (c == '.' && i + 1 < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i + 1])))) {
                i = Number(i);
                continue;
            }
            if (IdentStart(c) || c == '#' || (c == '\\' && i + 1 < s_.size() && s_[i + 1] == 'u')) {
                i = Identifier(i);
                continue;
            }
            if (c == '/') {
                const size_t end = RegexAllowed() ? Regex(i) : 0;
                if (end) {
                    Emit(TokenKind::Regex, i, end);
                    i = end;
                    continue;
                }
                const size_t length = i + 1 < s_.size() && s_[i + 1] == '=' ? 2 : 1;
                Emit(TokenKind::Punctuator, i, i + length);
                i += length;
                continue;
            }
            i = Punctuator(i);
        }
        if (!braces_.empty() && braces_.back() == 'T') Warn("unterminated_template", s_.size());
        if (suppressedWarnings_)
            result_.warnings.push_back("... " + std::to_string(suppressedWarnings_) + " more warning(s)");
        return std::move(result_);
    }

private:
    void Stats() {
        uint32_t lines = s_.empty() ? 0 : 1, current = 0, longest = 0;
        uint64_t whitespace = 0;
        for (size_t i = 0; i < s_.size(); ++i) {
            const char c = s_[i];
            if (c == '\n' || (c == '\r' && (i + 1 >= s_.size() || s_[i + 1] != '\n'))) {
                longest = std::max(longest, current);
                current = 0;
                if (i + 1 < s_.size()) ++lines;
                ++whitespace;
                continue;
            }
            if (c != '\r') ++current;
            if (c == ' ' || c == '\t' || c == '\r') ++whitespace;
        }
        result_.lines = lines;
        result_.longestLine = std::max(longest, current);
        result_.whitespaceBytes = whitespace;
    }

    size_t LineEnd(size_t i) const {
        while (i < s_.size() && s_[i] != '\n' && s_[i] != '\r') ++i;
        return i;
    }

    void Warn(const char* what, size_t at) {
        if (result_.warnings.size() >= kMaxWarnings) {
            ++suppressedWarnings_;
            return;
        }
        uint32_t line = 0, column = 0;
        Positions probe(s_);
        probe.At(at, line, column);
        result_.warnings.push_back(std::string(what) + " at " + std::to_string(line) + ":" + std::to_string(column));
    }

    void Comment(size_t begin, size_t end, size_t textStart) {
        result_.commentBytes += end - begin;
        std::string_view text = s_.substr(textStart, end - textStart);
        if (text.size() >= 2 && text.substr(text.size() - 2) == "*/") text.remove_suffix(2);
        while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
        if (text.size() < 20 || (text[0] != '#' && text[0] != '@')) return;
        text.remove_prefix(1);
        while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
        constexpr std::string_view key = "sourceMappingURL=";
        if (text.substr(0, key.size()) != key) return;
        text.remove_prefix(key.size());
        size_t length = 0;
        while (length < text.size() && !std::isspace(static_cast<unsigned char>(text[length]))) ++length;
        result_.sourceMappingUrl = std::string(text.substr(0, length));
        uint32_t column = 0;
        positions_.At(begin, result_.sourceMappingLine, column);
    }

    void Emit(TokenKind kind, size_t begin, size_t end, std::string value = {}, bool substitutions = false) {
        Token token;
        token.kind = kind;
        token.begin = static_cast<uint32_t>(begin);
        token.end = static_cast<uint32_t>(end);
        positions_.At(begin, token.line, token.column);
        token.value = std::move(value);
        token.substitutions = substitutions;
        result_.tokens.push_back(std::move(token));
    }

    size_t String(size_t i) {
        const char quote = s_[i];
        size_t j = i + 1;
        bool closed = false;
        while (j < s_.size()) {
            const char c = s_[j];
            if (c == '\\') {
                j += 2;
                continue;
            }
            if (c == quote) {
                closed = true;
                break;
            }
            if (c == '\n' || c == '\r') break;
            ++j;
        }
        j = std::min(j, s_.size());
        if (!closed) Warn("unterminated_string", i);
        Emit(TokenKind::String, i, closed ? j + 1 : j, Cook(s_.substr(i + 1, j - i - 1)));
        return closed ? j + 1 : j;
    }

    // From `begin` (a backtick, or the `}` closing a substitution) to the
    // closing backtick or the next `${`.
    size_t Template(size_t begin, size_t j) {
        const bool head = s_[begin] == '`';
        while (j < s_.size()) {
            const char c = s_[j];
            if (c == '\\') {
                j += 2;
                continue;
            }
            if (c == '`') {
                // A whole template without substitutions is a constant.
                if (head) Emit(TokenKind::Template, begin, j + 1, Cook(s_.substr(begin + 1, j - begin - 1)), false);
                else Emit(TokenKind::Template, begin, j + 1, {}, true);
                return j + 1;
            }
            if (c == '$' && j + 1 < s_.size() && s_[j + 1] == '{') {
                Emit(TokenKind::Template, begin, j + 2, {}, true);
                braces_.push_back('T');
                return j + 2;
            }
            ++j;
        }
        Warn("unterminated_template", begin);
        Emit(TokenKind::Template, begin, s_.size(), {}, true);
        return s_.size();
    }

    size_t Number(size_t i) {
        const bool hex = s_[i] == '0' && i + 1 < s_.size() && (s_[i + 1] == 'x' || s_[i + 1] == 'X');
        size_t j = i;
        while (j < s_.size()) {
            const auto c = static_cast<unsigned char>(s_[j]);
            if (std::isalnum(c) || c == '_' || c == '.') {
                ++j;
            } else if ((c == '+' || c == '-') && !hex && (s_[j - 1] == 'e' || s_[j - 1] == 'E')) {
                ++j;
            } else {
                break;
            }
        }
        Emit(TokenKind::Number, i, j);
        return j;
    }

    size_t Identifier(size_t i) {
        size_t j = i + 1;
        while (j < s_.size()) {
            const auto c = static_cast<unsigned char>(s_[j]);
            if (IdentPart(c)) {
                ++j;
            } else if (c == '\\' && j + 1 < s_.size() && s_[j + 1] == 'u') {
                j += 2;
            } else {
                break;
            }
        }
        Emit(TokenKind::Identifier, i, j);
        return j;
    }

    bool RegexAllowed() const {
        if (result_.tokens.empty()) return true;
        const Token& last = result_.tokens.back();
        const std::string_view text = TokenText(s_, last);
        switch (last.kind) {
            case TokenKind::Identifier: return RegexAfterKeyword(text);
            case TokenKind::Number:
            case TokenKind::String:
            case TokenKind::Regex: return false;
            case TokenKind::Template: return text.size() >= 2 && text.substr(text.size() - 2) == "${";
            case TokenKind::Punctuator:
                return !(text == ")" || text == "]" || text == "}" || text == "++" || text == "--");
        }
        return true;
    }

    // End of a regular expression literal starting at `i`, or 0 if the
    // line ends first (then the `/` is a division after all).
    size_t Regex(size_t i) const {
        size_t j = i + 1;
        bool inClass = false;
        while (j < s_.size()) {
            const char c = s_[j];
            if (c == '\n' || c == '\r') return 0;
            if (c == '\\') {
                j += 2;
                continue;
            }
            if (c == '[') inClass = true;
            else if (c == ']') inClass = false;
            else if (c == '/' && !inClass) break;
            ++j;
        }
        if (j >= s_.size()) return 0;
        ++j;
        while (j < s_.size() && IdentPart(static_cast<unsigned char>(s_[j]))) ++j;
        return j;
    }

    size_t Punctuator(size_t i) {
        for (const char* p : kPunctuators) {
            const size_t length = std::strlen(p);
            if (s_.compare(i, length, p) != 0) continue;
            // `a?.5:b` is a conditional, not optional chaining.
            if (length == 2 && p[0] == '?' && p[1] == '.' && i + 2 < s_.size() &&
                std::isdigit(static_cast<unsigned char>(s_[i + 2])))
                continue;
            if (length == 1 && p[0] == '{') braces_.push_back('B');
            if (length == 1 && p[0] == '}' && !braces_.empty()) braces_.pop_back();
            Emit(TokenKind::Punctuator, i, i + length);
            return i + length;
        }
        // Anything else (a stray backslash, a control byte) is skipped.
        ++i;
        return i;
    }

    std::string_view s_;
    Positions positions_;
    size_t maxTokens_;
    LexResult result_;
    std::vector<char> braces_;
    size_t suppressedWarnings_ = 0;
};

} // namespace

LexResult Lex(std::string_view source, size_t maxTokens) {
    return Lexer(source, maxTokens).Run();
}

} // namespace cortex::artifacts::js
