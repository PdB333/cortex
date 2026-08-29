#include "debugger.h"
#include "../memory/memory.h"

#include <tlhelp32.h>
#include <dbghelp.h>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <atomic>
#include <thread>
#include <set>
#include <chrono>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <iomanip>
#include <stdexcept>

namespace dbg {

namespace {

constexpr DWORD kTF = 0x100;
constexpr DWORD kRF = 0x10000;

Registers CtxToRegs(PCONTEXT ctx) {
    Registers r;
#ifdef _WIN64
    r.rax = ctx->Rax; r.rbx = ctx->Rbx; r.rcx = ctx->Rcx; r.rdx = ctx->Rdx;
    r.rsi = ctx->Rsi; r.rdi = ctx->Rdi; r.rbp = ctx->Rbp; r.rsp = ctx->Rsp;
    r.r8 = ctx->R8; r.r9 = ctx->R9; r.r10 = ctx->R10; r.r11 = ctx->R11;
    r.r12 = ctx->R12; r.r13 = ctx->R13; r.r14 = ctx->R14; r.r15 = ctx->R15;
    r.rip = ctx->Rip; r.eflags = ctx->EFlags;
#else
    r.eax = ctx->Eax; r.ebx = ctx->Ebx; r.ecx = ctx->Ecx; r.edx = ctx->Edx;
    r.esi = ctx->Esi; r.edi = ctx->Edi; r.ebp = ctx->Ebp; r.esp = ctx->Esp;
    r.eip = ctx->Eip; r.eflags = ctx->EFlags;
#endif
    return r;
}

// The instruction pointer field is named differently per architecture
// (Eip vs Rip) but is always pointer-sized -- read/write it generically.
uintptr_t GetCtxIp(PCONTEXT ctx) {
#ifdef _WIN64
    return ctx->Rip;
#else
    return ctx->Eip;
#endif
}
void SetCtxIp(PCONTEXT ctx, uintptr_t v) {
#ifdef _WIN64
    ctx->Rip = v;
#else
    ctx->Eip = static_cast<DWORD>(v);
#endif
}

struct BpTriggerState {
    BpTrigger cfg;
    bool fired = false;
};

struct SwEntry {
    uintptr_t address;
    uint8_t origByte;
    BpAction action;
    uint64_t hitCount;
    std::optional<BpCondition> condition;
    std::vector<BpCapture> captures;
    std::optional<BpTriggerState> trigger;
};

struct HwEntry {
    uintptr_t address;
    int size;
    BpKind kind;
    int slot;
    BpAction action;
    uint64_t hitCount;
    std::optional<BpCondition> condition;
    std::vector<BpCapture> captures;
    std::optional<BpTriggerState> trigger;
};

// State for a single thread of the *target* process while it is (or was)
// frozen inside our VEH handler.
struct ThreadCtl {
    HANDLE resumeEvent = nullptr;  // signaled by ContinueThread/StepThread to release the frozen thread
    HANDLE doneEvent = nullptr;    // signaled by the frozen thread once a requested step has landed
    PCONTEXT ctx = nullptr;        // only valid while frozen==true (points into the VEH's stack frame)
    bool frozen = false;
    bool stepArmed = false;
    int pausedBpId = -1;
    Registers regs;
};

std::mutex g_mutex;
int g_nextBpId = 1;
std::map<int, SwEntry> g_swBps;
// Removed software breakpoints remain as address/original-byte tombstones so
// an INT3 exception that was already in flight at removal time is still
// recognized and resumed on the original instruction without being re-armed.
struct RetiredSwEntry {
    uint8_t origByte = 0;
    ULONGLONG expiresAt = 0;
};
std::map<uintptr_t, RetiredSwEntry> g_retiredSwBps;
std::map<int, HwEntry> g_hwBps;
bool g_hwSlotUsed[4] = {false, false, false, false};
std::map<DWORD, ThreadCtl> g_threadCtl;
std::map<DWORD, uintptr_t> g_pendingSwRestore;
PVOID g_vehHandle = nullptr;
std::atomic<bool> g_hwMonitorRunning{false};
std::thread g_hwMonitorThread;
std::set<DWORD> g_hwConfiguredThreads;

void PruneRetiredSoftwareBreakpoints() {
    const ULONGLONG now = GetTickCount64();
    for (auto it = g_retiredSwBps.begin(); it != g_retiredSwBps.end();) {
        if (it->second.expiresAt <= now) it = g_retiredSwBps.erase(it);
        else ++it;
    }
}

void RetireSoftwareBreakpoint(uintptr_t address, uint8_t origByte) {
    PruneRetiredSoftwareBreakpoints();
    g_retiredSwBps[address] = RetiredSwEntry{origByte, GetTickCount64() + 2000};
}

// Trigger -> auto-trace dispatcher.
struct TriggerReq { DWORD tid; TraceConfig cfg; };
std::mutex g_trigMutex;
std::deque<TriggerReq> g_trigQueue;
std::atomic<bool> g_trigRunning{false};
std::thread g_trigThread;

// Per-breakpoint ring buffer for BpAction::Log hits. Capped so a hot
// breakpoint (hit thousands of times/sec) can't grow this unbounded --
// callers polling GET /debug/breakpoint/{id}/log just see the most recent
// window. Must be accessed with g_mutex held.
constexpr size_t kMaxBpLogEntries = 500;
struct BpLogRing {
    std::deque<BpLogEntry> entries;
    uint64_t dropped = 0;   // # of entries evicted from the ring since creation
    uint64_t total   = 0;   // # of entries ever pushed (== hitCount for logs)
};
std::map<int, BpLogRing> g_bpLogs;

struct TraceState {
    TraceConfig config;
    bool active = true;
    bool enteredRange = false;
    bool truncated = false;
    uint64_t steps = 0;
    std::string stopReason;
    std::vector<TraceEvent> events;
    std::unordered_map<uintptr_t, uint64_t> coverage;
};
std::map<int, TraceState> g_traces;
int g_nextTraceId = 1;

static bool IsExecAddr(uintptr_t a) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQueryEx(GetCurrentProcess(), (LPCVOID)a, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    constexpr DWORD kExec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & kExec) != 0;
}

static std::atomic<bool> g_symInitTried{false};
static bool EnsureSymInit() {
    static bool ok = false;
    bool expected = false;
    if (g_symInitTried.compare_exchange_strong(expected, true)) {
        SymSetOptions(SymGetOptions() | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
        ok = SymInitialize(GetCurrentProcess(), nullptr, TRUE) != 0;
    }
    return ok;
}

// StackWalk64 needs a mutable copy of CONTEXT. Returns up to maxFrames IPs
// (first entry is the current IP). Empty if StackWalk64 can't advance.
static std::vector<uintptr_t> StackWalkFromContext(CONTEXT ctx, HANDLE thread, int maxFrames) {
    std::vector<uintptr_t> out;
    if (!EnsureSymInit()) return out;
    STACKFRAME64 sf{};
    DWORD machine;
#ifdef _WIN64
    machine = IMAGE_FILE_MACHINE_AMD64;
    sf.AddrPC.Offset    = ctx.Rip;
    sf.AddrFrame.Offset = ctx.Rbp;
    sf.AddrStack.Offset = ctx.Rsp;
#else
    machine = IMAGE_FILE_MACHINE_I386;
    sf.AddrPC.Offset    = ctx.Eip;
    sf.AddrFrame.Offset = ctx.Ebp;
    sf.AddrStack.Offset = ctx.Esp;
#endif
    sf.AddrPC.Mode = sf.AddrFrame.Mode = sf.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < maxFrames; ++i) {
        if (!StackWalk64(machine, GetCurrentProcess(), thread, &sf, &ctx,
                         nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
        if (!sf.AddrPC.Offset) break;
        out.push_back((uintptr_t)sf.AddrPC.Offset);
    }
    return out;
}

// Heuristic fallback: when frame-pointer chain is broken (FPO/omit-fp code)
// and StackWalk64 lacks unwind info, scan the stack slots above SP and keep
// values that look like return addresses -- i.e. sit inside a committed
// executable page. False positives are possible; ordering matches stack
// depth reasonably in practice.
static std::vector<uintptr_t> HeuristicStackScan(uintptr_t sp, int maxFrames) {
    std::vector<uintptr_t> out;
    constexpr size_t kPtrSize = sizeof(void*);
    // Sweep at most 512 slots up the stack.
    for (size_t off = 0; off < 512 * kPtrSize && (int)out.size() < maxFrames; off += kPtrSize) {
        std::vector<uint8_t> b;
        if (!memory::ReadBytes(sp + off, kPtrSize, b) || b.size() != kPtrSize) break;
        uintptr_t v = 0; std::memcpy(&v, b.data(), kPtrSize);
        if (v && IsExecAddr(v)) out.push_back(v);
    }
    return out;
}

std::vector<uintptr_t> WalkContextStack(PCONTEXT ctx, int maxFrames) {
    std::vector<uintptr_t> frames{GetCtxIp(ctx)};
#ifdef _WIN64
    uintptr_t frame = ctx->Rbp, sp = ctx->Rsp; constexpr size_t ptrSize = 8;
#else
    uintptr_t frame = ctx->Ebp, sp = ctx->Esp; constexpr size_t ptrSize = 4;
#endif
    // Try classical EBP chain first (cheap, correct when frame pointers exist).
    for (int i = 1; i < maxFrames && frame; ++i) {
        std::vector<uint8_t> b;
        if (!memory::ReadBytes(frame, ptrSize * 2, b)) break;
        uintptr_t next = 0, ret = 0;
        std::memcpy(&next, b.data(), ptrSize);
        std::memcpy(&ret, b.data() + ptrSize, ptrSize);
        if (!ret || next <= frame) break;
        frames.push_back(ret);
        frame = next;
    }
    if ((int)frames.size() >= 2) return frames;

    // EBP walk yielded nothing beyond IP -- try StackWalk64 (works with PDBs
    // or PE unwind info on x64) then a heuristic stack-return-address scan.
    auto sw = StackWalkFromContext(*ctx, GetCurrentThread(), maxFrames);
    if (sw.size() > frames.size()) return sw;
    auto heur = HeuristicStackScan(sp, maxFrames - 1);
    frames.insert(frames.end(), heur.begin(), heur.end());
    return frames;
}

// ---- Capture expression evaluator ---------------------------------------
// Grammar: expr = term (('+'|'-') term)* ; term = reg | int | '[' expr ']'.
// Case-insensitive registers, uses the hitting thread's CONTEXT.
struct ExprEval {
    const std::string& s; PCONTEXT ctx; size_t p = 0; bool err = false;
    ExprEval(const std::string& s_, PCONTEXT c) : s(s_), ctx(c) {}
    void skip() { while (p < s.size() && std::isspace((unsigned char)s[p])) ++p; }
    uint64_t regVal(std::string n) {
        for (auto& ch : n) ch = (char)std::tolower((unsigned char)ch);
#ifdef _WIN64
        if (n=="rax") return ctx->Rax; if (n=="rbx") return ctx->Rbx;
        if (n=="rcx") return ctx->Rcx; if (n=="rdx") return ctx->Rdx;
        if (n=="rsi") return ctx->Rsi; if (n=="rdi") return ctx->Rdi;
        if (n=="rbp") return ctx->Rbp; if (n=="rsp") return ctx->Rsp;
        if (n=="r8")  return ctx->R8;  if (n=="r9")  return ctx->R9;
        if (n=="r10") return ctx->R10; if (n=="r11") return ctx->R11;
        if (n=="r12") return ctx->R12; if (n=="r13") return ctx->R13;
        if (n=="r14") return ctx->R14; if (n=="r15") return ctx->R15;
        if (n=="rip") return ctx->Rip;
        // Common aliases so a caller writing "eax" on x64 gets the low dword.
        if (n=="eax") return (uint32_t)ctx->Rax; if (n=="ebx") return (uint32_t)ctx->Rbx;
        if (n=="ecx") return (uint32_t)ctx->Rcx; if (n=="edx") return (uint32_t)ctx->Rdx;
        if (n=="esi") return (uint32_t)ctx->Rsi; if (n=="edi") return (uint32_t)ctx->Rdi;
        if (n=="ebp") return (uint32_t)ctx->Rbp; if (n=="esp") return (uint32_t)ctx->Rsp;
#else
        if (n=="eax") return ctx->Eax; if (n=="ebx") return ctx->Ebx;
        if (n=="ecx") return ctx->Ecx; if (n=="edx") return ctx->Edx;
        if (n=="esi") return ctx->Esi; if (n=="edi") return ctx->Edi;
        if (n=="ebp") return ctx->Ebp; if (n=="esp") return ctx->Esp;
        if (n=="eip") return ctx->Eip;
#endif
        err = true; return 0;
    }
    uint64_t term() {
        skip(); if (p >= s.size()) { err = true; return 0; }
        if (s[p] == '[') {
            ++p; uint64_t inner = expr(); skip();
            if (p >= s.size() || s[p] != ']') { err = true; return 0; }
            ++p;
            // Dereference pointer-sized.
            std::vector<uint8_t> buf;
            if (!memory::ReadBytes((uintptr_t)inner, sizeof(void*), buf) || buf.size() != sizeof(void*)) {
                err = true; return 0;
            }
            uintptr_t v = 0; std::memcpy(&v, buf.data(), sizeof(void*));
            return (uint64_t)v;
        }
        if (std::isalpha((unsigned char)s[p]) || s[p] == '_') {
            std::string name;
            while (p < s.size() && (std::isalnum((unsigned char)s[p]) || s[p] == '_')) name += s[p++];
            return regVal(name);
        }
        if (std::isdigit((unsigned char)s[p]) ||
            (s[p] == '-' && p + 1 < s.size() && std::isdigit((unsigned char)s[p+1]))) {
            size_t consumed = 0;
            try {
                bool neg = false; if (s[p] == '-') { neg = true; ++p; }
                uint64_t v = std::stoull(s.substr(p), &consumed, 0);
                p += consumed;
                return neg ? (uint64_t)-(int64_t)v : v;
            } catch (...) { err = true; return 0; }
        }
        err = true; return 0;
    }
    uint64_t expr() {
        uint64_t v = term();
        while (!err) {
            skip(); if (p >= s.size()) break;
            char op = s[p]; if (op != '+' && op != '-') break;
            ++p; uint64_t rhs = term();
            v = (op == '+') ? v + rhs : v - rhs;
        }
        return v;
    }
};

static uint64_t EvalExpression(const std::string& s, PCONTEXT ctx, bool& ok) {
    ExprEval e(s, ctx); uint64_t v = e.expr(); ok = !e.err; return v;
}

static std::string DecodeCapture(const std::string& type, const std::vector<uint8_t>& b) {
    auto sz = b.size();
    auto asHex = [&]{ std::ostringstream o; o << std::hex << std::setfill('0');
                      for (uint8_t x : b) o << std::setw(2) << (int)x; return o.str(); };
    if (type == "bytes") return "";
    if (type == "cstring") {
        std::string s; for (uint8_t c : b) { if (!c) break; s.push_back((char)c); }
        return s;
    }
    auto ok = [&](size_t n){ return sz >= n; };
    #define GET(T) T v{}; std::memcpy(&v, b.data(), sizeof(T)); return std::to_string(v)
    if (type == "u8"  && ok(1)) { GET(uint8_t); }
    if (type == "i8"  && ok(1)) { GET(int8_t); }
    if (type == "u16" && ok(2)) { GET(uint16_t); }
    if (type == "i16" && ok(2)) { GET(int16_t); }
    if (type == "u32" && ok(4)) { GET(uint32_t); }
    if (type == "i32" && ok(4)) { GET(int32_t); }
    if (type == "u64" && ok(8)) { GET(uint64_t); }
    if (type == "i64" && ok(8)) { GET(int64_t); }
    if (type == "float"  && ok(4)) { float f{}; std::memcpy(&f, b.data(), 4); std::ostringstream o; o << f; return o.str(); }
    if (type == "double" && ok(8)) { double d{}; std::memcpy(&d, b.data(), 8); std::ostringstream o; o << d; return o.str(); }
    #undef GET
    return {};
}

static std::vector<BpCaptureResult> RunCaptures(const std::vector<BpCapture>& caps, PCONTEXT ctx) {
    std::vector<BpCaptureResult> out;
    out.reserve(caps.size());
    for (const auto& c : caps) {
        BpCaptureResult r; r.name = c.name;
        bool ok = false;
        uint64_t addr = EvalExpression(c.expression, ctx, ok);
        r.address = (uintptr_t)addr;
        if (!ok || !addr) { out.push_back(std::move(r)); continue; }
        int sz = c.size > 0 ? c.size : 16;
        if (sz > 4096) sz = 4096;
        if (memory::ReadBytes(r.address, sz, r.bytes)) {
            r.ok = true;
            r.decoded = DecodeCapture(c.type, r.bytes);
            // cstring may finish early -- truncate buf to actual length.
            if (c.type == "cstring") {
                size_t n = 0; while (n < r.bytes.size() && r.bytes[n]) ++n;
                r.bytes.resize(n);
            }
        }
        out.push_back(std::move(r));
    }
    return out;
}
// ---- end evaluator ------------------------------------------------------

// Reads the caller return address from [sp] using the hitting CONTEXT.
static uintptr_t ReadReturnAddress(PCONTEXT ctx) {
#ifdef _WIN64
    uintptr_t sp = ctx->Rsp;
#else
    uintptr_t sp = ctx->Esp;
#endif
    std::vector<uint8_t> b;
    if (!memory::ReadBytes(sp, sizeof(void*), b) || b.size() != sizeof(void*)) return 0;
    uintptr_t v = 0; std::memcpy(&v, b.data(), sizeof(void*));
    return v;
}

// Called under g_mutex from PushLogEntry. Enqueues a StartTrace request to
// be picked up by the trigger worker thread -- must not call StartTrace
// synchronously (it grabs g_mutex + SuspendThread and would deadlock).
static void MaybeEnqueueTrigger(int id, DWORD tid, PCONTEXT ctx) {
    auto handle = [&](std::optional<BpTriggerState>& t) {
        if (!t || (t->fired && t->cfg.once)) return;
        TriggerReq req;
        req.tid = tid;
        req.cfg = t->cfg.templateCfg;
        if (t->cfg.stopOnReturn) req.cfg.stopAddress = ReadReturnAddress(ctx);
        req.cfg.threadId = tid;
        t->fired = true;
        std::lock_guard<std::mutex> lock(g_trigMutex);
        g_trigQueue.push_back(std::move(req));
    };
    if (auto sw = g_swBps.find(id); sw != g_swBps.end()) handle(sw->second.trigger);
    else if (auto hw = g_hwBps.find(id); hw != g_hwBps.end()) handle(hw->second.trigger);
}

void PushLogEntry(int id, DWORD tid, uint64_t seq, PCONTEXT ctx) {
    auto& ring = g_bpLogs[id];
    std::vector<uint8_t> bytes; memory::ReadBytes(GetCtxIp(ctx),16,bytes);
    // Fetch captures configured for this bp id.
    std::vector<BpCapture> caps;
    if (auto sw = g_swBps.find(id); sw != g_swBps.end()) caps = sw->second.captures;
    else if (auto hw = g_hwBps.find(id); hw != g_hwBps.end()) caps = hw->second.captures;
    auto captureResults = caps.empty() ? std::vector<BpCaptureResult>{} : RunCaptures(caps, ctx);
    MaybeEnqueueTrigger(id, tid, ctx);
    ring.entries.push_back(BpLogEntry{seq, tid, GetTickCount64(), CtxToRegs(ctx), GetCtxIp(ctx),
                                       std::move(bytes), WalkContextStack(ctx,16),
                                       std::move(captureResults)});
    ring.total = seq;
    if (ring.entries.size() > kMaxBpLogEntries) {
        ring.entries.pop_front();
        ring.dropped++;
    }
}

bool HandleTraceStep(DWORD tid, PCONTEXT ctx, bool& handled) {
    const uintptr_t ip = GetCtxIp(ctx);
    bool keepTracing = false;
    for (auto& item : g_traces) {
        TraceState& trace = item.second;
        if (!trace.active || trace.config.threadId != tid) continue;
        handled = true;
        trace.steps++;
        const bool hasRange = trace.config.rangeStart != 0 || trace.config.rangeEnd != 0;
        const bool inRange = !hasRange ||
            (ip >= trace.config.rangeStart && (trace.config.rangeEnd == 0 || ip < trace.config.rangeEnd));
        if (inRange) {
            trace.enteredRange = true;
            trace.coverage[ip]++;
            if (trace.events.size() < trace.config.maxEvents) {
                std::vector<uint8_t> bytes;
                memory::ReadBytes(ip, 16, bytes);
                trace.events.push_back(TraceEvent{trace.events.size() + 1, tid, GetTickCount64(), ip,
                                                  CtxToRegs(ctx), std::move(bytes)});
            } else {
                trace.truncated = true;
            }
        }
        if (trace.config.stopAddress && ip == trace.config.stopAddress) {
            trace.active = false; trace.stopReason = "stop_address";
        } else if (trace.steps >= trace.config.maxSteps) {
            trace.active = false; trace.stopReason = "max_steps";
        } else if (trace.config.stopWhenLeavingRange && trace.enteredRange && !inRange) {
            trace.active = false; trace.stopReason = "left_range";
        }
        keepTracing = keepTracing || trace.active;
    }
    return keepTracing;
}

ThreadCtl& GetOrCreateThreadCtl(DWORD tid) {
    auto it = g_threadCtl.find(tid);
    if (it != g_threadCtl.end()) return it->second;
    ThreadCtl tc;
    tc.resumeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    tc.doneEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    return g_threadCtl.emplace(tid, tc).first->second;
}

// Blocks the calling (target-process) thread until ContinueThread/StepThread
// releases it. Must be called with g_mutex NOT held.
void FreezeCurrentThread(DWORD tid, int bpId, PCONTEXT ctx, bool signalDone) {
    HANDLE resumeEv, doneEv;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ThreadCtl& tc = GetOrCreateThreadCtl(tid);
        tc.ctx = ctx;
        tc.frozen = true;
        tc.pausedBpId = bpId;
        tc.regs = CtxToRegs(ctx);
        ResetEvent(tc.resumeEvent);
        resumeEv = tc.resumeEvent;
        doneEv = tc.doneEvent;
    }
    if (signalDone) SetEvent(doneEv);
    WaitForSingleObject(resumeEv, INFINITE);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_threadCtl.find(tid);
        if (it != g_threadCtl.end()) it->second.frozen = false;
    }
}

int64_t GetRegisterValue(const std::string& name, PCONTEXT ctx) {
#ifdef _WIN64
    if (name == "rax") return static_cast<int64_t>(ctx->Rax);
    if (name == "rbx") return static_cast<int64_t>(ctx->Rbx);
    if (name == "rcx") return static_cast<int64_t>(ctx->Rcx);
    if (name == "rdx") return static_cast<int64_t>(ctx->Rdx);
    if (name == "rsi") return static_cast<int64_t>(ctx->Rsi);
    if (name == "rdi") return static_cast<int64_t>(ctx->Rdi);
    if (name == "rbp") return static_cast<int64_t>(ctx->Rbp);
    if (name == "rsp") return static_cast<int64_t>(ctx->Rsp);
    if (name == "r8") return static_cast<int64_t>(ctx->R8);
    if (name == "r9") return static_cast<int64_t>(ctx->R9);
    if (name == "r10") return static_cast<int64_t>(ctx->R10);
    if (name == "r11") return static_cast<int64_t>(ctx->R11);
    if (name == "r12") return static_cast<int64_t>(ctx->R12);
    if (name == "r13") return static_cast<int64_t>(ctx->R13);
    if (name == "r14") return static_cast<int64_t>(ctx->R14);
    if (name == "r15") return static_cast<int64_t>(ctx->R15);
    if (name == "rip") return static_cast<int64_t>(ctx->Rip);
#else
    if (name == "eax") return static_cast<int64_t>(static_cast<int32_t>(ctx->Eax));
    if (name == "ebx") return static_cast<int64_t>(static_cast<int32_t>(ctx->Ebx));
    if (name == "ecx") return static_cast<int64_t>(static_cast<int32_t>(ctx->Ecx));
    if (name == "edx") return static_cast<int64_t>(static_cast<int32_t>(ctx->Edx));
    if (name == "esi") return static_cast<int64_t>(static_cast<int32_t>(ctx->Esi));
    if (name == "edi") return static_cast<int64_t>(static_cast<int32_t>(ctx->Edi));
    if (name == "ebp") return static_cast<int64_t>(static_cast<int32_t>(ctx->Ebp));
    if (name == "esp") return static_cast<int64_t>(static_cast<int32_t>(ctx->Esp));
    if (name == "eip") return static_cast<int64_t>(static_cast<int32_t>(ctx->Eip));
#endif
    return 0;
}

bool CompareOp(int64_t a, const std::string& op, int64_t b) {
    if (op == "==") return a == b;
    if (op == "!=") return a != b;
    if (op == "<") return a < b;
    if (op == ">") return a > b;
    if (op == "<=") return a <= b;
    if (op == ">=") return a >= b;
    return true;
}

class ExpressionParser {
public:
    ExpressionParser(const std::string& source,PCONTEXT context,DWORD tid,uint64_t hit)
        : source_(source),context_(context),threadId_(tid),hitCount_(hit){}
    bool Evaluate(){pos_=0;int64_t value=ParseOr();Skip();if(pos_!=source_.size())throw std::runtime_error("unexpected token");return value!=0;}
private:
    void Skip(){while(pos_<source_.size()&&std::isspace(static_cast<unsigned char>(source_[pos_])))pos_++;}
    bool Take(const char* token){Skip();size_t n=strlen(token);if(source_.compare(pos_,n,token)==0){pos_+=n;return true;}return false;}
    int64_t ParseOr(){int64_t v=ParseAnd();while(Take("||"))v=(v!=0||ParseAnd()!=0);return v;}
    int64_t ParseAnd(){int64_t v=ParseCompare();while(Take("&&"))v=(v!=0&&ParseCompare()!=0);return v;}
    int64_t ParseCompare(){int64_t a=ParseAdd();if(Take("=="))return a==ParseAdd();if(Take("!="))return a!=ParseAdd();if(Take("<="))return a<=ParseAdd();if(Take(">="))return a>=ParseAdd();if(Take("<"))return a<ParseAdd();if(Take(">"))return a>ParseAdd();return a;}
    int64_t ParseAdd(){int64_t v=ParseUnary();for(;;){if(Take("+"))v+=ParseUnary();else if(Take("-"))v-=ParseUnary();else return v;}}
    int64_t ParseUnary(){if(Take("!"))return !ParseUnary();if(Take("-"))return-ParseUnary();return ParsePrimary();}
    int64_t ParsePrimary(){
        Skip();if(Take("(")){int64_t v=ParseOr();if(!Take(")"))throw std::runtime_error("missing )");return v;}
        if(pos_<source_.size()&&(std::isdigit(static_cast<unsigned char>(source_[pos_])))){
            size_t end=pos_;while(end<source_.size()&&(std::isalnum(static_cast<unsigned char>(source_[end]))||source_[end]=='x'||source_[end]=='X'))end++;
            std::string token=source_.substr(pos_,end-pos_);pos_=end;return std::stoll(token,nullptr,0);
        }
        size_t start=pos_;while(pos_<source_.size()&&(std::isalnum(static_cast<unsigned char>(source_[pos_]))||source_[pos_]=='_'||source_[pos_]=='.'))pos_++;
        if(start==pos_)throw std::runtime_error("expected value");std::string name=source_.substr(start,pos_-start);
        if(name=="thread.id")return threadId_;if(name=="hitcount")return static_cast<int64_t>(hitCount_);
        if(name=="mem8"||name=="mem16"||name=="mem32"||name=="mem64"){
            if(!Take("("))throw std::runtime_error("missing memory address");uintptr_t address=static_cast<uintptr_t>(ParseOr());if(!Take(")"))throw std::runtime_error("missing )");
            size_t size=name=="mem8"?1:name=="mem16"?2:name=="mem32"?4:8;std::vector<uint8_t>b;if(!memory::ReadBytes(address,size,b))return 0;int64_t value=0;memcpy(&value,b.data(),size);return value;
        }
        return GetRegisterValue(name,context_);
    }
    const std::string&source_;PCONTEXT context_;DWORD threadId_;uint64_t hitCount_;size_t pos_=0;
};

// A missing condition always passes -- keeps AddBreakpoint's default
// nullptr behaving exactly like before this feature existed.
bool EvalCondition(const std::optional<BpCondition>& cond, PCONTEXT ctx, DWORD tid, uint64_t nextHitCount) {
    if (!cond.has_value()) return true;
    if(!cond->expression.empty()){
        try{return ExpressionParser(cond->expression,ctx,tid,nextHitCount).Evaluate();}catch(...){return false;}
    }
    int64_t actual = 0;
    if (cond->source == "memory") {
        std::vector<uint8_t> buf;
        if (!memory::ReadBytes(cond->address, static_cast<size_t>(cond->size), buf)) return false;
        switch (cond->size) {
            case 1: actual = static_cast<int8_t>(buf[0]); break;
            case 2: { int16_t v; memcpy(&v, buf.data(), 2); actual = v; break; }
            case 8: { int64_t v; memcpy(&v, buf.data(), 8); actual = v; break; }
            default: { int32_t v; memcpy(&v, buf.data(), 4); actual = v; break; }
        }
    } else {
        actual = GetRegisterValue(cond->reg, ctx);
    }
    return CompareOp(actual, cond->op, cond->value);
}

LONG CALLBACK VectoredHandler(PEXCEPTION_POINTERS info) {
    DWORD code = info->ExceptionRecord->ExceptionCode;
    DWORD tid = GetCurrentThreadId();
    PCONTEXT ctx = info->ContextRecord;

    if (code == EXCEPTION_BREAKPOINT) {
        // Windows exposes the exception address and CPU context separately.
        // Depending on architecture/runtime details, the context IP can point
        // at or just past the INT3. Match only addresses Cortex actually owns
        // instead of blindly subtracting one and potentially leaking our trap.
        const uintptr_t exceptionAddress = reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress);
        const uintptr_t contextIp = GetCtxIp(ctx);
        const uintptr_t candidates[] = {
            exceptionAddress,
            contextIp > 0 ? contextIp - 1 : 0,
            contextIp,
            exceptionAddress > 0 ? exceptionAddress - 1 : 0
        };

        uintptr_t addr = 0;
        int foundId = -1;
        bool retired = false;
        SwEntry entry{};
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            PruneRetiredSoftwareBreakpoints();
            for (uintptr_t candidate : candidates) {
                if (candidate == 0) continue;
                for (const auto& [id, e] : g_swBps) {
                    if (e.address == candidate) {
                        addr = candidate;
                        foundId = id;
                        entry = e;
                        break;
                    }
                }
                if (foundId >= 0) break;
                const auto old = g_retiredSwBps.find(candidate);
                if (old != g_retiredSwBps.end()) {
                    addr = candidate;
                    entry.address = candidate;
                    entry.origByte = old->second.origByte;
                    retired = true;
                    break;
                }
            }
        }
        if (foundId < 0 && !retired) return EXCEPTION_CONTINUE_SEARCH;

