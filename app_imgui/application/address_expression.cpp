#include "address_expression.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace cortex::application {
namespace {

class Parser {
public:
    Parser(const std::string& text, const AddressResolver& resolver) : s_(text), resolver_(resolver) {}

    bool Parse(uint64_t& result, std::string& error) {
        if (!Expression(result, 0)) {
            error = error_;
            return false;
        }
        Skip();
        if (pos_ != s_.size()) {
            error = "Unexpected '" + s_.substr(pos_, 1) + "'";
            return false;
        }
        return true;
    }

private:
    void Skip() {
        while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) ++pos_;
    }

    bool Fail(const std::string& message) {
        if (error_.empty()) error_ = message;
        return false;
    }

    bool Expression(uint64_t& value, int depth) {
        if (depth > 32) return Fail("The expression is nested too deeply");
        if (!Term(value, depth)) return false;
        for (;;) {
            Skip();
            if (pos_ >= s_.size() || (s_[pos_] != '+' && s_[pos_] != '-')) return true;
            const char op = s_[pos_++];
            uint64_t right = 0;
            if (!Term(right, depth)) return false;
            value = op == '+' ? value + right : value - right;
        }
    }

    bool Term(uint64_t& value, int depth) {
        if (!Factor(value, depth)) return false;
        for (;;) {
            Skip();
            if (pos_ >= s_.size() || s_[pos_] != '*') return true;
            ++pos_;
            uint64_t right = 0;
            if (!Factor(right, depth)) return false;
            value *= right;
        }
    }

    bool Factor(uint64_t& value, int depth) {
        Skip();
        if (pos_ >= s_.size()) return Fail("The expression ends too early");
        const char ch = s_[pos_];
        if (ch == '-') {  // unary minus: -8
            ++pos_;
            if (!Factor(value, depth)) return false;
            value = 0 - value;
            return true;
        }
        if (ch == '[' || ch == '(') {
            ++pos_;
            if (!Expression(value, depth + 1)) return false;
            Skip();
            const char close = ch == '[' ? ']' : ')';
            if (pos_ >= s_.size() || s_[pos_] != close) return Fail(std::string("Missing '") + close + "'");
            ++pos_;
            if (ch == '[') {
                uint64_t pointer = 0;
                if (!resolver_.readPointer || !resolver_.readPointer(value, pointer)) {
                    char text[40] = {};
                    std::snprintf(text, sizeof(text), "%llX", static_cast<unsigned long long>(value));
                    return Fail(std::string("Cannot read the pointer at ") + text);
                }
                value = pointer;
            }
            return true;
        }
        if (ch == '"' || ch == '\'') {
            const auto end = s_.find(ch, pos_ + 1);
            if (end == std::string::npos) return Fail("Missing closing quote");
            const std::string name = s_.substr(pos_ + 1, end - pos_ - 1);
            pos_ = end + 1;
            return Name(name, value);
        }
        const size_t start = pos_;
        while (pos_ < s_.size() &&
               (std::isalnum(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '_' || s_[pos_] == '.' ||
                s_[pos_] == '$' || s_[pos_] == '@'))
            ++pos_;
        if (pos_ == start) return Fail("Unexpected '" + std::string(1, ch) + "'");
        std::string token = s_.substr(start, pos_ - start);
        std::string digits = token;
        if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) digits.erase(0, 2);
        else if (!digits.empty() && digits[0] == '$') digits.erase(0, 1);
        bool hex = !digits.empty() && digits.size() <= 16;
        for (const char digit : digits) hex = hex && std::isxdigit(static_cast<unsigned char>(digit));
        if (hex) {
            value = std::strtoull(digits.c_str(), nullptr, 16);
            return true;
        }
        return Name(token, value);
    }

    bool Name(const std::string& name, uint64_t& value) {
        if (resolver_.symbol && resolver_.symbol(name, value)) return true;
        return Fail("Unknown module or symbol: " + name);
    }

    const std::string& s_;
    const AddressResolver& resolver_;
    size_t pos_ = 0;
    std::string error_;
};

} // namespace

bool EvaluateAddress(const std::string& text, const AddressResolver& resolver, uint64_t& result, std::string* error) {
    std::string message;
    Parser parser(text, resolver);
    if (!parser.Parse(result, message)) {
        if (error) *error = message;
        return false;
    }
    if (error) error->clear();
    return true;
}

} // namespace cortex::application
