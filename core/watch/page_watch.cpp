#include "watch.h"
#include "../log.h"
#include "../memory/memory.h"

#include <Zydis/Zydis.h>

#include <windows.h>
#include <atomic>
#include <mutex>
#include <deque>
#include <vector>
#include <chrono>
#include <map>

namespace watch {

namespace {

constexpr size_t kMaxPageEvents = 1000;

struct PageInfo {
    uintptr_t pageBase;
    DWORD originalProtect; // protection observed before we OR'd in PAGE_GUARD
};

struct RegisteredWatch {
    int id;
    uintptr_t start;
    uintptr_t end; // exclusive
    std::string label;
    std::vector<PageInfo> pages;
};

std::mutex g_mutex; // guards g_watches and g_nextId
std::vector<RegisteredWatch> g_watches;
int g_nextId = 1;

std::mutex g_eventsMutex;
std::deque<PageAccessEvent> g_events;

struct PendingAccess {
    PageAccessEvent event;
    uintptr_t pageBase;
    DWORD protect;
};
std::map<DWORD, PendingAccess> g_pending;

std::atomic<bool> g_vehInstalled{false};
std::mutex g_vehMutex;
PVOID g_vehHandle = nullptr;

long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

size_t PageSize() {
    static size_t size = [] {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        return static_cast<size_t>(si.dwPageSize);
    }();
    return size;
}

dbg::Registers CaptureRegisters(PCONTEXT ctx) {
    dbg::Registers r{};
#ifdef _WIN64
    r.rax=ctx->Rax; r.rbx=ctx->Rbx; r.rcx=ctx->Rcx; r.rdx=ctx->Rdx; r.rsi=ctx->Rsi; r.rdi=ctx->Rdi;
    r.rbp=ctx->Rbp; r.rsp=ctx->Rsp; r.r8=ctx->R8; r.r9=ctx->R9; r.r10=ctx->R10; r.r11=ctx->R11;
    r.r12=ctx->R12; r.r13=ctx->R13; r.r14=ctx->R14; r.r15=ctx->R15; r.rip=ctx->Rip; r.eflags=ctx->EFlags;
#else
    r.eax=ctx->Eax; r.ebx=ctx->Ebx; r.ecx=ctx->Ecx; r.edx=ctx->Edx; r.esi=ctx->Esi; r.edi=ctx->Edi;
    r.ebp=ctx->Ebp; r.esp=ctx->Esp; r.eip=ctx->Eip; r.eflags=ctx->EFlags;
#endif
    return r;
}

uintptr_t ContextIp(PCONTEXT ctx) {
#ifdef _WIN64
    return static_cast<uintptr_t>(ctx->Rip);
#else
    return static_cast<uintptr_t>(ctx->Eip);
#endif
}

std::vector<uintptr_t> CaptureStack(PCONTEXT ctx) {
    std::vector<uintptr_t> frames{ContextIp(ctx)};
#ifdef _WIN64
    uintptr_t frame = static_cast<uintptr_t>(ctx->Rbp);
    constexpr size_t ptrSize = 8;
#else
    uintptr_t frame = static_cast<uintptr_t>(ctx->Ebp);
    constexpr size_t ptrSize = 4;
#endif
    for (int i = 0; i < 15 && frame; ++i) {
        std::vector<uint8_t> bytes;
        if (!memory::ReadBytes(frame, ptrSize * 2, bytes)) break;
        uintptr_t next = 0, ret = 0;
        memcpy(&next, bytes.data(), ptrSize);
        memcpy(&ret, bytes.data() + ptrSize, ptrSize);
        if (!ret || next <= frame) break;
        frames.push_back(ret);
        frame = next;
    }
    return frames;
}

size_t DecodeAccessSize(uintptr_t ip) {
    std::vector<uint8_t> bytes;
    if (!memory::ReadBytes(ip, ZYDIS_MAX_INSTRUCTION_LENGTH, bytes)) return 0;
#ifdef _WIN64
    ZydisDecoder decoder; ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
#else
    ZydisDecoder decoder; ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32);
#endif
    ZydisDecodedInstruction instruction{};
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT]{};
    if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, bytes.data(), bytes.size(), &instruction, operands))) return 0;
    for (uint8_t i = 0; i < instruction.operand_count; ++i) {
        if (operands[i].type == ZYDIS_OPERAND_TYPE_MEMORY && operands[i].size) return operands[i].size / 8;
    }
    return 0;
}

void PushPageEvent(PageAccessEvent event) {
    std::lock_guard<std::mutex> lock(g_eventsMutex);
    if (g_events.size() >= kMaxPageEvents) g_events.pop_front();
    g_events.push_back(std::move(event));
}