        // A removed breakpoint may still have an exception already dispatched
        // on another thread. Its original byte has already been restored; just
        // rewind that thread to execute it and never re-arm the trap.
        if (retired) {
            SetCtxIp(ctx, addr);
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        // Restore before rewinding IP. If restoration fails, leave the context
        // untouched and let another debugger/handler decide how to recover.
        if (!memory::WriteBytes(addr, {entry.origByte})) return EXCEPTION_CONTINUE_SEARCH;
        SetCtxIp(ctx, addr);

        // The condition check happens after the byte restore above: that part
        // must always run so the CPU can step past the 0xCC regardless of
        // whether this particular hit "counts".
        bool condMet = EvalCondition(entry.condition, ctx, tid, entry.hitCount + 1);
        bool shouldPause = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_pendingSwRestore[tid] = addr;
            if (condMet) {
                uint64_t seq = ++g_swBps[foundId].hitCount;
                if (entry.action == BpAction::Log) PushLogEntry(foundId, tid, seq, ctx);
                shouldPause = (entry.action == BpAction::Pause);
            }
        }
        ctx->EFlags |= kTF; // single-step the original instruction, then re-arm 0xCC

        if (shouldPause) {
            FreezeCurrentThread(tid, foundId, ctx, false);
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (code == EXCEPTION_SINGLE_STEP) {
        bool traceActive = false;
        bool traceHandled = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            traceActive = HandleTraceStep(tid, ctx, traceHandled);
        }
        ULONG_PTR dr6 = static_cast<ULONG_PTR>(ctx->Dr6);
        if (dr6 & 0xF) {
            int hitId = -1;
            bool shouldPause = false;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                for (int slot = 0; slot < 4; ++slot) {
                    if (!(dr6 & (1u << slot))) continue;
                    for (auto& [id, e] : g_hwBps) {
                        if (e.slot == slot) {
                            if (EvalCondition(e.condition, ctx, tid, e.hitCount + 1)) {
                                uint64_t seq = ++e.hitCount;
                                hitId = id;
                                if (e.action == BpAction::Log) PushLogEntry(id, tid, seq, ctx);
                                shouldPause = (e.action == BpAction::Pause);
                            }
                            break;
                        }
                    }
                }
            }
            ctx->Dr6 = 0;
            if (shouldPause) {
                FreezeCurrentThread(tid, hitId, ctx, false);
            }
            // RF suppresses the debug exception for the very next instruction --
            // without it, EIP is still sitting on the same address DR7 watches,
            // so the CPU would re-trap on itself forever instead of advancing.
            ctx->EFlags |= kRF;
            if (traceActive) ctx->EFlags |= kTF;
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        uintptr_t pendingAddr = 0;
        bool hasPending = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_pendingSwRestore.find(tid);
            if (it != g_pendingSwRestore.end()) {
                pendingAddr = it->second;
                hasPending = true;
                g_pendingSwRestore.erase(it);
            }
        }
        if (hasPending) {
            // A user may remove the breakpoint while this thread is stepping
            // the original instruction. Never resurrect a deleted breakpoint.
            int activeId = -1;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                for (const auto& [id, e] : g_swBps) {
                    if (e.address == pendingAddr) { activeId = id; break; }
                }
            }
            if (activeId >= 0 && !memory::WriteBytes(pendingAddr, {0xCC})) {
                // The original byte is already back in memory, so failure to
                // re-arm is safe: disable the logical breakpoint instead of
                // pretending a trap is still installed.
                std::lock_guard<std::mutex> lock(g_mutex);
                const auto failed = g_swBps.find(activeId);
                if (failed != g_swBps.end()) {
                    RetireSoftwareBreakpoint(failed->second.address, failed->second.origByte);
                    g_swBps.erase(failed);
                }
                g_bpLogs.erase(activeId);
            }
            if (traceActive) ctx->EFlags |= kTF; else ctx->EFlags &= ~kTF;
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        bool stepArmed = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_threadCtl.find(tid);
            if (it != g_threadCtl.end() && it->second.stepArmed) {
                stepArmed = true;
                it->second.stepArmed = false;
            }
        }
        if (stepArmed) {
            FreezeCurrentThread(tid, -1, ctx, true);
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        if (traceActive) {
            ctx->EFlags |= kTF;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (traceHandled) {
            ctx->EFlags &= ~kTF;
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        return EXCEPTION_CONTINUE_SEARCH;
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

void ClearHwSlotOnAllThreads(int slot) {
    for (DWORD tid : ListThreadIds()) {
        if (tid == GetCurrentThreadId()) continue;
        HANDLE h = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, tid);
        if (!h) continue;
        SuspendThread(h);
        CONTEXT ctx = {};
        ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(h, &ctx)) {
            ctx.Dr7 &= ~(static_cast<ULONG_PTR>(1u) << (slot * 2));
            SetThreadContext(h, &ctx);
        }
        ResumeThread(h);
        CloseHandle(h);
    }
}

bool ApplyHwEntriesToThread(DWORD tid, const std::vector<HwEntry>& entries) {
    if (tid == GetCurrentThreadId() || entries.empty()) return false;
    HANDLE h = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, tid);
    if (!h) return false;
    if (SuspendThread(h) == static_cast<DWORD>(-1)) { CloseHandle(h); return false; }
    bool applied = false;
    CONTEXT ctx = {};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (GetThreadContext(h, &ctx)) {
        for (const auto& entry : entries) {
            const int slot = entry.slot;
            const ULONG_PTR rw = entry.kind == BpKind::HwExecute ? 0u :
                                 (entry.kind == BpKind::HwWrite ? 1u : 3u);
            const ULONG_PTR len = entry.size == 1 ? 0u : (entry.size == 2 ? 1u : 3u);
            (&ctx.Dr0)[slot] = static_cast<ULONG_PTR>(entry.address);
            ctx.Dr7 |= (static_cast<ULONG_PTR>(1u) << (slot * 2));
            ctx.Dr7 &= ~(static_cast<ULONG_PTR>(0x3u) << (16 + slot * 4));
            ctx.Dr7 |= (rw << (16 + slot * 4));
            ctx.Dr7 &= ~(static_cast<ULONG_PTR>(0x3u) << (18 + slot * 4));
            ctx.Dr7 |= (len << (18 + slot * 4));
        }
        applied = SetThreadContext(h, &ctx) != FALSE;
    }
    ResumeThread(h);
    CloseHandle(h);
    return applied;
}

