#pragma once

#include "launch_config.h"

#include <windows.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace cortex::launch {

inline std::wstring QuoteWindowsArgument(const std::wstring& value) {
    std::wstring output = L"\"";
    size_t backslashes = 0;
    for (const wchar_t ch : value) {
        if (ch == L'\\') { ++backslashes; continue; }
        if (ch == L'"') {
            output.append(backslashes * 2 + 1, L'\\');
            output.push_back(ch);
        } else {
            output.append(backslashes, L'\\');
            output.push_back(ch);
        }
        backslashes = 0;
    }
    output.append(backslashes * 2, L'\\');
    output.push_back(L'"');
    return output;
}

inline std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                   value.data(), static_cast<int>(value.size()),
                                   nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                            value.data(), static_cast<int>(value.size()),
                            result.data(), size) != size) return {};
    return result;
}

class Manager {
public:
    explicit Manager(std::vector<Profile> profiles) {
        for (auto& profile : profiles) {
            Entry entry;
            entry.profile = std::move(profile);
            entries_.emplace(entry.profile.name, std::move(entry));
        }
    }
    ~Manager() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& item : entries_) if (item.second.handle) CloseHandle(item.second.handle);
        // Closing handles does not terminate user programs. Only explicit
        // cortex_stop on a profile with allow_stop=true can terminate a child.
    }
    Manager(const Manager&) = delete;
    Manager& operator=(const Manager&) = delete;

    json Status(const std::string& name = {}) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!name.empty()) {
            auto it = entries_.find(name);
            if (it == entries_.end()) return {{"ok", false}, {"error", "launch_profile_not_found"}};
            return {{"ok", true}, {"profile", MakeStatus(it->second)}};
        }
        json records = json::array();
        for (auto& [key, entry] : entries_) {
            (void)key;
            records.push_back(MakeStatus(entry));
        }
        return {{"ok", true}, {"profiles", std::move(records)},
                {"configured", !entries_.empty()}};
    }

    bool Start(const std::string& name, json& result, std::string& error) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(name);
        if (it == entries_.end()) { error = "launch_profile_not_found"; return false; }
        Entry& entry = it->second;
        if (entry.handle && WaitForSingleObject(entry.handle, 0) == WAIT_TIMEOUT) {
            error = "launch_profile_already_running"; return false;
        }
        if (entry.launches >= entry.profile.maxRuns) {
            error = "launch_profile_run_limit_reached"; return false;
        }
        std::error_code ec;
        if (!std::filesystem::is_regular_file(entry.profile.executable, ec)) {
            error = "launch_executable_unavailable"; return false;
        }
        ec.clear();
        if (!std::filesystem::is_directory(entry.profile.workingDirectory, ec)) {
            error = "launch_working_directory_unavailable"; return false;
        }
        std::wstring command = QuoteWindowsArgument(entry.profile.executable.wstring());
        for (const auto& arg : entry.profile.arguments) {
            const std::wstring converted = Utf8ToWide(arg);
            if (!arg.empty() && converted.empty()) {
                error = "launch_argument_invalid_utf8"; return false;
            }
            command += L" " + QuoteWindowsArgument(converted);
        }
        std::vector<wchar_t> mutableCommand(command.begin(), command.end());
        mutableCommand.push_back(L'\0');
        // Never let a console child write into Cortex's MCP stdout (or read
        // its stdin). Explicitly inherit only NUL; other inheritable handles
        // belonging to the host or concurrent callers stay private.
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE nullIo = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr);
        if (nullIo == INVALID_HANDLE_VALUE) {
            error = "launch_stdio_setup_failed:" + std::to_string(GetLastError());
            return false;
        }
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = nullIo;
        startup.StartupInfo.hStdOutput = nullIo;
        startup.StartupInfo.hStdError = nullIo;
        SIZE_T attributeBytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
        std::vector<unsigned char> attributes(attributeBytes);
        startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attributeBytes)) {
            const DWORD code = GetLastError();
            CloseHandle(nullIo);
            error = "launch_attributes_failed:" + std::to_string(code);
            return false;
        }
        if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0,
                PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &nullIo, sizeof(nullIo), nullptr, nullptr)) {
            const DWORD code = GetLastError();
            DeleteProcThreadAttributeList(startup.lpAttributeList);
            CloseHandle(nullIo);
            error = "launch_handles_failed:" + std::to_string(code);
            return false;
        }
        PROCESS_INFORMATION process{};
        const std::wstring exe = entry.profile.executable.wstring();
        const std::wstring cwd = entry.profile.workingDirectory.wstring();
        const BOOL childCreated = CreateProcessW(exe.c_str(), mutableCommand.data(), nullptr,
            nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,
            nullptr, cwd.c_str(), &startup.StartupInfo, &process);
        const DWORD createError = childCreated ? ERROR_SUCCESS : GetLastError();
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        CloseHandle(nullIo);
        if (!childCreated) {
            error = "launch_failed:" + std::to_string(createError);
            return false;
        }
        CloseHandle(process.hThread);
        if (entry.handle) CloseHandle(entry.handle);
        entry.handle = process.hProcess;
        entry.pid = process.dwProcessId;
        ++entry.launches;
        entry.startedMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        FILETIME created{}, exited{}, kernel{}, user{};
        entry.generation = 0;
        if (GetProcessTimes(entry.handle, &created, &exited, &kernel, &user)) {
            ULARGE_INTEGER generation{};
            generation.LowPart = created.dwLowDateTime;
            generation.HighPart = created.dwHighDateTime;
            entry.generation = generation.QuadPart;
        }
        result = MakeStatus(entry);
        result["ok"] = true;
        result["note"] = "Process launched. Use cortex_attach with this PID to enable runtime tools.";
        return true;
    }

    bool Stop(const std::string& name, uint64_t requestedPid,
              json& result, std::string& error) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(name);
        if (it == entries_.end()) { error = "launch_profile_not_found"; return false; }
        Entry& entry = it->second;
        if (!entry.profile.allowStop) { error = "launch_stop_not_allowed"; return false; }
        if (!entry.handle || requestedPid == 0 || requestedPid != entry.pid) {
            error = "launch_pid_mismatch"; return false;
        }
        if (WaitForSingleObject(entry.handle, 0) != WAIT_TIMEOUT) {
            error = "launch_process_not_running"; return false;
        }
        // TerminateProcess is intentionally NOT automatic on MCP exit or
        // debugger detach. It is only available for an explicitly permitted
        // profile and a matching child PID, with a separate permission check.
        if (!TerminateProcess(entry.handle, 1)) {
            error = "launch_stop_failed:" + std::to_string(GetLastError());
            return false;
        }
        WaitForSingleObject(entry.handle, 3000);
        result = MakeStatus(entry);
        result["ok"] = true;
        return true;
    }

private:
    struct Entry {
        Profile profile;
        HANDLE handle = nullptr;
        DWORD pid = 0;
        uint64_t generation = 0;
        uint64_t startedMs = 0;
        int launches = 0;
    };
    static json MakeStatus(const Entry& e) {
        bool alive = e.handle && WaitForSingleObject(e.handle, 0) == WAIT_TIMEOUT;
        json data = {
            {"name", e.profile.name},
            {"allow_stop", e.profile.allowStop},
            {"max_runs", e.profile.maxRuns},
            {"runs", e.launches},
            {"pid", e.pid},
            {"generation", e.generation},
            {"started_ms", e.startedMs},
            {"running", alive}
        };
        if (e.handle && !alive) {
            DWORD code = 0;
            if (GetExitCodeProcess(e.handle, &code)) data["exit_code"] = code;
        }
        return data;
    }
    std::mutex mutex_;
    std::map<std::string, Entry> entries_;
};

} // namespace cortex::launch
