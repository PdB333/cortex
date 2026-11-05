#include "process_runner.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace cortex::providers {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

void Append(ProcessOutcome& outcome, const char* data, size_t size, size_t limit) {
    outcome.output.append(data, size);
    if (outcome.output.size() > limit) {
        // Keep the tail: engines print their errors last.
        outcome.output.erase(0, outcome.output.size() - limit);
        outcome.outputTruncated = true;
    }
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

#if defined(_WIN32)
std::wstring Wide(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(size > 0 ? size : 0), L'\0');
    if (size > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

// The current environment with `changes` applied, as a CreateProcessW block.
std::wstring EnvironmentBlock(const std::vector<std::pair<std::string, std::string>>& changes) {
    std::vector<std::wstring> entries;
    if (wchar_t* block = GetEnvironmentStringsW()) {
        for (const wchar_t* at = block; *at; at += wcslen(at) + 1) entries.emplace_back(at);
        FreeEnvironmentStringsW(block);
    }
    for (const auto& [key, value] : changes) {
        const std::wstring wideKey = Wide(key);
        entries.erase(std::remove_if(entries.begin(), entries.end(),
                                     [&](const std::wstring& entry) {
                                         return entry.size() > wideKey.size() && entry[wideKey.size()] == L'=' &&
                                                _wcsnicmp(entry.c_str(), wideKey.c_str(), wideKey.size()) == 0;
                                     }),
                      entries.end());
        entries.push_back(wideKey + L"=" + Wide(value));
    }
    std::wstring out;
    for (const auto& entry : entries) out += entry + L'\0';
    out += L'\0';
    return out;
}
#endif

} // namespace

std::string QuoteArgument(const std::string& value) {
    if (!value.empty() && value.find_first_of(" \t\n\v\"") == std::string::npos) return value;
    std::string out = "\"";
    size_t slashes = 0;
    for (char c : value) {
        if (c == '\\') {
            ++slashes;
            continue;
        }
        if (c == '"') {
            out.append(slashes * 2 + 1, '\\');
            out.push_back('"');
        } else {
            out.append(slashes, '\\');
            out.push_back(c);
        }
        slashes = 0;
    }
    out.append(slashes * 2, '\\');
    out.push_back('"');
    return out;
}

bool SafeForCmd(const std::string& value) {
    // Quotes and % are interpreted by cmd.exe even inside quotes; & | < > ^
    // break batch files that expand %1 unquoted (Ghidra's launcher does).
    return value.find_first_of("\"%!&|<>^\r\n") == std::string::npos;
}

std::string FindOnPath(const std::string& name) {
    const char* path = std::getenv("PATH");
    if (!path || name.empty()) return {};
#if defined(_WIN32)
    const char separator = ';';
    std::vector<std::string> extensions{""};
    const char* pathext = std::getenv("PATHEXT");
    std::string list = pathext ? pathext : ".COM;.EXE;.BAT;.CMD";
    for (size_t start = 0; start <= list.size();) {
        size_t end = list.find(';', start);
        if (end == std::string::npos) end = list.size();
        if (end > start) extensions.push_back(list.substr(start, end - start));
        start = end + 1;
    }
#else
    const char separator = ':';
    const std::vector<std::string> extensions{""};
#endif
    const std::string text = path;
    for (size_t start = 0; start <= text.size();) {
        size_t end = text.find(separator, start);
        if (end == std::string::npos) end = text.size();
        const std::string directory = text.substr(start, end - start);
        start = end + 1;
        if (directory.empty()) continue;
        for (const auto& extension : extensions) {
            std::error_code ignored;
            const fs::path candidate = fs::u8path(directory) / fs::u8path(name + extension);
            if (fs::is_regular_file(candidate, ignored)) {
#if !defined(_WIN32)
                if (access(candidate.c_str(), X_OK) != 0) continue;
#endif
                return candidate.u8string();
            }
        }
    }
    return {};
}

ProcessOutcome RunProcess(const ProcessSpec& spec, const std::atomic<bool>* cancel) {
    ProcessOutcome outcome;
    const auto started = Clock::now();
    auto finish = [&] {
        outcome.milliseconds = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        return outcome;
    };
    if (spec.argv.empty()) {
        outcome.error = "empty_command";
        return finish();
    }
#if defined(_WIN32)
    std::string commandLine;
    std::wstring application;
    const std::string extension = Lower(fs::u8path(spec.argv[0]).extension().u8string());
    if (extension == ".bat" || extension == ".cmd") {
        for (const auto& argument : spec.argv)
            if (!SafeForCmd(argument)) {
                outcome.error = "argument_unsafe_for_cmd";
                return finish();
            }
        wchar_t system[MAX_PATH]{};
        GetSystemDirectoryW(system, MAX_PATH);
        application = std::wstring(system) + L"\\cmd.exe";
        std::string inner;
        for (const auto& argument : spec.argv) inner += (inner.empty() ? "" : " ") + ("\"" + argument + "\"");
        commandLine = "cmd.exe /d /s /c \"" + inner + "\"";
    } else {
        application = Wide(spec.argv[0]);
        for (const auto& argument : spec.argv) commandLine += (commandLine.empty() ? "" : " ") + QuoteArgument(argument);
    }

    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &security, 0)) {
        outcome.error = "pipe_failed";
        return finish();
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr);

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nul;
    HANDLE outputFile = INVALID_HANDLE_VALUE;
    if (!spec.outputFile.empty()) {
        outputFile = CreateFileW(Wide(spec.outputFile).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    startup.hStdOutput = outputFile != INVALID_HANDLE_VALUE ? outputFile : writePipe;
    startup.hStdError = outputFile != INVALID_HANDLE_VALUE ? outputFile : writePipe;
    PROCESS_INFORMATION process{};
    std::wstring mutableLine = Wide(commandLine);
    std::wstring environment = EnvironmentBlock(spec.environment);
    const std::wstring directory = Wide(spec.workingDirectory);
    const BOOL created = CreateProcessW(application.c_str(), mutableLine.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, environment.data(),
                                        directory.empty() ? nullptr : directory.c_str(), &startup, &process);
    CloseHandle(writePipe);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (outputFile != INVALID_HANDLE_VALUE) CloseHandle(outputFile);
    if (!created) {
        outcome.error = "start_failed:" + std::to_string(GetLastError());
        CloseHandle(readPipe);
        if (job) CloseHandle(job);
        return finish();
    }
    // In the job before it runs, so its children are in it too.
    if (job) AssignProcessToJobObject(job, process.hProcess);
    ResumeThread(process.hThread);
    outcome.started = true;

    // The pipe is drained on a Win32 thread (its handle allows cancelling
    // a blocked read) so a chatty engine never blocks on a full pipe.
    struct Pump {
        HANDLE pipe;
        size_t limit;
        ProcessOutcome reader;
        static DWORD WINAPI Run(void* argument) {
            auto* self = static_cast<Pump*>(argument);
            char buffer[8192];
            DWORD read = 0;
            while (ReadFile(self->pipe, buffer, sizeof(buffer), &read, nullptr) && read)
                Append(self->reader, buffer, read, self->limit);
            return 0;
        }
    };
    // Heap-allocated: a reader that cannot be stopped is abandoned with it.
    auto* pumpState = new Pump{readPipe, spec.outputLimit, {}};
    HANDLE pump = CreateThread(nullptr, 0, &Pump::Run, pumpState, 0, nullptr);
    auto kill = [&] {
        if (job) TerminateJobObject(job, 1);
        TerminateProcess(process.hProcess, 1);
    };
    Clock::time_point killed{};
    for (;;) {
        if (WaitForSingleObject(process.hProcess, 50) == WAIT_OBJECT_0) break;
        if (killed != Clock::time_point{}) {
            // A process that survives termination is abandoned, not waited
            // for forever.
            if (Clock::now() - killed > std::chrono::seconds(10)) {
                outcome.error = "process_did_not_exit";
                break;
            }
            continue;
        }
        if (cancel && cancel->load()) {
            outcome.cancelled = true;
            kill();
            killed = Clock::now();
        } else if (Clock::now() - started > spec.timeout) {
            outcome.timedOut = true;
            kill();
            killed = Clock::now();
        }
    }
    // Children that outlive the launcher would keep the pipe open.
    if (job) TerminateJobObject(job, 0);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    outcome.exitCode = static_cast<int>(code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    bool abandoned = false;
    if (pump) {
        // A process outside the job could still hold the write end; cancel
        // the read rather than block forever.
        if (WaitForSingleObject(pump, 5000) == WAIT_TIMEOUT) {
            CancelSynchronousIo(pump);
            abandoned = WaitForSingleObject(pump, 5000) == WAIT_TIMEOUT;
        }
        CloseHandle(pump);
    } else {
        Pump::Run(pumpState);
    }
    if (abandoned) {
        if (outcome.error.empty()) outcome.error = "output_pipe_stuck";
        if (job) CloseHandle(job);
        return finish();
    }
    CloseHandle(readPipe);
    if (job) CloseHandle(job);
    outcome.output = std::move(pumpState->reader.output);
    outcome.outputTruncated = pumpState->reader.outputTruncated;
    delete pumpState;
    return finish();
#else
    int pipeFds[2];
    if (pipe(pipeFds) != 0) {
        outcome.error = "pipe_failed";
        return finish();
    }
    std::vector<std::string> arguments = spec.argv;
    std::vector<char*> raw;
    for (auto& argument : arguments) raw.push_back(argument.data());
    raw.push_back(nullptr);
    const pid_t pid = fork();
    if (pid < 0) {
        close(pipeFds[0]);
        close(pipeFds[1]);
        outcome.error = "fork_failed";
        return finish();
    }
    if (pid == 0) {
        setpgid(0, 0);
        const int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0) dup2(nul, 0);
        const int out = spec.outputFile.empty() ? -1 : open(spec.outputFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        dup2(out >= 0 ? out : pipeFds[1], 1);
        dup2(out >= 0 ? out : pipeFds[1], 2);
        if (out >= 0) close(out);
        close(pipeFds[0]);
        close(pipeFds[1]);
        for (const auto& [key, value] : spec.environment) setenv(key.c_str(), value.c_str(), 1);
        if (!spec.workingDirectory.empty() && chdir(spec.workingDirectory.c_str()) != 0) _exit(126);
        execv(raw[0], raw.data());
        _exit(127);
    }
    setpgid(pid, pid);
    close(pipeFds[1]);
    outcome.started = true;
    fcntl(pipeFds[0], F_SETFL, fcntl(pipeFds[0], F_GETFL) | O_NONBLOCK);
    bool exited = false, pipeOpen = true;
    int status = 0;
    while (!exited || pipeOpen) {
        if (pipeOpen) {
            pollfd entry{pipeFds[0], POLLIN, 0};
            poll(&entry, 1, 50);
            char buffer[8192];
            for (;;) {
                const ssize_t read = ::read(pipeFds[0], buffer, sizeof(buffer));
                if (read > 0) {
                    Append(outcome, buffer, static_cast<size_t>(read), spec.outputLimit);
                    continue;
                }
                if (read == 0 || (errno != EAGAIN && errno != EINTR)) pipeOpen = false;
                break;
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!exited && waitpid(pid, &status, WNOHANG) == pid) {
            exited = true;
            // Descendants holding the pipe die with the group.
            kill(-pid, SIGKILL);
        }
        if (!exited && cancel && cancel->load() && !outcome.cancelled) {
            outcome.cancelled = true;
            kill(-pid, SIGKILL);
        } else if (!exited && !outcome.timedOut && Clock::now() - started > spec.timeout) {
            outcome.timedOut = true;
            kill(-pid, SIGKILL);
        }
    }
    close(pipeFds[0]);
    if (WIFEXITED(status)) outcome.exitCode = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) outcome.exitCode = 128 + WTERMSIG(status);
    if (outcome.exitCode == 127 && outcome.output.empty()) outcome.error = "exec_failed";
    return finish();
#endif
}

} // namespace cortex::providers