void HwThreadMonitor() {
    while (g_hwMonitorRunning.load()) {
        const auto tids = ListThreadIds();
        std::set<DWORD> live(tids.begin(), tids.end());
        std::vector<HwEntry> entries;
        std::vector<DWORD> newThreads;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            for (auto it = g_hwConfiguredThreads.begin(); it != g_hwConfiguredThreads.end();) {
                if (live.count(*it) == 0) it = g_hwConfiguredThreads.erase(it); else ++it;
            }
            if (!g_hwBps.empty()) {
                for (const auto& [id, entry] : g_hwBps) entries.push_back(entry);
                for (DWORD tid : tids) {
                    if (tid != GetCurrentThreadId() && g_hwConfiguredThreads.count(tid) == 0)
                        newThreads.push_back(tid);
                }
            }
        }
        for (DWORD tid : newThreads) {
            if (ApplyHwEntriesToThread(tid, entries)) {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_hwConfiguredThreads.insert(tid);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

} // namespace

static void TriggerWorker() {
    while (g_trigRunning.load()) {
        TriggerReq req; bool got = false;
        {
            std::lock_guard<std::mutex> lock(g_trigMutex);
            if (!g_trigQueue.empty()) { req = std::move(g_trigQueue.front()); g_trigQueue.pop_front(); got = true; }
        }
        if (!got) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); continue; }
        std::string err;
        StartTrace(req.cfg, err);
    }
}