LONG WINAPI PageVectoredHandler(EXCEPTION_POINTERS* info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    const DWORD tid = GetCurrentThreadId();
    if (code == EXCEPTION_SINGLE_STEP) {
        PendingAccess pending;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_pending.find(tid);
            if (it == g_pending.end()) return EXCEPTION_CONTINUE_SEARCH;
            pending = std::move(it->second);
            g_pending.erase(it);
        }
        if (!pending.event.before.empty())
            memory::ReadBytes(pending.event.address, pending.event.before.size(), pending.event.after);
        DWORD oldProtect = 0;
        VirtualProtect(reinterpret_cast<LPVOID>(pending.pageBase), PageSize(),
                       pending.protect | PAGE_GUARD, &oldProtect);
        info->ContextRecord->EFlags &= ~0x100u;
        PushPageEvent(std::move(pending.event));
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code != EXCEPTION_GUARD_PAGE) return EXCEPTION_CONTINUE_SEARCH;

    uintptr_t faultAddr = static_cast<uintptr_t>(info->ExceptionRecord->ExceptionInformation[1]);
    ULONG_PTR accessType = info->ExceptionRecord->ExceptionInformation[0];
    size_t pageSize = PageSize();
    uintptr_t pageBase = faultAddr - (faultAddr % pageSize);

    std::lock_guard<std::mutex> lock(g_mutex);
    bool matched = false;
    for (auto& w : g_watches) {
        if (faultAddr < w.start || faultAddr >= w.end) continue;
        matched = true;
        const char* access = accessType == 8 ? "execute" : accessType == 1 ? "write" : "read";
        for (auto& p : w.pages) {
            if (p.pageBase != pageBase) continue;
            PageAccessEvent event{};
            event.timestamp_ms = NowMs(); event.watch_id = w.id; event.address = faultAddr;
            event.access = access; event.label = w.label; event.thread_id = tid;
            event.instruction = ContextIp(info->ContextRecord);
            event.access_size = DecodeAccessSize(event.instruction);
            event.registers = CaptureRegisters(info->ContextRecord);
            event.stack = CaptureStack(info->ContextRecord);
            const size_t captureSize = (std::min)(static_cast<size_t>(16),
                event.access_size ? event.access_size : static_cast<size_t>(1));
            memory::ReadBytes(faultAddr, captureSize, event.before);
            g_pending[tid] = PendingAccess{std::move(event), pageBase, p.originalProtect};
            info->ContextRecord->EFlags |= 0x100u;
            break;
        }
    }

    return matched ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

void EnsureVehInstalled() {
    if (g_vehInstalled.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(g_vehMutex);
    if (g_vehInstalled.load(std::memory_order_relaxed)) return;
    g_vehHandle = AddVectoredExceptionHandler(1, PageVectoredHandler);
    dbglog::Line("page_watch: VEH installed=%d", g_vehHandle != nullptr);
    g_vehInstalled.store(true, std::memory_order_relaxed);
}

} // namespace

int AddPageWatch(uintptr_t address, size_t size, const std::string& label) {
    if (size == 0) return -1;
    size_t pageSize = PageSize();
    uintptr_t start = address - (address % pageSize);
    uintptr_t endAligned = ((address + size + pageSize - 1) / pageSize) * pageSize;

    std::vector<PageInfo> pages;
    for (uintptr_t p = start; p < endAligned; p += pageSize) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(reinterpret_cast<LPCVOID>(p), &mbi, sizeof(mbi)) != sizeof(mbi)) return -1;
        if (mbi.State != MEM_COMMIT) return -1;

        DWORD oldProtect;
        DWORD newProtect = mbi.Protect | PAGE_GUARD;
        if (!VirtualProtect(reinterpret_cast<LPVOID>(p), pageSize, newProtect, &oldProtect)) return -1;
        pages.push_back({p, mbi.Protect});
    }

    EnsureVehInstalled();

    std::lock_guard<std::mutex> lock(g_mutex);
    int id = g_nextId++;
    g_watches.push_back({id, address, address + size, label, std::move(pages)});
    return id;
}

bool RemovePageWatch(int id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (size_t i = 0; i < g_watches.size(); ++i) {
        if (g_watches[i].id != id) continue;
        size_t pageSize = PageSize();
        for (auto& p : g_watches[i].pages) {
            DWORD oldProtect;
            VirtualProtect(reinterpret_cast<LPVOID>(p.pageBase), pageSize, p.originalProtect, &oldProtect);
        }
        g_watches.erase(g_watches.begin() + i);
        return true;
    }
    return false;
}

std::vector<PageWatchInfo> ListPageWatches() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<PageWatchInfo> out;
    out.reserve(g_watches.size());
    for (const auto& w : g_watches) out.push_back({w.id, w.start, w.end - w.start, w.label});
    return out;
}

std::vector<PageAccessEvent> SnapshotPageAccessEvents() {
    std::lock_guard<std::mutex> lock(g_eventsMutex);
    return std::vector<PageAccessEvent>(g_events.begin(), g_events.end());
}
std::vector<PageAccessEvent> DrainPageAccessEvents() {
    std::lock_guard<std::mutex> lock(g_eventsMutex);
    std::vector<PageAccessEvent> out(g_events.begin(), g_events.end());
    g_events.clear();
    return out;
}

} // namespace watch
