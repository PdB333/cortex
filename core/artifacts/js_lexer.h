#pragma once

// A JavaScript/TypeScript tokenizer for static analysis (roadmap step 4b).
// It is not a parser: it only has to know where strings, template literals,
// regular expressions and comments start and end, so that text inside them
// is never mistaken for code (an `import` inside a string is not a
// dependency). The source is untrusted: every scan is bounded by the input,
// and errors (an unterminated string, comment or template) are reported as
// warnings while lexing continues.
//
// Positions are 1-based lines and 1-based columns counted in UTF-16 code
// units, the unit source maps and browsers use.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cortex::artifacts::js {

enum class TokenKind {
    Identifier,   // names and keywords; `#name` private names too
    Punctuator,
    String,       // '...' or "..."; `value` holds the cooked text
    Template,     // a whole template without substitutions, or one piece of one
    Regex,
    Number,
};

struct Token {
    TokenKind kind = TokenKind::Punctuator;
    uint32_t begin = 0;      // byte offsets in the source
    uint32_t end = 0;
    uint32_t line = 0;
    uint32_t column = 0;
    // Cooked text of a String, or of a Template without substitutions.
    std::string value;
    // Template pieces: true when this template has ${...} substitutions,
    // so its text is not a constant.
    bool substitutions = false;
};

struct LexResult {
    std::vector<Token> tokens;
    std::vector<std::string> warnings;
    // The last `//# sourceMappingURL=` (or `//@`) comment, as written.
    std::string sourceMappingUrl;
    uint32_t sourceMappingLine = 0;
    uint32_t lines = 0;
    uint32_t longestLine = 0;      // bytes
    uint64_t whitespaceBytes = 0;
    uint64_t commentBytes = 0;
    bool truncated = false;        // maxTokens reached
};

LexResult Lex(std::string_view source, size_t maxTokens = 4u << 20);

// Text of a token in the source.
inline std::string_view TokenText(std::string_view source, const Token& token) {
    return source.substr(token.begin, token.end - token.begin);
}

} // namespace cortex::artifacts::js