void Init() {
    if (g_vehHandle) return;
    g_vehHandle = AddVectoredExceptionHandler(1, VectoredHandler);
    g_hwMonitorRunning = true;
    g_hwMonitorThread = std::thread(HwThreadMonitor);
    g_trigRunning = true;
    g_trigThread = std::thread(TriggerWorker);
}

bool Shutdown() {
    g_hwMonitorRunning = false;
    if (g_hwMonitorThread.joinable()) g_hwMonitorThread.join();
    g_trigRunning = false;
    if (g_trigThread.joinable()) g_trigThread.join();

    // Release any game threads paused inside our VEH before removing the
    // handler or unloading this DLL. They must leave FreezeCurrentThread and
    // finish any pending INT3 single-step while our code is still resident.
    std::vector<HANDLE> resumeEvents;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto& [tid, ctl] : g_threadCtl) if (ctl.frozen && ctl.resumeEvent) resumeEvents.push_back(ctl.resumeEvent);
    }
    for (HANDLE event : resumeEvents) SetEvent(event);
    const ULONGLONG deadline = GetTickCount64() + 2000;
    bool settled = false;
    while (GetTickCount64() < deadline) {
        settled = true;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            for (const auto& [tid, ctl] : g_threadCtl) settled = settled && !ctl.frozen;
            settled = settled && g_pendingSwRestore.empty();
        }
        if (settled) break;
        Sleep(1);
    }
    if (!settled) return false;

    std::vector<int> activeTraceIds;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const auto& item : g_traces) if (item.second.active) activeTraceIds.push_back(item.first);
    }
    for (int id : activeTraceIds) StopTrace(id, "shutdown");

    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto& [id, e] : g_swBps) {
        memory::WriteBytes(e.address, {e.origByte});
    }
    g_swBps.clear();
    g_retiredSwBps.clear();
    for (auto& [id, e] : g_hwBps) {
        ClearHwSlotOnAllThreads(e.slot);
    }
    g_hwBps.clear();
    for (bool& used : g_hwSlotUsed) used = false;
    g_hwConfiguredThreads.clear();
    for (auto& [tid, ctl] : g_threadCtl) {
        if (ctl.resumeEvent) CloseHandle(ctl.resumeEvent);
        if (ctl.doneEvent) CloseHandle(ctl.doneEvent);
    }
    g_threadCtl.clear();
    g_pendingSwRestore.clear();
    g_traces.clear();
    if (g_vehHandle) {
        RemoveVectoredExceptionHandler(g_vehHandle);
        g_vehHandle = nullptr;
    }
    return true;
}

