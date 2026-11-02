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

// The first match for `name` in PATH (with PATHEXT on Windows), or "".
std::string FindOnPath(const std::string& name);

// Windows command-line quoting (CommandLineToArgvW rules). Exposed for tests.
std::string QuoteArgument(const std::string& value);

// True when `value` can be passed through cmd.exe and a batch file
// unchanged: no quote, %, !, &, |, <, >, ^ or line break.
bool SafeForCmd(const std::string& value);

} // namespace cortex::providers
