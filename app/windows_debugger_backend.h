#pragma once

#include "target/session_manager.h"
#include "target/thread_provider.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class WindowsDebuggerBackend final {
public:
    struct BreakpointInfo {
        int id = -1;
        std::string kind;
        uint64_t address = 0;
        int size = 1;
        bool pauseOnHit = true;
        uint64_t hitCount = 0;
        bool processGlobal = true;
        uint64_t targetThreadId = 0;
        size_t appliedThreads = 0;
        size_t totalThreads = 0;
    };

    struct BreakpointLogEntry {
        uint64_t seq = 0;
        uint64_t threadId = 0;
        uint64_t timestampMs = 0;
        uint64_t instruction = 0;
        cortex::target::ThreadRegisterSnapshot registers;
    };

    struct PausedThread {
        uint64_t threadId = 0;
        int breakpointId = -1;
        cortex::target::ThreadRegisterSnapshot registers;
    };

    explicit WindowsDebuggerBackend(cortex::target::SessionManager& sessions);
    ~WindowsDebuggerBackend();

    WindowsDebuggerBackend(const WindowsDebuggerBackend&) = delete;
    WindowsDebuggerBackend& operator=(const WindowsDebuggerBackend&) = delete;

    bool ensureAttached(std::string* error = nullptr);
    void detach();
    bool attached() const;

    bool resolveAddress(const std::string& expression, uint64_t& address, std::string* error = nullptr) const;

    int addBreakpoint(const std::string& kind,
                      uint64_t address,
                      int size,
                      bool pauseOnHit,
                      bool processGlobal,
                      uint64_t threadId,
                      std::string* error = nullptr);
    bool removeBreakpoint(int id, std::string* error = nullptr);
    std::vector<BreakpointInfo> breakpoints() const;
    std::vector<BreakpointLogEntry> breakpointLog(int id, uint64_t sinceSeq, size_t limit) const;
    std::vector<PausedThread> pausedThreads() const;

    bool readRegisters(uint64_t threadId,
                       cortex::target::ThreadRegisterSnapshot& snapshot,
                       std::string* error = nullptr) const;
    bool pauseThread(uint64_t threadId,
                     cortex::target::ThreadRegisterSnapshot& snapshot,
                     std::string* error = nullptr);
    bool continueThread(uint64_t threadId, std::string* error = nullptr);
    bool stepThread(uint64_t threadId,
                    uint32_t timeoutMs,
                    cortex::target::ThreadRegisterSnapshot& snapshot,
                    std::string* error = nullptr);
    bool stepOverThread(uint64_t threadId,
                        uint32_t timeoutMs,
                        cortex::target::ThreadRegisterSnapshot& snapshot,
                        std::string* error = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
