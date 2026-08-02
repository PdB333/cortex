#include "external.h"
#include "symbols.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace symbols {
namespace {

bool FileExists(const std::string& path) {
    if (path.empty()) return false;
    const DWORD attributes = GetFileAttributesA(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::string BaseNameLower(std::string path) {
    const size_t position = path.find_last_of("\\/");
    if (position != std::string::npos) path.erase(0, position + 1);
    std::transform(path.begin(), path.end(), path.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return path;
}

std::string FindExecutable(const char* name) {
    char buffer[32768]{};
    const DWORD length = SearchPathA(nullptr, name, nullptr,
                                     static_cast<DWORD>(sizeof(buffer)), buffer, nullptr);
    if (!length || length >= sizeof(buffer)) return {};
    return std::string(buffer, length);
}

std::string Quote(const std::string& value) {
    std::string output = "\"";
    size_t slashes = 0;
    for (char character : value) {
        if (character == '\\') {
            ++slashes;
            continue;
        }
        if (character == '"') {
            output.append(slashes * 2 + 1, '\\');
            output.push_back('"');
            slashes = 0;
            continue;
        }
        output.append(slashes, '\\');
        slashes = 0;
        output.push_back(character);
    }
    output.append(slashes * 2, '\\');
    output.push_back('"');
    return output;
}

bool RunAndCapture(const std::string& executable, const std::string& arguments,
                   std::string& output, DWORD& exitCode) {
    output.clear();
    exitCode = ERROR_GEN_FAILURE;
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &security, 0)) return false;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};

    std::string commandLine = Quote(executable);
    if (!arguments.empty()) {
        commandLine.push_back(' ');
        commandLine += arguments;
    }
    std::vector<char> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back('\0');
    const BOOL created = CreateProcessA(executable.c_str(), mutableCommand.data(),
                                        nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                        nullptr, nullptr, &startup, &process);
    CloseHandle(writePipe);
    if (!created) {
        CloseHandle(readPipe);
        return false;
    }

    const DWORD wait = WaitForSingleObject(process.hProcess, 10000);
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, ERROR_TIMEOUT);
        WaitForSingleObject(process.hProcess, 1000);
    }

    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(readPipe, buffer, sizeof(buffer), &read, nullptr) && read)
        output.append(buffer, buffer + read);

    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(readPipe);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return wait != WAIT_FAILED;
}

std::vector<std::string> Lines(const std::string& value) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= value.size()) {
        size_t end = value.find('\n', start);
        if (end == std::string::npos) end = value.size();
        std::string line = value.substr(start, end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        size_t first = 0;
        while (first < line.size() && (line[first] == ' ' || line[first] == '\t')) ++first;
        if (first) line.erase(0, first);
        if (!line.empty()) lines.push_back(line);
        if (end == value.size()) break;
        start = end + 1;
    }
    return lines;
}

bool ParseFileLine(std::string value, std::string& file, uint32_t& line) {
    line = 0;
    if (value.empty() || value == "??:0" || value == "??:0:0") return false;
    const size_t lastColon = value.rfind(':');
    if (lastColon == std::string::npos) {
        file = value;
        return file != "??";
    }
    bool lastNumeric = lastColon + 1 < value.size();
    for (size_t i = lastColon + 1; i < value.size(); ++i)
        lastNumeric = lastNumeric && std::isdigit(static_cast<unsigned char>(value[i]));
    if (lastNumeric) {
        const uint32_t trailing = static_cast<uint32_t>(std::strtoul(value.c_str() + lastColon + 1, nullptr, 10));
        value.erase(lastColon);
        const size_t previousColon = value.rfind(':');
        bool previousNumeric = previousColon != std::string::npos && previousColon + 1 < value.size();
        if (previousNumeric) {
            for (size_t i = previousColon + 1; i < value.size(); ++i)
                previousNumeric = previousNumeric && std::isdigit(static_cast<unsigned char>(value[i]));
        }
        if (previousNumeric) {
            line = static_cast<uint32_t>(std::strtoul(value.c_str() + previousColon + 1, nullptr, 10));
            value.erase(previousColon);
        } else {
            line = trailing;
        }
    }
    file = value;
    return file != "??" && !file.empty();
}

std::optional<ExternalLocation> RunLlvm(const std::string& executable,
                                        const std::string& imagePath,
                                        uintptr_t rva) {
    char address[32]{};
#ifdef _WIN64
    std::snprintf(address, sizeof(address), "0x%llX", static_cast<unsigned long long>(rva));
#else
    std::snprintf(address, sizeof(address), "0x%lX", static_cast<unsigned long>(rva));
#endif
    const std::string arguments = "--demangle --functions=linkage --inlining=false "
                                  "--relative-address --obj=" + Quote(imagePath) + " " + address;
    std::string raw;
    DWORD exitCode = 0;
    if (!RunAndCapture(executable, arguments, raw, exitCode) || exitCode != 0) return std::nullopt;
    const auto lines = Lines(raw);
    if (lines.empty()) return std::nullopt;
    ExternalLocation result;
    result.tool = executable;
    if (lines[0] != "??") result.function = lines[0];
    if (lines.size() > 1) ParseFileLine(lines[1], result.file, result.line);
    if (result.function.empty() && result.file.empty()) return std::nullopt;
    return result;
}

std::optional<ExternalLocation> RunAddr2Line(const std::string& executable,
                                             const std::string& imagePath,
                                             uintptr_t rva) {
    const ModuleIdentity identity = InspectModule(imagePath);
    const uint64_t virtualAddress = identity.preferredImageBase + static_cast<uint64_t>(rva);
    char address[32]{};
    std::snprintf(address, sizeof(address), "0x%llX",
                  static_cast<unsigned long long>(virtualAddress));
    const std::string arguments = "-f -C -e " + Quote(imagePath) + " " + address;
    std::string raw;
    DWORD exitCode = 0;
    if (!RunAndCapture(executable, arguments, raw, exitCode) || exitCode != 0) return std::nullopt;
    const auto lines = Lines(raw);
    if (lines.empty()) return std::nullopt;
    ExternalLocation result;
    result.tool = executable;
    if (lines[0] != "??") result.function = lines[0];
    if (lines.size() > 1) ParseFileLine(lines[1], result.file, result.line);
    if (result.function.empty() && result.file.empty()) return std::nullopt;
    return result;
}

std::optional<ExternalLocation> RunTool(const std::string& executable,
                                        const std::string& imagePath,
                                        uintptr_t rva) {
    const std::string name = BaseNameLower(executable);
    if (name.find("llvm-symbolizer") != std::string::npos)
        return RunLlvm(executable, imagePath, rva);
    return RunAddr2Line(executable, imagePath, rva);
}

} // namespace

std::optional<ExternalLocation> ResolveExternal(const std::string& imagePath,
                                                uintptr_t moduleRva,
                                                const std::string& toolPath) {
    if (!FileExists(imagePath)) return std::nullopt;
    if (!toolPath.empty()) {
        if (!FileExists(toolPath)) return std::nullopt;
        return RunTool(toolPath, imagePath, moduleRva);
    }
    std::string llvm = FindExecutable("llvm-symbolizer.exe");
    if (!llvm.empty()) {
        auto result = RunLlvm(llvm, imagePath, moduleRva);
        if (result) return result;
    }
    std::string addr2line = FindExecutable("addr2line.exe");
    if (!addr2line.empty()) return RunAddr2Line(addr2line, imagePath, moduleRva);
    return std::nullopt;
}

} // namespace symbols