int AddBreakpoint(BpKind kind, uintptr_t address, int size, BpAction action,
                  const BpCondition* condition, const std::vector<BpCapture>* captures) {
    std::optional<BpCondition> cond = condition ? std::optional<BpCondition>(*condition) : std::nullopt;
    std::vector<BpCapture> caps = captures ? *captures : std::vector<BpCapture>{};

    if (kind == BpKind::Software) {
        // A software breakpoint without our VEH would turn into an unhandled
        // process breakpoint. Never patch the target unless the handler exists.
        if (!g_vehHandle || address == 0) return -1;

        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQueryEx(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)) == 0) return -1;
        constexpr DWORD kExecMask = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & kExecMask) == 0 ||
            (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return -1;

        // Serialize validation, patching and registration so there is no window
        // where an INT3 exists without a matching Cortex breakpoint record.
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const auto& [id, existing] : g_swBps)
            if (existing.address == address) return -1;

        std::vector<uint8_t> orig;
        if (!memory::ReadBytes(address, 1, orig) || orig.empty()) return -1;
        // Do not claim a pre-existing INT3: it may belong to the game or an
        // attached debugger and cannot be safely restored by Cortex.
        if (orig[0] == 0xCC) return -1;
        g_retiredSwBps.erase(address);
        if (!memory::WriteBytes(address, {0xCC})) return -1;
        int id = g_nextBpId++;
        g_swBps[id] = SwEntry{address, orig[0], action, 0, cond, caps};
        return id;
    }

    // Hardware breakpoint: find a free DR0-3 slot.
    int slot = -1;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (int i = 0; i < 4; ++i) {
            if (!g_hwSlotUsed[i]) { slot = i; break; }
        }
        if (slot < 0) return -1;
        g_hwSlotUsed[slot] = true;
    }

    int effSize = (kind == BpKind::HwExecute) ? 1 : size;
    if (effSize != 1 && effSize != 2 && effSize != 4) effSize = 4;

    ULONG_PTR rw = (kind == BpKind::HwExecute) ? 0u : (kind == BpKind::HwWrite ? 1u : 3u);
    ULONG_PTR len = (effSize == 1) ? 0u : (effSize == 2 ? 1u : 3u);

    int id;
    HwEntry newEntry{address, effSize, kind, slot, action, 0, cond, caps};
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        id = g_nextBpId++;
        g_hwBps[id] = newEntry;
    }
    for (DWORD tid : ListThreadIds()) {
        if (ApplyHwEntriesToThread(tid, {newEntry})) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_hwConfiguredThreads.insert(tid);
        }
    }
    return id;
}

