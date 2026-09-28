#include "address_expression.h"

#include <algorithm>
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
                s_[pos_] == '$' || s_[pos_] == '@' || s_[pos_] == '!'))
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

namespace {

std::string LowerName(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return text;
}

} // namespace

void UserSymbols::Set(const std::string& name, uint64_t address) {
    std::lock_guard<std::mutex> lock(mutex_);
    symbols_[LowerName(name)] = {name, address};
}

bool UserSymbols::Remove(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    return symbols_.erase(LowerName(name)) != 0;
}

bool UserSymbols::Find(const std::string& name, uint64_t& address) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = symbols_.find(LowerName(name));
    if (found == symbols_.end()) return false;
    address = found->second.second;
    return true;
}

std::vector<std::pair<std::string, uint64_t>> UserSymbols::List() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::pair<std::string, uint64_t>> list;
    for (const auto& item : symbols_) list.push_back(item.second);
    return list;
}

void UserSymbols::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    symbols_.clear();
}

void TargetSymbols::SetModules(std::vector<target::ModuleInfo> modules) {
    const bool same = modules.size() == modules_.size() &&
        std::equal(modules.begin(), modules.end(), modules_.begin(), [](const auto& left, const auto& right) {
            return left.base == right.base && left.name == right.name;
        });
    modules_ = std::move(modules);
    if (!same) exports_.clear();
}

const target::ModuleInfo* TargetSymbols::FindModule(const std::string& name) const {
    const std::string wanted = LowerName(name);
    for (const auto& module : modules_)
        if (LowerName(module.name) == wanted) return &module;
    for (const auto& module : modules_) {
        const std::string lower = LowerName(module.name);
        const auto dot = lower.find_last_of('.');
        if (dot != std::string::npos && lower.compare(0, dot, wanted) == 0 && dot == wanted.size()) return &module;
    }
    return nullptr;
}

const std::map<std::string, TargetSymbols::ExportEntry>& TargetSymbols::ExportsOf(const target::ModuleInfo& module) {
    auto found = exports_.find(module.base);
    if (found != exports_.end()) return found->second;
    std::map<std::string, ExportEntry> names;
    if (reader_)
        for (auto& item : reader_(module)) {
            const std::string key = LowerName(item.name);
            names.emplace(key, std::move(item));
        }
    return exports_.emplace(module.base, std::move(names)).first->second;
}

// An export by name; a forwarded export ("NTDLL.RtlAllocateHeap") is
// followed into the module it names, a few levels deep at most.
bool TargetSymbols::Lookup(const target::ModuleInfo& module, const std::string& name, uint64_t& value, int depth) {
    if (depth > 4) return false;
    const auto& names = ExportsOf(module);
    const auto found = names.find(LowerName(name));
    if (found == names.end()) return false;
    if (found->second.forwarder.empty()) {
        value = found->second.address;
        return true;
    }
    const std::string& forwarder = found->second.forwarder;
    const auto dot = forwarder.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= forwarder.size() || forwarder[dot + 1] == '#') return false;
    const auto* target = FindModule(forwarder.substr(0, dot));
    return target && Lookup(*target, forwarder.substr(dot + 1), value, depth + 1);
}

bool TargetSymbols::Resolve(const std::string& name, uint64_t& value) {
    if (name.empty()) return false;
    if (user_ && user_->Find(name, value)) return true;
    if (const auto* module = FindModule(name)) {
        value = module->base;
        return true;
    }
    // module!export, then module.export at each dot (module names have dots).
    const auto bang = name.find('!');
    if (bang != std::string::npos) {
        const auto* module = FindModule(name.substr(0, bang));
        return module && Lookup(*module, name.substr(bang + 1), value, 0);
    }
    for (size_t dot = name.find('.'); dot != std::string::npos; dot = name.find('.', dot + 1)) {
        const auto* module = FindModule(name.substr(0, dot));
        if (module && Lookup(*module, name.substr(dot + 1), value, 0)) return true;
    }
    for (const auto& module : modules_)
        if (Lookup(module, name, value, 0)) return true;
    return false;
}

std::vector<TargetSymbols::Export> TargetSymbols::Search(const std::string& filter, size_t limit) {
    std::vector<Export> found;
    const std::string wanted = LowerName(filter);
    for (const auto& module : modules_) {
        const auto& names = ExportsOf(module);
        for (const auto& item : names) {
            if (!wanted.empty() && item.first.find(wanted) == std::string::npos) continue;
            uint64_t address = item.second.address;
            if (!item.second.forwarder.empty() && !Lookup(module, item.second.name, address, 0)) continue;
            found.push_back({module.name + "." + item.second.name, address});
            if (found.size() >= limit) return found;
        }
    }
    return found;
}

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
