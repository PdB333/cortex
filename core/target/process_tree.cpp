#include "process_tree.h"

#include <functional>
#include <map>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <winternl.h>
#endif

namespace cortex::target {
namespace {

using json = nlohmann::json;

#if defined(_WIN32)
std::string Narrow(const wchar_t* text, size_t length) {
    if (!length) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size > 0 ? size : 0), '\0');
    if (size > 0) WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), out.data(), size, nullptr, nullptr);
    return out;
}

// ProcessCommandLineInformation (60): a UNICODE_STRING followed by its text.
bool CommandLine(HANDLE process, std::string& text) {
    using Query = LONG(WINAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static const auto query = reinterpret_cast<Query>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess")));
    if (!query) return false;
    ULONG needed = 0;
    query(process, 60, nullptr, 0, &needed);
    if (needed < sizeof(UNICODE_STRING) || needed > (1u << 20)) return false;
    std::vector<unsigned char> buffer(needed);
    if (query(process, 60, buffer.data(), needed, &needed) < 0) return false;
    const auto* string = reinterpret_cast<const UNICODE_STRING*>(buffer.data());
    if (!string->Buffer || string->Length == 0) return false;
    const auto* begin = reinterpret_cast<const unsigned char*>(string->Buffer);
    if (begin < buffer.data() || begin + string->Length > buffer.data() + buffer.size()) return false;
    text = Narrow(string->Buffer, string->Length / sizeof(wchar_t));
    return true;
}

// Fallback for systems without class 60 (older Windows, Wine): read
// PEB -> ProcessParameters -> CommandLine. Needs PROCESS_VM_READ; reads only.
bool CommandLineFromPeb(HANDLE process, std::string& text) {
    using Query = LONG(WINAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static const auto query = reinterpret_cast<Query>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess")));
    if (!query) return false;
    auto read = [&](uint64_t address, void* out, size_t size) {
        SIZE_T done = 0;
        return address && ReadProcessMemory(process, reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(address)), out, size, &done) &&
               done == size;
    };
    auto finish = [&](uint64_t buffer, uint16_t length) {
        if (!buffer || !length || length > 32766) return false;
        std::wstring wide(length / sizeof(wchar_t), L'\0');
        if (!read(buffer, wide.data(), length)) return false;
        text = Narrow(wide.data(), wide.size());
        return true;
    };
#if defined(_WIN64)
    // A 32-bit process under WOW64 has its own 32-bit PEB.
    ULONG_PTR peb32 = 0;
    if (query(process, 26 /*ProcessWow64Information*/, &peb32, sizeof(peb32), nullptr) >= 0 && peb32) {
        uint32_t parameters = 0, buffer = 0;
        uint16_t length = 0;
        return read(peb32 + 0x10, &parameters, 4) && read(uint64_t{parameters} + 0x40, &length, 2) &&
               read(uint64_t{parameters} + 0x44, &buffer, 4) && finish(buffer, length);
    }
    constexpr uint64_t kParameters = 0x20, kCommandLine = 0x70;
#else
    constexpr uint64_t kParameters = 0x10, kCommandLine = 0x40;
#endif
    PROCESS_BASIC_INFORMATION basic{};
    if (query(process, 0 /*ProcessBasicInformation*/, &basic, sizeof(basic), nullptr) < 0 || !basic.PebBaseAddress) return false;
    uintptr_t parameters = 0, buffer = 0;
    uint16_t length = 0;
    return read(reinterpret_cast<uintptr_t>(basic.PebBaseAddress) + kParameters, &parameters, sizeof(parameters)) &&
           read(parameters + kCommandLine, &length, 2) &&
           read(parameters + kCommandLine + sizeof(void*), &buffer, sizeof(buffer)) && finish(buffer, length);
}
#endif

} // namespace