bool SetBreakpointTrigger(int id, const BpTrigger& trigger) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (auto sw = g_swBps.find(id); sw != g_swBps.end()) {
        sw->second.trigger = BpTriggerState{trigger, false};
        return true;
    }
    if (auto hw = g_hwBps.find(id); hw != g_hwBps.end()) {
        hw->second.trigger = BpTriggerState{trigger, false};
        return true;
    }
    return false;
}

bool ClearBreakpointTrigger(int id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (auto sw = g_swBps.find(id); sw != g_swBps.end()) { sw->second.trigger.reset(); return true; }
    if (auto hw = g_hwBps.find(id); hw != g_hwBps.end()) { hw->second.trigger.reset(); return true; }
    return false;
}

bool RemoveBreakpoint(int id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto sw = g_swBps.find(id);
    if (sw != g_swBps.end()) {
        if (!memory::WriteBytes(sw->second.address, {sw->second.origByte})) return false;
        RetireSoftwareBreakpoint(sw->second.address, sw->second.origByte);
        g_swBps.erase(sw);
        g_bpLogs.erase(id);
        return true;
    }
    auto hw = g_hwBps.find(id);
    if (hw != g_hwBps.end()) {
        int slot = hw->second.slot;
        g_hwSlotUsed[slot] = false;
        g_hwBps.erase(hw);
        ClearHwSlotOnAllThreads(slot);
        g_bpLogs.erase(id);
        return true;
    }
    return false;
}

