#pragma once

// Runs an external analysis engine (Ghidra, ILSpy, ...) as a separate
// process: nothing of the engine is linked into Cortex. The child and every
// process it starts (Ghidra's launcher starts a JVM) are killed together on
// timeout or cancellation: a Windows job object, or a POSIX process group.

#include <atomic>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace cortex::providers {

struct ProcessSpec {
    // argv[0] is the executable. On Windows a .bat/.cmd goes through cmd.exe;
    // its arguments must then be free of characters cmd.exe expands.
    std::vector<std::string> argv;
    std::string workingDirectory;
    std::vector<std::pair<std::string, std::string>> environment;  // added or replaced
    std::chrono::milliseconds timeout{std::chrono::minutes(15)};
    size_t outputLimit = 1u << 20;  // stdout+stderr kept (the tail beyond is dropped)
    // When set, stdout and stderr go to this file instead of a pipe (and
    // ProcessOutcome::output stays empty). Some tools behave differently
    // when their output is a pipe.
    std::string outputFile;
};

struct ProcessOutcome {
    bool started = false;
    bool timedOut = false;
    bool cancelled = false;
    int exitCode = -1;
    std::string output;  // stdout and stderr, interleaved
    bool outputTruncated = false;
    std::string error;   // why the process could not start
    double milliseconds = 0;
};

// Blocks until the process exits, the timeout passes or `*cancel` becomes
// true (polled every 50 ms).
ProcessOutcome RunProcess(const ProcessSpec& spec, const std::atomic<bool>* cancel = nullptr);

// A child process the caller talks to over its stdin/stdout (a debug
// adapter in stdio mode). stderr goes to spec.outputFile, or is discarded.
// The child and its descendants are killed on Kill() or destruction (job
// object / process group).
class ChildProcess {
public:
    ChildProcess() = default;
    ~ChildProcess();
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    bool Start(const ProcessSpec& spec, std::string& error);
    bool Write(const std::string& bytes);  // thread-safe with Read
    // Up to `max` bytes of its stdout; 0 on timeout, -1 when it is closed.
    int Read(char* buffer, size_t max, int timeoutMs);
    void Kill();
    bool Running() const;
    unsigned long Pid() const { return pid_; }

private:
    unsigned long pid_ = 0;
#if defined(_WIN32)
    void* process_ = nullptr;
    void* job_ = nullptr;
    void* stdinWrite_ = nullptr;
    void* stdoutRead_ = nullptr;
#else
    int stdinWrite_ = -1;
    int stdoutRead_ = -1;
#endif
};

// The first match for `name` in PATH (with PATHEXT on Windows), or "".
std::string FindOnPath(const std::string& name);

// Windows command-line quoting (CommandLineToArgvW rules). Exposed for tests.
std::string QuoteArgument(const std::string& value);

// True when `value` can be passed through cmd.exe and a batch file
// unchanged: no quote, %, !, &, |, <, >, ^ or line break.
bool SafeForCmd(const std::string& value);

} // namespace cortex::providers