std::vector<ProcessNode> SnapshotProcesses() {
    std::vector<ProcessNode> processes;
#if defined(_WIN32)
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return processes;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            ProcessNode node;
            node.pid = entry.th32ProcessID;
            node.parent = entry.th32ParentProcessID;
            node.name = Narrow(entry.szExeFile, wcslen(entry.szExeFile));
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, entry.th32ProcessID);
            if (!process) process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (process) {
                wchar_t path[32768];
                DWORD length = 32768;
                if (QueryFullProcessImageNameW(process, 0, path, &length)) node.path = Narrow(path, length);
                FILETIME created{}, exited{}, kernel{}, user{};
                if (GetProcessTimes(process, &created, &exited, &kernel, &user))
                    node.generation = (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
                node.commandLineRead = CommandLine(process, node.commandLine) || CommandLineFromPeb(process, node.commandLine);
                CloseHandle(process);
            }
            processes.push_back(std::move(node));
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
#endif
    return processes;
}

std::string ChromiumRole(const std::string& commandLine) {
    auto value = [&](const std::string& key) -> std::string {
        size_t at = 0;
        while ((at = commandLine.find(key, at)) != std::string::npos) {
            // A switch starts the line or follows whitespace or a quote.
            if (at == 0 || commandLine[at - 1] == ' ' || commandLine[at - 1] == '"' || commandLine[at - 1] == '\t') {
                size_t start = at + key.size(), end = start;
                const bool quoted = start < commandLine.size() && commandLine[start] == '"';
                if (quoted) ++start, end = commandLine.find('"', start);
                else end = commandLine.find_first_of(" \t\"", start);
                return commandLine.substr(start, (end == std::string::npos ? commandLine.size() : end) - start);
            }
            at += key.size();
        }
        return {};
    };
    const std::string type = value("--type=");
    if (type == "utility") {
        const std::string sub = value("--utility-sub-type=");
        return sub.empty() ? type : type + ":" + sub;
    }
    return type;
}

json ProcessTreeJson(const std::vector<ProcessNode>& processes, uint64_t root, size_t maxProcesses) {
    std::map<uint64_t, const ProcessNode*> byPid;
    std::multimap<uint64_t, const ProcessNode*> children;
    for (const auto& node : processes) byPid[node.pid] = &node;
    for (const auto& node : processes)
        if (node.pid != node.parent) children.emplace(node.parent, &node);
    const auto top = byPid.find(root);
    if (top == byPid.end()) return {{"ok", false}, {"error", "process_not_found"}};

    json rows = json::array();
    bool truncated = false, electron = false;
    std::function<void(const ProcessNode&, size_t)> walk = [&](const ProcessNode& node, size_t depth) {
        if (rows.size() >= maxProcesses || depth > 16) {
            truncated = true;
            return;
        }
        std::string role = ChromiumRole(node.commandLine);
        if (!role.empty()) electron = true;
        std::string commandLine = node.commandLine.size() > 1024 ? node.commandLine.substr(0, 1024) + "..." : node.commandLine;
        rows.push_back({{"pid", node.pid},
                        {"parent", node.parent},
                        {"depth", depth},
                        {"name", node.name},
                        {"path", node.path},
                        {"role", role},
                        {"command_line", node.commandLineRead ? json(commandLine) : json(nullptr)}});
        const auto range = children.equal_range(node.pid);
        for (auto it = range.first; it != range.second; ++it) {
            const ProcessNode& child = *it->second;
            // A child created before its "parent" belongs to a recycled PID.
            if (node.generation && child.generation && child.generation < node.generation) continue;
            walk(child, depth + 1);
        }
    };
    walk(*top->second, 0);
    // The top of a Chromium/Electron tree has no --type: it is the main process.
    if (electron && rows[0]["role"] == "") rows[0]["role"] = "main";
    return {{"ok", true}, {"root", root}, {"processes", rows}, {"electron", electron}, {"truncated", truncated},
            {"note", "roles come from each process's --type= switch; nothing is injected"}};
}

} // namespace cortex::target