std::vector<BreakpointInfo> ListBreakpoints() {
    std::vector<BreakpointInfo> out;
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto& [id, e] : g_swBps) {
        out.push_back(BreakpointInfo{id, BpKind::Software, e.address, 1, e.action, e.hitCount, e.condition.has_value()});
    }
    for (auto& [id, e] : g_hwBps) {
        out.push_back(BreakpointInfo{id, e.kind, e.address, e.size, e.action, e.hitCount, e.condition.has_value()});
    }
    return out;
}

std::vector<PausedThread> ListPausedThreads() {
    std::vector<PausedThread> out;
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto& [tid, tc] : g_threadCtl) {
        if (tc.frozen) out.push_back(PausedThread{tid, tc.pausedBpId, tc.regs});
    }
    return out;
}

bool GetPausedRegisters(DWORD threadId, Registers& out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_threadCtl.find(threadId);
    if (it == g_threadCtl.end() || !it->second.frozen) return false;
    out = it->second.regs;
    return true;
}

std::vector<BpLogEntry> GetBreakpointLog(int id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_bpLogs.find(id);
    if (it == g_bpLogs.end()) return {};
    return std::vector<BpLogEntry>(it->second.entries.begin(), it->second.entries.end());
}

bool GetBreakpointLogPaged(int id, uint64_t sinceSeq, size_t limit,
                           std::vector<BpLogEntry>& out,
                           uint64_t& outDropped, uint64_t& outTotal) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_bpLogs.find(id);
    if (it == g_bpLogs.end()) return false;
    outDropped = it->second.dropped;
    outTotal   = it->second.total;
    out.clear();
    if (limit == 0) limit = kMaxBpLogEntries;
    out.reserve(std::min(limit, it->second.entries.size()));
    for (const auto& e : it->second.entries) {
        if (e.seq < sinceSeq) continue;
        out.push_back(e);
        if (out.size() >= limit) break;
    }
    return true;
}

