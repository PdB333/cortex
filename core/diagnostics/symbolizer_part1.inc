#include "symbolizer.h"

#include "registry.h"
#include "../symbols/symbols.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace diagnostics {
namespace {

constexpr size_t kMaximumSymbolFrames = 128;
std::atomic<bool> g_symbolizerEnabled{false};
std::atomic_flag g_symbolCaptureActive = ATOMIC_FLAG_INIT;
SymbolizerOptions g_symbolizerOptions{};
LPTOP_LEVEL_EXCEPTION_FILTER g_previousSymbolizerFilter = nullptr;

struct PreparedModule {
    ModSnapshot mod;
    symbols::ModuleLoadInfo loaded;
};

std::string JsonEscape(const std::string& input) {
    std::string output;
    output.reserve(input.size() + 16);
    char encoded[7]{};
    for (unsigned char character : input) {
        switch (character) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (character < 0x20) {
                    std::snprintf(encoded, sizeof(encoded), "\\u%04X", static_cast<unsigned>(character));
                    output += encoded;
                } else {
                    output.push_back(static_cast<char>(character));
                }
                break;
        }
    }
    return output;
}

std::string Hex(uint64_t value) {
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "0x%llX", static_cast<unsigned long long>(value));
    return buffer;
}

std::string BaseName(const std::string& path) {
    const size_t position = path.find_last_of("\\/");
    return position == std::string::npos ? path : path.substr(position + 1);
}

std::string DirectoryOf(const std::string& path) {
    const size_t position = path.find_last_of("\\/");
    return position == std::string::npos ? std::string() : path.substr(0, position);
}

bool FileExists(const std::string& path) {
    if (path.empty()) return false;
    const DWORD attributes = GetFileAttributesA(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::string JoinPath(const std::string& left, const std::string& right) {
    if (left.empty()) return right;
    if (right.empty()) return left;
    if (left.back() == '\\' || left.back() == '/') return left + right;
    if (right.front() == '\\' || right.front() == '/') return left + right;
    return left + "\\" + right;
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

std::string RemapSource(const std::string& original, const ModSnapshot* mod,
                        bool& mapped) {
    mapped = false;
    if (original.empty() || !mod || !mod->sourceRoot[0]) return original;
    if (FileExists(original)) return original;
    const std::string root = mod->sourceRoot;
    const std::string normalized = Lower(original);
    const std::array<const char*, 4> markers{{"\\src\\", "/src/", "\\include\\", "/include/"}};
    for (const char* marker : markers) {
        const size_t position = normalized.find(marker);
        if (position == std::string::npos) continue;
        std::string suffix = original.substr(position + 1);
        std::replace(suffix.begin(), suffix.end(), '/', '\\');
        const std::string candidate = JoinPath(root, suffix);
        if (FileExists(candidate)) {
            mapped = true;
            return candidate;
        }
        const size_t nextSlash = suffix.find('\\');
        if (nextSlash != std::string::npos) {
            const std::string withoutTop = JoinPath(root, suffix.substr(nextSlash + 1));
            if (FileExists(withoutTop)) {
                mapped = true;
                return withoutTop;
            }
        }
    }
    const std::string byName = JoinPath(root, BaseName(original));
    if (FileExists(byName)) {
        mapped = true;
        return byName;
    }
    return original;
}

const char* ExceptionName(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION: return "EXCEPTION_ACCESS_VIOLATION";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "EXCEPTION_ILLEGAL_INSTRUCTION";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
        case EXCEPTION_STACK_OVERFLOW: return "EXCEPTION_STACK_OVERFLOW";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "EXCEPTION_DATATYPE_MISALIGNMENT";
        case EXCEPTION_PRIV_INSTRUCTION: return "EXCEPTION_PRIV_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR: return "EXCEPTION_IN_PAGE_ERROR";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_FLT_INVALID_OPERATION: return "EXCEPTION_FLT_INVALID_OPERATION";
        case EXCEPTION_FLT_OVERFLOW: return "EXCEPTION_FLT_OVERFLOW";
        default: return "UNKNOWN_EXCEPTION";
    }
}

const char* AccessName(PEXCEPTION_RECORD record, uintptr_t& accessedAddress) {
    accessedAddress = 0;
    if (!record ||
        (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION &&
         record->ExceptionCode != EXCEPTION_IN_PAGE_ERROR) ||
        record->NumberParameters < 2) return "unknown";
    accessedAddress = static_cast<uintptr_t>(record->ExceptionInformation[1]);
    switch (record->ExceptionInformation[0]) {
        case 0: return "read";
        case 1: return "write";
        case 8: return "execute";
        default: return "unknown";
    }
}

bool FindNewestCrashDirectory(char* output, size_t outputSize) {
    if (!output || outputSize == 0 || g_symbolizerOptions.crashOutputDirectory.empty()) return false;
    char pattern[2048]{};
    std::snprintf(pattern, sizeof(pattern), "%s\\crash_*_%lu",
                  g_symbolizerOptions.crashOutputDirectory.c_str(),
                  static_cast<unsigned long>(GetCurrentProcessId()));
    WIN32_FIND_DATAA data{};
    HANDLE find = FindFirstFileA(pattern, &data);
    if (find == INVALID_HANDLE_VALUE) return false;
    char newest[MAX_PATH]{};
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
        if (!newest[0] || std::strcmp(data.cFileName, newest) > 0)
            std::strncpy(newest, data.cFileName, sizeof(newest) - 1);
    } while (FindNextFileA(find, &data));
    FindClose(find);
    if (!newest[0]) return false;
    std::snprintf(output, outputSize, "%s\\%s",
                  g_symbolizerOptions.crashOutputDirectory.c_str(), newest);
    return true;
}

const ModSnapshot* ModForAddress(uintptr_t address,
                                 const std::vector<PreparedModule>& modules) {
    for (const auto& prepared : modules) {
        const ModSnapshot& mod = prepared.mod;
        if (mod.base && mod.imageSize && address >= mod.base && address < mod.base + mod.imageSize)
            return &mod;
    }
    return nullptr;
}

