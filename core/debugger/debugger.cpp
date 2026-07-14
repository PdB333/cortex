#include "debugger.h"
#include "../memory/memory.h"

#include <tlhelp32.h>
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

struct SwEntry {
    uintptr_t address;
    uint8_t origByte;
    BpAction action;
    uint64_t hitCount;
    std::optional<BpCondition> condition;
};

struct HwEntry {
    uintptr_t address;
    int size;
    BpKind kind;
    int slot;
    BpAction action;
    uint64_t hitCount;
    std::optional<BpCondition> condition;
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
std::map<int, HwEntry> g_hwBps;
bool g_hwSlotUsed[4] = {false, false, false, false};
std::map<DWORD, ThreadCtl> g_threadCtl;
std::map<DWORD, uintptr_t> g_pendingSwRestore;
PVOID g_vehHandle = nullptr;
std::atomic<bool> g_hwMonitorRunning{false};
std::thread g_hwMonitorThread;
std::set<DWORD> g_hwConfiguredThreads;

// Per-breakpoint ring buffer for BpAction::Log hits. Capped so a hot
// breakpoint (hit thousands of times/sec) can't grow this unbounded --
// callers polling GET /debug/breakpoint/{id}/log just see the most recent
// window. Must be accessed with g_mutex held.
constexpr size_t kMaxBpLogEntries = 500;
std::map<int, std::deque<BpLogEntry>> g_bpLogs;

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

std::vector<uintptr_t> WalkContextStack(PCONTEXT ctx, int maxFrames) {
    std::vector<uintptr_t> frames{GetCtxIp(ctx)};
#ifdef _WIN64
    uintptr_t frame=ctx->Rbp; constexpr size_t ptrSize=8;
#else
    uintptr_t frame=ctx->Ebp; constexpr size_t ptrSize=4;
#endif
    for(int i=1;i<maxFrames&&frame;++i){std::vector<uint8_t>b;if(!memory::ReadBytes(frame,ptrSize*2,b))break;uintptr_t next=0,ret=0;memcpy(&next,b.data(),ptrSize);memcpy(&ret,b.data()+ptrSize,ptrSize);if(!ret||next<=frame)break;frames.push_back(ret);frame=next;}
    return frames;
}

void PushLogEntry(int id, DWORD tid, uint64_t seq, PCONTEXT ctx) {
    auto& dq = g_bpLogs[id];
    std::vector<uint8_t> bytes; memory::ReadBytes(GetCtxIp(ctx),16,bytes);
    dq.push_back(BpLogEntry{seq, tid, GetTickCount64(), CtxToRegs(ctx), GetCtxIp(ctx),
                            std::move(bytes), WalkContextStack(ctx,16)});
    if (dq.size() > kMaxBpLogEntries) dq.pop_front();
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
        // INT3 is a trap: EIP/ExceptionAddress already point one byte past
        // the 0xCC that fired.
        uintptr_t addr = reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress) - 1;

        int foundId = -1;
        SwEntry entry{};
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            for (auto& [id, e] : g_swBps) {
                if (e.address == addr) { foundId = id; entry = e; break; }
            }
        }
        if (foundId < 0) return EXCEPTION_CONTINUE_SEARCH;

        SetCtxIp(ctx, addr);
        memory::WriteBytes(addr, {entry.origByte});

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
            memory::WriteBytes(pendingAddr, {0xCC});
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

void Init() {
    if (g_vehHandle) return;
    g_vehHandle = AddVectoredExceptionHandler(1, VectoredHandler);
    g_hwMonitorRunning = true;
    g_hwMonitorThread = std::thread(HwThreadMonitor);
}

bool Shutdown() {
    g_hwMonitorRunning = false;
    if (g_hwMonitorThread.joinable()) g_hwMonitorThread.join();

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

int AddBreakpoint(BpKind kind, uintptr_t address, int size, BpAction action, const BpCondition* condition) {
    std::optional<BpCondition> cond = condition ? std::optional<BpCondition>(*condition) : std::nullopt;

    if (kind == BpKind::Software) {
        // INT3 only makes sense on executable code -- writing it into a data
        // address corrupts that data and can never trigger (nothing ever
        // executes there), so refuse rather than silently vandalizing memory.
        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQueryEx(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)) == 0) {
            return -1;
        }
        constexpr DWORD kExecMask = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if ((mbi.Protect & kExecMask) == 0) return -1;

        std::vector<uint8_t> orig;
        if (!memory::ReadBytes(address, 1, orig)) return -1;
        if (!memory::WriteBytes(address, {0xCC})) return -1;
        std::lock_guard<std::mutex> lock(g_mutex);
        int id = g_nextBpId++;
        g_swBps[id] = SwEntry{address, orig[0], action, 0, cond};
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
    HwEntry newEntry{address, effSize, kind, slot, action, 0, cond};
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

bool RemoveBreakpoint(int id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto sw = g_swBps.find(id);
    if (sw != g_swBps.end()) {
        memory::WriteBytes(sw->second.address, {sw->second.origByte});
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
    return std::vector<BpLogEntry>(it->second.begin(), it->second.end());
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
    frames.push_back(regs.rip);
    uintptr_t ebp = regs.rbp;
    constexpr size_t kPtrSize = 8;
    auto readPtr = [](const uint8_t* p) -> uintptr_t { return *reinterpret_cast<const uint64_t*>(p); };
#else
    frames.push_back(regs.eip);
    uintptr_t ebp = regs.ebp;
    constexpr size_t kPtrSize = 4;
    auto readPtr = [](const uint8_t* p) -> uintptr_t { return *reinterpret_cast<const uint32_t*>(p); };
#endif
    for (int i = 0; i < maxFrames && ebp; ++i) {
        std::vector<uint8_t> buf;
        if (!memory::ReadBytes(ebp, kPtrSize * 2, buf)) break;
        uintptr_t savedEbp = readPtr(&buf[0]);
        uintptr_t retAddr = readPtr(&buf[kPtrSize]);
        if (retAddr == 0) break;
        frames.push_back(retAddr);
        if (savedEbp <= ebp) break; // guard against a corrupt/cyclic frame chain
        ebp = savedEbp;
    }
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