bool ContinueThread(DWORD threadId) {
    HANDLE resumeEv;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_threadCtl.find(threadId);
        if (it == g_threadCtl.end() || !it->second.frozen) return false;
        it->second.stepArmed = false;
        if (it->second.ctx) it->second.ctx->EFlags &= ~kTF;
        resumeEv = it->second.resumeEvent;
    }
    SetEvent(resumeEv);
    return true;
}

bool StepThread(DWORD threadId, DWORD timeoutMs, Registers& outRegs) {
    HANDLE resumeEv, doneEv;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_threadCtl.find(threadId);
        if (it == g_threadCtl.end() || !it->second.frozen || !it->second.ctx) return false;
        it->second.stepArmed = true;
        it->second.ctx->EFlags |= kTF;
        ResetEvent(it->second.doneEvent);
        resumeEv = it->second.resumeEvent;
        doneEv = it->second.doneEvent;
    }
    SetEvent(resumeEv);
    if (WaitForSingleObject(doneEv, timeoutMs) != WAIT_OBJECT_0) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    outRegs = g_threadCtl[threadId].regs;
    return true;
}

bool ReadThreadRegisters(DWORD threadId, Registers& out) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_threadCtl.find(threadId);
        if (it != g_threadCtl.end() && it->second.frozen) {
            out = it->second.regs;
            return true;
        }
    }
    if (threadId == GetCurrentThreadId()) return false; // can't suspend/inspect self
    HANDLE h = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, threadId);
    if (!h) return false;
    SuspendThread(h);
    CONTEXT ctx = {};
    ctx.ContextFlags = CONTEXT_FULL;
    BOOL ok = GetThreadContext(h, &ctx);
    ResumeThread(h);
    CloseHandle(h);
    if (!ok) return false;
    out = CtxToRegs(&ctx);
    return true;
}

std::vector<uintptr_t> WalkStack(DWORD threadId, int maxFrames) {
    std::vector<uintptr_t> frames;
    Registers regs;
    if (!ReadThreadRegisters(threadId, regs)) return frames;
#ifdef _WIN64
    uintptr_t ip = regs.rip, fp = regs.rbp, sp = regs.rsp; constexpr size_t kPtrSize = 8;
#else
    uintptr_t ip = regs.eip, fp = regs.ebp, sp = regs.esp; constexpr size_t kPtrSize = 4;
#endif
    frames.push_back(ip);
    // Classical EBP chain first.
    for (int i = 1; i < maxFrames && fp; ++i) {
        std::vector<uint8_t> buf;
        if (!memory::ReadBytes(fp, kPtrSize * 2, buf)) break;
        uintptr_t savedFp = 0, ret = 0;
        std::memcpy(&savedFp, buf.data(), kPtrSize);
        std::memcpy(&ret, buf.data() + kPtrSize, kPtrSize);
        if (!ret) break;
        frames.push_back(ret);
        if (savedFp <= fp) break;
        fp = savedFp;
    }
    if ((int)frames.size() >= 2) return frames;

    // Fallback: StackWalk64 with a fresh CONTEXT, then heuristic scan.
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION,
                               FALSE, threadId);
    if (thread) {
        SuspendThread(thread);
        CONTEXT ctx{}; ctx.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(thread, &ctx)) {
            auto sw = StackWalkFromContext(ctx, thread, maxFrames);
            if (sw.size() > frames.size()) { ResumeThread(thread); CloseHandle(thread); return sw; }
        }
        ResumeThread(thread);
        CloseHandle(thread);
    }
    auto heur = HeuristicStackScan(sp, maxFrames - 1);
    frames.insert(frames.end(), heur.begin(), heur.end());
    return frames;
}

std::vector<DWORD> ListThreadIds() {
    std::vector<DWORD> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    THREADENTRY32 te = {};
    te.dwSize = sizeof(te);
    DWORD pid = GetCurrentProcessId();
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid) out.push_back(te.th32ThreadID);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return out;
}

int StartTrace(const TraceConfig& requested, std::string& error) {
    TraceConfig config = requested;
    if (config.threadId == 0 || config.threadId == GetCurrentThreadId()) { error = "invalid_thread"; return -1; }
    if (config.rangeEnd && config.rangeStart >= config.rangeEnd) { error = "invalid_range"; return -1; }
    config.maxSteps = (std::max)(uint64_t{1}, (std::min)(config.maxSteps, uint64_t{1000000}));
    config.maxEvents = (std::max)(size_t{1}, (std::min)(config.maxEvents, size_t{100000}));

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const auto& item : g_traces) {
            if (item.second.active && item.second.config.threadId == config.threadId) {
                error = "thread_already_traced";
                return -1;
            }
        }
    }

    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, config.threadId);
    if (!thread) { error = "thread_open_failed"; return -1; }
    bool armed = false;
    int id = -1;
    if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
        CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(thread, &context)) {
            context.EFlags |= kTF;
            if (SetThreadContext(thread, &context)) {
                std::lock_guard<std::mutex> lock(g_mutex);
                id = g_nextTraceId++;
                TraceState state;
                state.config = config;
                state.events.reserve((std::min)(config.maxEvents, size_t{4096}));
                g_traces.emplace(id, std::move(state));
                armed = true;
            }
        }
        ResumeThread(thread);
    }
    CloseHandle(thread);
    if (!armed) { error = "trace_arm_failed"; return -1; }

    return id;
}

bool StopTrace(int id, const std::string& reason) {
    DWORD tid = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_traces.find(id);
        if (it == g_traces.end()) return false;
        it->second.active = false;
        it->second.stopReason = reason;
        tid = it->second.config.threadId;
    }
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, tid);
    if (!thread) return true;
    if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
        CONTEXT context{}; context.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(thread, &context)) { context.EFlags &= ~kTF; SetThreadContext(thread, &context); }
        ResumeThread(thread);
    }
    CloseHandle(thread);
    return true;
}

std::vector<TraceInfo> ListTraces() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<TraceInfo> out;
    for (const auto& item : g_traces) {
        const auto& trace = item.second;
        out.push_back({item.first, trace.config.threadId, trace.active, trace.stopReason,
                       trace.steps, trace.events.size(), trace.truncated});
    }
    return out;
}

bool GetTraceEvents(int id, size_t offset, size_t limit, std::vector<TraceEvent>& out, size_t& total) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_traces.find(id);
    if (it == g_traces.end()) return false;
    total = it->second.events.size();
    limit = (std::min)(limit, size_t{1000});
    if (offset >= total) { out.clear(); return true; }
    const size_t end = (std::min)(total, offset + limit);
    out.assign(it->second.events.begin() + offset, it->second.events.begin() + end);
    return true;
}

bool GetTraceCoverage(int id, std::vector<std::pair<uintptr_t, uint64_t>>& out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_traces.find(id);
    if (it == g_traces.end()) return false;
    out.assign(it->second.coverage.begin(), it->second.coverage.end());
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    return true;
}

bool RemoveTrace(int id) {
    StopTrace(id, "removed");
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_traces.erase(id) != 0;
}

} // namespace dbg
