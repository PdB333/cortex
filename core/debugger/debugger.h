#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <windows.h>

namespace dbg {

enum class BpKind { Software, HwExecute, HwWrite, HwReadWrite };
enum class BpAction { Pause, Log };

// Filters whether a breakpoint hit counts as a "real" hit (action taken,
// hitCount incremented) or is silently ignored -- lets a hot breakpoint
// only fire when e.g. `eax == 0` instead of on every single call.
struct BpCondition {
    std::string expression;         // optional C-like expression; takes precedence over legacy fields
    std::string source = "register"; // "register" or "memory"
    std::string reg;                 // register name for source=="register", e.g. "eax"/"rax"
    uintptr_t address = 0;           // source=="memory" only
    int size = 4;                    // source=="memory" only: 1, 2, 4 or 8
    std::string op = "==";           // "==", "!=", "<", ">", "<=", ">="
    int64_t value = 0;
};

struct Registers {
#ifdef _WIN64
    uint64_t rax = 0, rbx = 0, rcx = 0, rdx = 0;
    uint64_t rsi = 0, rdi = 0, rbp = 0, rsp = 0;
    uint64_t r8 = 0, r9 = 0, r10 = 0, r11 = 0, r12 = 0, r13 = 0, r14 = 0, r15 = 0;
    uint64_t rip = 0;
    uint32_t eflags = 0;
#else
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    uint32_t esi = 0, edi = 0, ebp = 0, esp = 0;
    uint32_t eip = 0, eflags = 0;
#endif
};

struct BreakpointInfo {
    int id;
    BpKind kind;
    uintptr_t address;
    int size;       // hw bp only: 1,2,4
    BpAction action;
    uint64_t hitCount;
    bool hasCondition;
};

struct PausedThread {
    DWORD threadId;
    int bpId;
    Registers regs;
};

// One captured hit of a BpAction::Log breakpoint (Pause breakpoints don't use
// this -- their single current state is available via ListPausedThreads).
struct BpLogEntry {
    uint64_t seq;         // mirrors hitCount at the time of capture
    DWORD threadId;
    uint64_t timestampMs; // GetTickCount64() at capture time
    Registers regs;
    uintptr_t instruction;
    std::vector<uint8_t> bytes;
    std::vector<uintptr_t> stack;
};

struct TraceConfig {
    DWORD threadId = 0;
    uintptr_t rangeStart = 0;
    uintptr_t rangeEnd = 0; // exclusive; 0/0 records every instruction
    uintptr_t stopAddress = 0;
    uint64_t maxSteps = 100000;
    size_t maxEvents = 50000;
    bool stopWhenLeavingRange = false;
};

struct TraceEvent {
    uint64_t seq;
    DWORD threadId;
    uint64_t timestampMs;
    uintptr_t instruction;
    Registers regs;
    std::vector<uint8_t> bytes;
};

struct TraceInfo {
    int id;
    DWORD threadId;
    bool active;
    std::string stopReason;
    uint64_t steps;
    size_t eventCount;
    bool truncated;
};

void Init();
// Returns false only if a paused target thread could not be released within
// the bounded shutdown window; callers must not unload the DLL in that case.
bool Shutdown();

// Software: patches a 0xCC at `address`. Hardware: uses DR0-3/DR7 on every
// thread of this process (size must be 1, 2 or 4; ignored for HwExecute which
// is always 1). `condition`, if non-null, is copied and evaluated on every
// hit; the hit is only counted/logged/paused when it passes.
int AddBreakpoint(BpKind kind, uintptr_t address, int size, BpAction action, const BpCondition* condition = nullptr);
bool RemoveBreakpoint(int id);
std::vector<BreakpointInfo> ListBreakpoints();

// Threads currently frozen inside our VEH handler waiting for /debug/continue
// or /debug/step (only populated for BpAction::Pause hits).
std::vector<PausedThread> ListPausedThreads();
bool GetPausedRegisters(DWORD threadId, Registers& out);

// Captured register snapshots for a BpAction::Log breakpoint, oldest first,
// capped at a fixed ring size (oldest entries silently dropped once full).
std::vector<BpLogEntry> GetBreakpointLog(int id);

// Resumes a paused thread normally.
bool ContinueThread(DWORD threadId);
// Resumes a paused thread for exactly one instruction, then re-freezes it;
// blocks the calling (API) thread until the step completes or times out.
bool StepThread(DWORD threadId, DWORD timeoutMs, Registers& outRegs);

// Live registers/stack for any thread of the process, paused or not (via
// Suspend/GetThreadContext/Resume) -- useful for a quick look without
// setting a breakpoint.
bool ReadThreadRegisters(DWORD threadId, Registers& out);
std::vector<uintptr_t> WalkStack(DWORD threadId, int maxFrames);
std::vector<DWORD> ListThreadIds();

// Conditional single-step recorder. It keeps a bounded instruction/register
// ring and coverage counters without pausing the game on every instruction.
int StartTrace(const TraceConfig& config, std::string& error);
bool StopTrace(int id, const std::string& reason = "requested");
std::vector<TraceInfo> ListTraces();
bool GetTraceEvents(int id, size_t offset, size_t limit, std::vector<TraceEvent>& out, size_t& total);
bool GetTraceCoverage(int id, std::vector<std::pair<uintptr_t, uint64_t>>& out);
bool RemoveTrace(int id);

} // namespace dbg
