#include "windows_debugger_backend.h"

#include "services/disassembly_service.h"
#include "target/module_provider.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string Trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool ParseInteger(const std::string& text, uint64_t& value) {
    try {
        const std::string trimmed = Trim(text);
        size_t used = 0;
        value = std::stoull(trimmed, &used, 0);
        return used == trimmed.size();
    } catch (...) {
        return false;
    }
}

std::string FileName(std::string path) {
    const auto slash = path.find_last_of("/\\");
    if (slash != std::string::npos) path.erase(0, slash + 1);
    return path;
}

} // namespace

#if defined(_WIN32)

struct WindowsDebuggerBackend::Impl {
    enum class ResumeCommand { None, Continue, Step };

    struct Breakpoint {
        BreakpointInfo info;
        uint8_t originalByte = 0;
        int hwSlot = -1;
        uint64_t nextLogSeq = 1;
        std::deque<BreakpointLogEntry> log;
    };

    struct TempStepOver {
        uint64_t address = 0;
        uint8_t originalByte = 0;
        int sourceSoftwareBreakpointId = -1;
    };

    struct DebugPause {
        bool active = false;
        DEBUG_EVENT event{};
        PausedThread value;
    };

    explicit Impl(cortex::target::SessionManager& owner)
        : sessions(owner), disassembly(owner) {}

    cortex::target::SessionManager& sessions;
    cortex::services::DisassemblyService disassembly;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::condition_variable stepCv;
    std::thread worker;
    std::atomic<bool> stop{false};
    bool attached = false;
    bool attachFinished = false;
    bool initialBreakpointSeen = false;
    std::string attachError;
    cortex::target::TargetDescriptor target;
    DWORD pid = 0;
    HANDLE process = nullptr;
    int nextBreakpointId = 1;
    std::map<int, Breakpoint> bps;
    bool hwSlotUsed[4] = {false, false, false, false};
    std::map<DWORD, int> pendingSoftwareRearm;
    std::map<DWORD, TempStepOver> tempStepOvers;
    std::map<DWORD, HANDLE> manualPaused;
    DebugPause debugPause;
    ResumeCommand resumeCommand = ResumeCommand::None;
    DWORD requestedStepThread = 0;
    uint64_t stepGeneration = 0;
    cortex::target::ThreadRegisterSnapshot stepSnapshot;

    static void SetError(std::string* error, const std::string& value) {
        if (error) *error = value;
    }

    bool targetIsWow64X86() const {
#if defined(_WIN64)
        return target.architecture == cortex::target::Architecture::X86;
#else
        return false;
#endif
    }

    static void Push(std::vector<cortex::target::RegisterValue>& values, const char* name, uint64_t value) {
        values.push_back({name, value});
    }

    bool snapshotFromHandle(HANDLE thread, DWORD tid,
                            cortex::target::ThreadRegisterSnapshot& out,
                            std::string* error = nullptr) const {
        out = {};
        out.threadId = tid;
#if defined(_WIN64)
        if (targetIsWow64X86()) {
            WOW64_CONTEXT c{};
            c.ContextFlags = WOW64_CONTEXT_FULL | WOW64_CONTEXT_DEBUG_REGISTERS;
            if (!Wow64GetThreadContext(thread, &c)) {
                SetError(error, "wow64_get_context_failed:" + std::to_string(GetLastError()));
                return false;
            }
            out.instructionPointer = c.Eip;
            Push(out.registers, "EAX", c.Eax); Push(out.registers, "EBX", c.Ebx);
            Push(out.registers, "ECX", c.Ecx); Push(out.registers, "EDX", c.Edx);
            Push(out.registers, "ESI", c.Esi); Push(out.registers, "EDI", c.Edi);
            Push(out.registers, "EBP", c.Ebp); Push(out.registers, "ESP", c.Esp);
            Push(out.registers, "EIP", c.Eip); Push(out.registers, "EFLAGS", c.EFlags);
            return true;
        }
        if (target.architecture != cortex::target::Architecture::X64) {
            SetError(error, "register_architecture_not_supported");
            return false;
        }
        CONTEXT c{};
        c.ContextFlags = CONTEXT_FULL | CONTEXT_DEBUG_REGISTERS;
        if (!GetThreadContext(thread, &c)) {
            SetError(error, "get_context_failed:" + std::to_string(GetLastError()));
            return false;
        }
        out.instructionPointer = c.Rip;
        Push(out.registers, "RAX", c.Rax); Push(out.registers, "RBX", c.Rbx);
        Push(out.registers, "RCX", c.Rcx); Push(out.registers, "RDX", c.Rdx);
        Push(out.registers, "RSI", c.Rsi); Push(out.registers, "RDI", c.Rdi);
        Push(out.registers, "RBP", c.Rbp); Push(out.registers, "RSP", c.Rsp);
        Push(out.registers, "R8", c.R8); Push(out.registers, "R9", c.R9);
        Push(out.registers, "R10", c.R10); Push(out.registers, "R11", c.R11);
        Push(out.registers, "R12", c.R12); Push(out.registers, "R13", c.R13);
        Push(out.registers, "R14", c.R14); Push(out.registers, "R15", c.R15);
        Push(out.registers, "RIP", c.Rip); Push(out.registers, "EFLAGS", c.EFlags);
        return true;
#else
        if (target.architecture != cortex::target::Architecture::X86) {
            SetError(error, "register_bitness_not_supported");
            return false;
        }
        CONTEXT c{};
        c.ContextFlags = CONTEXT_FULL | CONTEXT_DEBUG_REGISTERS;
        if (!GetThreadContext(thread, &c)) {
            SetError(error, "get_context_failed:" + std::to_string(GetLastError()));
            return false;
        }
        out.instructionPointer = c.Eip;
        Push(out.registers, "EAX", c.Eax); Push(out.registers, "EBX", c.Ebx);
        Push(out.registers, "ECX", c.Ecx); Push(out.registers, "EDX", c.Edx);
        Push(out.registers, "ESI", c.Esi); Push(out.registers, "EDI", c.Edi);
        Push(out.registers, "EBP", c.Ebp); Push(out.registers, "ESP", c.Esp);
        Push(out.registers, "EIP", c.Eip); Push(out.registers, "EFLAGS", c.EFlags);
        return true;
#endif
    }

    bool setExecutionState(HANDLE thread,
                           std::optional<uint64_t> instructionPointer,
                           bool trap,
                           bool resumeFlag,
                           std::string* error = nullptr) const {
#if defined(_WIN64)
        if (targetIsWow64X86()) {
            WOW64_CONTEXT c{};
            c.ContextFlags = WOW64_CONTEXT_CONTROL;
            if (!Wow64GetThreadContext(thread, &c)) {
                SetError(error, "wow64_get_context_failed:" + std::to_string(GetLastError()));
                return false;
            }
            if (instructionPointer) c.Eip = static_cast<DWORD>(*instructionPointer);
            if (trap) c.EFlags |= 0x100u; else c.EFlags &= ~0x100u;
            if (resumeFlag) c.EFlags |= 0x10000u; else c.EFlags &= ~0x10000u;
            if (!Wow64SetThreadContext(thread, &c)) {
                SetError(error, "wow64_set_context_failed:" + std::to_string(GetLastError()));
                return false;
            }
            return true;
        }
#endif
        CONTEXT c{};
        c.ContextFlags = CONTEXT_CONTROL;
        if (!GetThreadContext(thread, &c)) {
            SetError(error, "get_context_failed:" + std::to_string(GetLastError()));
            return false;
        }
#if defined(_WIN64)
        if (instructionPointer) c.Rip = *instructionPointer;
#else
        if (instructionPointer) c.Eip = static_cast<DWORD>(*instructionPointer);
#endif
        if (trap) c.EFlags |= 0x100u; else c.EFlags &= ~0x100u;
        if (resumeFlag) c.EFlags |= 0x10000u; else c.EFlags &= ~0x10000u;
        if (!SetThreadContext(thread, &c)) {
            SetError(error, "set_context_failed:" + std::to_string(GetLastError()));
            return false;
        }
        return true;
    }

    bool getDebugRegisters(HANDLE thread, uint64_t dr[4], uint64_t& dr6, uint64_t& dr7,
                           std::string* error = nullptr) const {
#if defined(_WIN64)
        if (targetIsWow64X86()) {
            WOW64_CONTEXT c{};
            c.ContextFlags = WOW64_CONTEXT_DEBUG_REGISTERS;
            if (!Wow64GetThreadContext(thread, &c)) {
                SetError(error, "wow64_get_debug_context_failed:" + std::to_string(GetLastError()));
                return false;
            }
            dr[0] = c.Dr0; dr[1] = c.Dr1; dr[2] = c.Dr2; dr[3] = c.Dr3;
            dr6 = c.Dr6; dr7 = c.Dr7;
            return true;
        }
#endif
        CONTEXT c{};
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (!GetThreadContext(thread, &c)) {
            SetError(error, "get_debug_context_failed:" + std::to_string(GetLastError()));
            return false;
        }
        dr[0] = c.Dr0; dr[1] = c.Dr1; dr[2] = c.Dr2; dr[3] = c.Dr3;
        dr6 = c.Dr6; dr7 = c.Dr7;
        return true;
    }

    bool setDebugRegisters(HANDLE thread, const uint64_t dr[4], uint64_t dr6, uint64_t dr7,
                           std::string* error = nullptr) const {
#if defined(_WIN64)
        if (targetIsWow64X86()) {
            WOW64_CONTEXT c{};
            c.ContextFlags = WOW64_CONTEXT_DEBUG_REGISTERS;
            if (!Wow64GetThreadContext(thread, &c)) {
                SetError(error, "wow64_get_debug_context_failed:" + std::to_string(GetLastError()));
                return false;
            }
            c.Dr0 = static_cast<DWORD>(dr[0]); c.Dr1 = static_cast<DWORD>(dr[1]);
            c.Dr2 = static_cast<DWORD>(dr[2]); c.Dr3 = static_cast<DWORD>(dr[3]);
            c.Dr6 = static_cast<DWORD>(dr6); c.Dr7 = static_cast<DWORD>(dr7);
            if (!Wow64SetThreadContext(thread, &c)) {
                SetError(error, "wow64_set_debug_context_failed:" + std::to_string(GetLastError()));
                return false;
            }
            return true;
        }
#endif
        CONTEXT c{};
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (!GetThreadContext(thread, &c)) {
            SetError(error, "get_debug_context_failed:" + std::to_string(GetLastError()));
            return false;
        }
        c.Dr0 = static_cast<DWORD_PTR>(dr[0]); c.Dr1 = static_cast<DWORD_PTR>(dr[1]);
        c.Dr2 = static_cast<DWORD_PTR>(dr[2]); c.Dr3 = static_cast<DWORD_PTR>(dr[3]);
        c.Dr6 = static_cast<DWORD_PTR>(dr6); c.Dr7 = static_cast<DWORD_PTR>(dr7);
        if (!SetThreadContext(thread, &c)) {
            SetError(error, "set_debug_context_failed:" + std::to_string(GetLastError()));
            return false;
        }
        return true;
    }

    static uint64_t HardwareLenBits(int size) {
        switch (size) {
            case 1: return 0;
            case 2: return 1;
            case 4: return 3;
            case 8: return 2;
            default: return 3;
        }
    }

    static uint64_t HardwareRwBits(const std::string& kind) {
        if (kind == "hw_write") return 1;
        if (kind == "hw_readwrite") return 3;
        return 0;
    }

    bool configureHardwareHandle(HANDLE thread, const Breakpoint& bp, bool enable,
                                 std::string* error = nullptr) const {
        if (!thread || bp.hwSlot < 0 || bp.hwSlot > 3) return false;
        uint64_t dr[4] = {}, dr6 = 0, dr7 = 0;
        if (!getDebugRegisters(thread, dr, dr6, dr7, error)) return false;
        const int slot = bp.hwSlot;
        const uint64_t enableMask = uint64_t{3} << (slot * 2);
        const uint64_t typeMask = uint64_t{0xF} << (16 + slot * 4);
        dr7 &= ~enableMask;
        dr7 &= ~typeMask;
        if (enable) {
            dr[slot] = bp.info.address;
            dr7 |= uint64_t{1} << (slot * 2);
            const uint64_t rw = HardwareRwBits(bp.info.kind);
            const uint64_t len = bp.info.kind == "hw_execute" ? 0 : HardwareLenBits(bp.info.size);
            dr7 |= (rw | (len << 2)) << (16 + slot * 4);
        } else {
            dr[slot] = 0;
        }
        dr6 = 0;
        return setDebugRegisters(thread, dr, dr6, dr7, error);
    }

    bool configureHardwareThread(DWORD tid, const Breakpoint& bp, bool enable,
                                 bool alreadyStopped, std::string* error = nullptr,
                                 HANDLE suppliedThread = nullptr) const {
        HANDLE thread = suppliedThread;
        bool opened = false;
        if (!thread) {
            thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME |
                                THREAD_QUERY_INFORMATION, FALSE, tid);
            opened = true;
        }
        if (!thread) {
            SetError(error, "thread_open_failed:" + std::to_string(GetLastError()));
            return false;
        }
        bool suspended = alreadyStopped;
        if (!alreadyStopped) suspended = SuspendThread(thread) != static_cast<DWORD>(-1);
        if (!suspended) {
            SetError(error, "thread_suspend_failed:" + std::to_string(GetLastError()));
            if (opened) CloseHandle(thread);
            return false;
        }
        const bool ok = configureHardwareHandle(thread, bp, enable, error);
        if (!alreadyStopped) ResumeThread(thread);
        if (opened) CloseHandle(thread);
        return ok;
    }

    bool readByte(uint64_t address, uint8_t& byte, std::string* error = nullptr) const {
        SIZE_T read = 0;
        if (!process || !ReadProcessMemory(process, reinterpret_cast<const void*>(static_cast<uintptr_t>(address)),
                                           &byte, 1, &read) || read != 1) {
            SetError(error, "read_breakpoint_byte_failed:" + std::to_string(GetLastError()));
            return false;
        }
        return true;
    }

    bool writeByte(uint64_t address, uint8_t byte, std::string* error = nullptr) const {
        SIZE_T written = 0;
        DWORD oldProtect = 0;
        void* location = reinterpret_cast<void*>(static_cast<uintptr_t>(address));
        if (!process || !VirtualProtectEx(process, location, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            SetError(error, "protect_breakpoint_byte_failed:" + std::to_string(GetLastError()));
            return false;
        }
        const BOOL ok = WriteProcessMemory(process, location, &byte, 1, &written);
        DWORD ignored = 0;
        VirtualProtectEx(process, location, 1, oldProtect, &ignored);
        if (!ok || written != 1) {
            SetError(error, "write_breakpoint_byte_failed:" + std::to_string(GetLastError()));
            return false;
        }
        FlushInstructionCache(process, location, 1);
        return true;
    }

    Breakpoint* softwareAt(uint64_t address) {
        for (auto& item : bps) {
            if (item.second.info.kind == "software" && item.second.info.address == address)
                return &item.second;
        }
        return nullptr;
    }

    Breakpoint* hardwareAtSlot(int slot, DWORD tid) {
        for (auto& item : bps) {
            auto& bp = item.second;
            if (bp.hwSlot != slot || bp.info.kind == "software") continue;
            if (!bp.info.processGlobal && bp.info.targetThreadId != tid) continue;
            return &bp;
        }
        return nullptr;
    }

    void recordHit(Breakpoint& bp, const cortex::target::ThreadRegisterSnapshot& snapshot) {
        BreakpointLogEntry entry;
        entry.seq = bp.nextLogSeq++;
        entry.threadId = snapshot.threadId;
        entry.timestampMs = GetTickCount64();
        entry.instruction = snapshot.instructionPointer;
        entry.registers = snapshot;
        bp.log.push_back(std::move(entry));
        while (bp.log.size() > 500) bp.log.pop_front();
    }

    void setPause(const DEBUG_EVENT& event, int bpId,
                  const cortex::target::ThreadRegisterSnapshot& snapshot) {
        std::lock_guard<std::mutex> lock(mutex);
        debugPause.active = true;
        debugPause.event = event;
        debugPause.value.threadId = event.dwThreadId;
        debugPause.value.breakpointId = bpId;
        debugPause.value.registers = snapshot;
        resumeCommand = ResumeCommand::None;
        cv.notify_all();
    }

    bool waitForResume(const DEBUG_EVENT& event) {
        ResumeCommand command = ResumeCommand::Continue;
        bool hasPendingRearm = false;
        {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait(lock, [&] { return stop.load() || resumeCommand != ResumeCommand::None; });
            command = stop.load() ? ResumeCommand::Continue : resumeCommand;
            hasPendingRearm = pendingSoftwareRearm.find(event.dwThreadId) != pendingSoftwareRearm.end();
            if (command == ResumeCommand::Step) requestedStepThread = event.dwThreadId;
            debugPause.active = false;
            resumeCommand = ResumeCommand::None;
        }

        HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, event.dwThreadId);
        if (!thread) return false;
        const bool trap = command == ResumeCommand::Step || hasPendingRearm;
        const bool ok = setExecutionState(thread, std::nullopt, trap, false, nullptr);
        CloseHandle(thread);
        return ok;
    }

    void notifyStep(const cortex::target::ThreadRegisterSnapshot& snapshot) {
        std::lock_guard<std::mutex> lock(mutex);
        stepSnapshot = snapshot;
        ++stepGeneration;
        stepCv.notify_all();
    }

    bool installTempStepOver(DWORD tid, uint64_t address, int sourceSoftwareId,
                             std::string* error) {
        uint8_t original = 0;
        if (!readByte(address, original, error)) return false;
        if (original == 0xCC) {
            SetError(error, "step_over_target_already_breakpointed");
            return false;
        }
        if (!writeByte(address, 0xCC, error)) return false;
        std::lock_guard<std::mutex> lock(mutex);
        tempStepOvers[tid] = TempStepOver{address, original, sourceSoftwareId};
        return true;
    }

    bool handleTempStepOver(const DEBUG_EVENT& event, uint64_t address, bool& pauseEvent) {
        TempStepOver temp;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto it = tempStepOvers.find(event.dwThreadId);
            if (it != tempStepOvers.end() && it->second.address == address) {
                temp = it->second;
                tempStepOvers.erase(it);
                found = true;
            }
        }
        if (!found) return false;

        writeByte(temp.address, temp.originalByte, nullptr);
        if (temp.sourceSoftwareBreakpointId >= 0) {
            std::lock_guard<std::mutex> lock(mutex);
            const auto bp = bps.find(temp.sourceSoftwareBreakpointId);
            if (bp != bps.end()) writeByte(bp->second.info.address, 0xCC, nullptr);
        }

        HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, event.dwThreadId);
        if (!thread) return true;
        setExecutionState(thread, temp.address, false, false, nullptr);
        cortex::target::ThreadRegisterSnapshot snapshot;
        snapshotFromHandle(thread, event.dwThreadId, snapshot, nullptr);
        CloseHandle(thread);
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (requestedStepThread == event.dwThreadId) requestedStepThread = 0;
        }
        notifyStep(snapshot);
        setPause(event, -1, snapshot);
        pauseEvent = true;
        return true;
    }

    DWORD handleException(const DEBUG_EVENT& event, bool& pauseEvent) {
        pauseEvent = false;
        const auto& ex = event.u.Exception.ExceptionRecord;
        const DWORD code = ex.ExceptionCode;
        const uint64_t address = reinterpret_cast<uint64_t>(ex.ExceptionAddress);

        if (code == EXCEPTION_BREAKPOINT) {
            if (handleTempStepOver(event, address, pauseEvent)) return DBG_CONTINUE;

            Breakpoint local;
            bool found = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (Breakpoint* bp = softwareAt(address)) {
                    ++bp->info.hitCount;
                    local = *bp;
                    found = true;
                }
            }
            if (!found) {
                std::lock_guard<std::mutex> lock(mutex);
                if (!initialBreakpointSeen) {
                    initialBreakpointSeen = true;
                    cv.notify_all();
                    return DBG_CONTINUE;
                }
                return DBG_EXCEPTION_NOT_HANDLED;
            }

            writeByte(local.info.address, local.originalByte, nullptr);
            HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, event.dwThreadId);
            if (!thread) return DBG_EXCEPTION_NOT_HANDLED;
            if (!setExecutionState(thread, local.info.address, false, false, nullptr)) {
                CloseHandle(thread);
                return DBG_EXCEPTION_NOT_HANDLED;
            }
            cortex::target::ThreadRegisterSnapshot snapshot;
            snapshotFromHandle(thread, event.dwThreadId, snapshot, nullptr);
            CloseHandle(thread);
            {
                std::lock_guard<std::mutex> lock(mutex);
                auto current = bps.find(local.info.id);
                if (current != bps.end()) recordHit(current->second, snapshot);
                pendingSoftwareRearm[event.dwThreadId] = local.info.id;
            }
            if (local.info.pauseOnHit) {
                setPause(event, local.info.id, snapshot);
                pauseEvent = true;
            } else {
                HANDLE stepThread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, event.dwThreadId);
                if (stepThread) {
                    setExecutionState(stepThread, std::nullopt, true, false, nullptr);
                    CloseHandle(stepThread);
                }
            }
            return DBG_CONTINUE;
        }

        if (code == EXCEPTION_SINGLE_STEP) {
            HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, event.dwThreadId);
            if (!thread) return DBG_EXCEPTION_NOT_HANDLED;

            int rearmId = -1;
            bool requested = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                auto pending = pendingSoftwareRearm.find(event.dwThreadId);
                if (pending != pendingSoftwareRearm.end()) {
                    rearmId = pending->second;
                    pendingSoftwareRearm.erase(pending);
                }
                requested = requestedStepThread == event.dwThreadId;
            }
            if (rearmId >= 0) {
                std::lock_guard<std::mutex> lock(mutex);
                const auto it = bps.find(rearmId);
                if (it != bps.end() && it->second.info.kind == "software")
                    writeByte(it->second.info.address, 0xCC, nullptr);
            }

            if (requested) {
                setExecutionState(thread, std::nullopt, false, false, nullptr);
                cortex::target::ThreadRegisterSnapshot snapshot;
                snapshotFromHandle(thread, event.dwThreadId, snapshot, nullptr);
                CloseHandle(thread);
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    requestedStepThread = 0;
                }
                notifyStep(snapshot);
                setPause(event, -1, snapshot);
                pauseEvent = true;
                return DBG_CONTINUE;
            }

            uint64_t dr[4] = {}, dr6 = 0, dr7 = 0;
            int slot = -1;
            if (getDebugRegisters(thread, dr, dr6, dr7, nullptr)) {
                for (int candidate = 0; candidate < 4; ++candidate) {
                    if ((dr6 & (uint64_t{1} << candidate)) != 0) { slot = candidate; break; }
                }
                if (slot >= 0) setDebugRegisters(thread, dr, 0, dr7, nullptr);
            }

            Breakpoint local;
            bool found = false;
            if (slot >= 0) {
                std::lock_guard<std::mutex> lock(mutex);
                if (Breakpoint* bp = hardwareAtSlot(slot, event.dwThreadId)) {
                    ++bp->info.hitCount;
                    local = *bp;
                    found = true;
                }
            }
            if (found) {
                setExecutionState(thread, std::nullopt, false, true, nullptr);
                cortex::target::ThreadRegisterSnapshot snapshot;
                snapshotFromHandle(thread, event.dwThreadId, snapshot, nullptr);
                CloseHandle(thread);
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    auto current = bps.find(local.info.id);
                    if (current != bps.end()) recordHit(current->second, snapshot);
                }
                if (local.info.pauseOnHit) {
                    setPause(event, local.info.id, snapshot);
                    pauseEvent = true;
                }
                return DBG_CONTINUE;
            }

            if (rearmId >= 0) setExecutionState(thread, std::nullopt, false, false, nullptr);
            CloseHandle(thread);
            if (rearmId >= 0) return DBG_CONTINUE;
            return DBG_EXCEPTION_NOT_HANDLED;
        }

        return DBG_EXCEPTION_NOT_HANDLED;
    }

    void applyGlobalsToNewThread(DWORD tid, HANDLE thread) {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& item : bps) {
            auto& bp = item.second;
            if (bp.info.kind == "software" || !bp.info.processGlobal) continue;
            ++bp.info.totalThreads;
            if (configureHardwareThread(tid, bp, true, true, nullptr, thread))
                ++bp.info.appliedThreads;
        }
    }

    void cleanupBreakpoints() {
        std::vector<Breakpoint> copy;
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (const auto& item : bps) copy.push_back(item.second);
            for (const auto& item : tempStepOvers) writeByte(item.second.address, item.second.originalByte, nullptr);
            tempStepOvers.clear();
        }
        for (const auto& bp : copy) {
            if (bp.info.kind == "software") {
                writeByte(bp.info.address, bp.originalByte, nullptr);
            } else {
                for (uint64_t tid : cortex::target::ListTargetThreads(target, nullptr)) {
                    if (!bp.info.processGlobal && tid != bp.info.targetThreadId) continue;
                    configureHardwareThread(static_cast<DWORD>(tid), bp, false, false, nullptr);
                }
            }
        }
    }

    void run() {
        if (!DebugActiveProcess(pid)) {
            std::lock_guard<std::mutex> lock(mutex);
            attachError = "debug_attach_failed:" + std::to_string(GetLastError());
            attachFinished = true;
            attached = false;
            cv.notify_all();
            return;
        }
        DebugSetProcessKillOnExit(FALSE);

        HANDLE ownProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE |
                                        PROCESS_VM_OPERATION | SYNCHRONIZE, FALSE, pid);
        {
            std::lock_guard<std::mutex> lock(mutex);
            process = ownProcess;
            attached = ownProcess != nullptr;
            if (!ownProcess) attachError = "debug_open_process_failed:" + std::to_string(GetLastError());
            attachFinished = true;
            cv.notify_all();
        }
        if (!ownProcess) {
            DebugActiveProcessStop(pid);
            return;
        }

        while (!stop.load()) {
            DEBUG_EVENT event{};
            if (!WaitForDebugEvent(&event, 100)) {
                if (GetLastError() == ERROR_SEM_TIMEOUT) continue;
                break;
            }

            DWORD continueStatus = DBG_CONTINUE;
            bool pauseEvent = false;
            switch (event.dwDebugEventCode) {
                case CREATE_PROCESS_DEBUG_EVENT:
                    if (event.u.CreateProcessInfo.hFile) CloseHandle(event.u.CreateProcessInfo.hFile);
                    if (event.u.CreateProcessInfo.hThread) CloseHandle(event.u.CreateProcessInfo.hThread);
                    if (event.u.CreateProcessInfo.hProcess) CloseHandle(event.u.CreateProcessInfo.hProcess);
                    break;
                case CREATE_THREAD_DEBUG_EVENT:
                    if (event.u.CreateThread.hThread) {
                        applyGlobalsToNewThread(event.dwThreadId, event.u.CreateThread.hThread);
                        CloseHandle(event.u.CreateThread.hThread);
                    }
                    break;
                case LOAD_DLL_DEBUG_EVENT:
                    if (event.u.LoadDll.hFile) CloseHandle(event.u.LoadDll.hFile);
                    break;
                case EXCEPTION_DEBUG_EVENT:
                    continueStatus = handleException(event, pauseEvent);
                    break;
                case EXIT_PROCESS_DEBUG_EVENT:
                    stop = true;
                    break;
                default:
                    break;
            }

            if (pauseEvent) waitForResume(event);
            ContinueDebugEvent(event.dwProcessId, event.dwThreadId, continueStatus);
        }

        cleanupBreakpoints();
        DebugActiveProcessStop(pid);
        {
            std::lock_guard<std::mutex> lock(mutex);
            attached = false;
            debugPause.active = false;
            resumeCommand = ResumeCommand::None;
            requestedStepThread = 0;
            if (process) {
                CloseHandle(process);
                process = nullptr;
            }
            cv.notify_all();
            stepCv.notify_all();
        }
    }
};

#endif

WindowsDebuggerBackend::WindowsDebuggerBackend(cortex::target::SessionManager& sessions)
#if defined(_WIN32)
    : impl_(std::make_unique<Impl>(sessions)) {}
#else
    : impl_(nullptr) { (void)sessions; }
#endif

WindowsDebuggerBackend::~WindowsDebuggerBackend() { detach(); }

bool WindowsDebuggerBackend::ensureAttached(std::string* error) {
#if defined(_WIN32)
    if (error) error->clear();
    auto session = impl_->sessions.Active();
    if (!session || !session->Alive()) {
        if (error) *error = "no_active_session";
        return false;
    }
    const auto target = session->Target();
    if (target.platform != cortex::target::Platform::Windows || target.processId == 0) {
        if (error) *error = "windows_debugger_requires_windows_process";
        return false;
    }
#if !defined(_WIN64)
    if (target.architecture != cortex::target::Architecture::X86) {
        if (error) *error = "x86_debugger_cannot_control_64bit_target";
        return false;
    }
#endif
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->attached && impl_->pid == static_cast<DWORD>(target.processId) &&
            (impl_->target.generation == 0 || target.generation == 0 ||
             impl_->target.generation == target.generation)) return true;
    }
    detach();
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->target = target;
        impl_->pid = static_cast<DWORD>(target.processId);
        impl_->stop = false;
        impl_->attachFinished = false;
        impl_->initialBreakpointSeen = false;
        impl_->attachError.clear();
    }
    impl_->worker = std::thread([this] { impl_->run(); });

    std::unique_lock<std::mutex> lock(impl_->mutex);
    if (!impl_->cv.wait_for(lock, std::chrono::seconds(5), [this] { return impl_->attachFinished; })) {
        lock.unlock();
        detach();
        if (error) *error = "debug_attach_timeout";
        return false;
    }
    if (!impl_->attached) {
        const std::string reason = impl_->attachError.empty() ? "debug_attach_failed" : impl_->attachError;
        lock.unlock();
        detach();
        if (error) *error = reason;
        return false;
    }
    if (!impl_->cv.wait_for(lock, std::chrono::seconds(5), [this] {
            return impl_->initialBreakpointSeen || !impl_->attached || impl_->stop.load();
        })) {
        lock.unlock();
        detach();
        if (error) *error = "debug_attach_initialization_timeout";
        return false;
    }
    if (!impl_->initialBreakpointSeen) {
        const std::string reason = impl_->attached ? "debug_attach_initialization_failed" : "debug_target_exited_during_attach";
        lock.unlock();
        detach();
        if (error) *error = reason;
        return false;
    }
    return true;
#else
    if (error) *error = "windows_debugger_not_supported";
    return false;
#endif
}

void WindowsDebuggerBackend::detach() {
#if defined(_WIN32)
    if (!impl_) return;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->stop = true;
        impl_->resumeCommand = Impl::ResumeCommand::Continue;
        for (auto& item : impl_->manualPaused) {
            ResumeThread(item.second);
            CloseHandle(item.second);
        }
        impl_->manualPaused.clear();
        impl_->cv.notify_all();
        impl_->stepCv.notify_all();
    }
    if (impl_->worker.joinable()) impl_->worker.join();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->pid = 0;
    impl_->target = {};
    impl_->bps.clear();
    impl_->pendingSoftwareRearm.clear();
    impl_->tempStepOvers.clear();
    for (bool& used : impl_->hwSlotUsed) used = false;
    impl_->nextBreakpointId = 1;
    impl_->attachFinished = false;
    impl_->initialBreakpointSeen = false;
    impl_->attachError.clear();
#endif
}

bool WindowsDebuggerBackend::attached() const {
#if defined(_WIN32)
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->attached;
#else
    return false;
#endif
}

bool WindowsDebuggerBackend::resolveAddress(const std::string& expression, uint64_t& address, std::string* error) const {
    if (error) error->clear();
    address = 0;
    const std::string value = Trim(expression);
    if (ParseInteger(value, address)) return address != 0;
    const auto plus = value.find('+');
    if (plus == std::string::npos) {
        if (error) *error = "invalid_address_expression";
        return false;
    }
    const std::string moduleName = Lower(Trim(value.substr(0, plus)));
    uint64_t offset = 0;
    if (!ParseInteger(Trim(value.substr(plus + 1)), offset)) {
        if (error) *error = "invalid_address_offset";
        return false;
    }
#if defined(_WIN32)
    auto session = impl_->sessions.Active();
    if (!session) {
        if (error) *error = "no_active_session";
        return false;
    }
    std::string moduleError;
    for (const auto& module : cortex::target::ListTargetModules(session->Target(), &moduleError)) {
        if (Lower(module.name) == moduleName || Lower(FileName(module.path)) == moduleName) {
            address = module.base + offset;
            return true;
        }
    }
    if (error) *error = moduleError.empty() ? "module_not_found" : moduleError;
#else
    if (error) *error = "windows_debugger_not_supported";
#endif
    return false;
}

int WindowsDebuggerBackend::addBreakpoint(const std::string& kindValue, uint64_t address, int size,
                                           bool pauseOnHit, bool processGlobal, uint64_t threadId,
                                           std::string* error) {
#if defined(_WIN32)
    if (error) error->clear();
    if (!ensureAttached(error)) return -1;
    const std::string kind = Lower(kindValue);
    if (kind != "software" && kind != "hw_execute" && kind != "hw_write" && kind != "hw_readwrite") {
        if (error) *error = "invalid_breakpoint_kind";
        return -1;
    }
    if (address == 0) {
        if (error) *error = "invalid_breakpoint_address";
        return -1;
    }
    if (!processGlobal && threadId == 0) {
        if (error) *error = "thread_id_required";
        return -1;
    }

    Impl::Breakpoint bp;
    bp.info.kind = kind;
    bp.info.address = address;
    bp.info.size = kind == "hw_execute" ? 1 : size;
    bp.info.pauseOnHit = pauseOnHit;
    bp.info.processGlobal = processGlobal;
    bp.info.targetThreadId = processGlobal ? 0 : threadId;

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        for (const auto& item : impl_->bps) {
            if (item.second.info.kind == kind && item.second.info.address == address &&
                item.second.info.targetThreadId == bp.info.targetThreadId) {
                if (error) *error = "breakpoint_already_exists";
                return -1;
            }
        }
        bp.info.id = impl_->nextBreakpointId++;
        if (kind != "software") {
            for (int slot = 0; slot < 4; ++slot) {
                if (!impl_->hwSlotUsed[slot]) {
                    impl_->hwSlotUsed[slot] = true;
                    bp.hwSlot = slot;
                    break;
                }
            }
            if (bp.hwSlot < 0) {
                if (error) *error = "no_hardware_breakpoint_slot";
                return -1;
            }
        }
    }

    if (kind == "software") {
        uint8_t original = 0;
        if (!impl_->readByte(address, original, error) || original == 0xCC) {
            if (original == 0xCC && error) *error = "preexisting_int3";
            return -1;
        }
        bp.originalByte = original;
        if (!impl_->writeByte(address, 0xCC, error)) return -1;
    } else {
        if (bp.info.size != 1 && bp.info.size != 2 && bp.info.size != 4 && bp.info.size != 8) bp.info.size = 4;
        const auto threads = cortex::target::ListTargetThreads(impl_->target, nullptr);
        bp.info.totalThreads = processGlobal ? threads.size() : 1;
        for (uint64_t tid : threads) {
            if (!processGlobal && tid != threadId) continue;
            if (impl_->configureHardwareThread(static_cast<DWORD>(tid), bp, true, false, nullptr))
                ++bp.info.appliedThreads;
        }
        if (bp.info.appliedThreads == 0) {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->hwSlotUsed[bp.hwSlot] = false;
            if (error) *error = "hardware_breakpoint_apply_failed";
            return -1;
        }
    }

    const int id = bp.info.id;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->bps[id] = std::move(bp);
    }
    return id;
#else
    (void)kindValue; (void)address; (void)size; (void)pauseOnHit; (void)processGlobal; (void)threadId;
    if (error) *error = "windows_debugger_not_supported";
    return -1;
#endif
}

bool WindowsDebuggerBackend::removeBreakpoint(int id, std::string* error) {
#if defined(_WIN32)
    if (error) error->clear();
    Impl::Breakpoint bp;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto it = impl_->bps.find(id);
        if (it == impl_->bps.end()) {
            if (error) *error = "unknown_breakpoint";
            return false;
        }
        bp = it->second;
        impl_->bps.erase(it);
        if (bp.hwSlot >= 0) impl_->hwSlotUsed[bp.hwSlot] = false;
        for (auto pending = impl_->pendingSoftwareRearm.begin(); pending != impl_->pendingSoftwareRearm.end();) {
            if (pending->second == id) pending = impl_->pendingSoftwareRearm.erase(pending);
            else ++pending;
        }
    }
    if (bp.info.kind == "software") {
        uint8_t current = 0;
        if (impl_->readByte(bp.info.address, current, nullptr) && current == 0xCC)
            return impl_->writeByte(bp.info.address, bp.originalByte, error);
        return true;
    }
    for (uint64_t tid : cortex::target::ListTargetThreads(impl_->target, nullptr)) {
        if (!bp.info.processGlobal && tid != bp.info.targetThreadId) continue;
        impl_->configureHardwareThread(static_cast<DWORD>(tid), bp, false, false, nullptr);
    }
    return true;
#else
    (void)id;
    if (error) *error = "windows_debugger_not_supported";
    return false;
#endif
}

std::vector<WindowsDebuggerBackend::BreakpointInfo> WindowsDebuggerBackend::breakpoints() const {
    std::vector<BreakpointInfo> out;
#if defined(_WIN32)
    std::lock_guard<std::mutex> lock(impl_->mutex);
    out.reserve(impl_->bps.size());
    for (const auto& item : impl_->bps) out.push_back(item.second.info);
#endif
    return out;
}

std::vector<WindowsDebuggerBackend::BreakpointLogEntry>
WindowsDebuggerBackend::breakpointLog(int id, uint64_t sinceSeq, size_t limit) const {
    std::vector<BreakpointLogEntry> out;
#if defined(_WIN32)
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto found = impl_->bps.find(id);
    if (found == impl_->bps.end()) return out;
    const size_t cap = limit == 0 ? 500 : std::min<size_t>(limit, 500);
    for (const auto& entry : found->second.log) {
        if (entry.seq < sinceSeq) continue;
        out.push_back(entry);
        if (out.size() >= cap) break;
    }
#else
    (void)id; (void)sinceSeq; (void)limit;
#endif
    return out;
}

std::vector<WindowsDebuggerBackend::PausedThread> WindowsDebuggerBackend::pausedThreads() const {
    std::vector<PausedThread> out;
#if defined(_WIN32)
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->debugPause.active) out.push_back(impl_->debugPause.value);
    for (const auto& item : impl_->manualPaused) {
        cortex::target::ThreadRegisterSnapshot snapshot;
        if (impl_->snapshotFromHandle(item.second, item.first, snapshot, nullptr))
            out.push_back(PausedThread{item.first, -1, std::move(snapshot)});
    }
#endif
    return out;
}

bool WindowsDebuggerBackend::readRegisters(uint64_t threadId,
                                            cortex::target::ThreadRegisterSnapshot& snapshot,
                                            std::string* error) const {
#if defined(_WIN32)
    if (error) error->clear();
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->debugPause.active && impl_->debugPause.value.threadId == threadId) {
            snapshot = impl_->debugPause.value.registers;
            return true;
        }
        const auto manual = impl_->manualPaused.find(static_cast<DWORD>(threadId));
        if (manual != impl_->manualPaused.end())
            return impl_->snapshotFromHandle(manual->second, static_cast<DWORD>(threadId), snapshot, error);
    }
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION,
                               FALSE, static_cast<DWORD>(threadId));
    if (!thread) {
        if (error) *error = "thread_open_failed:" + std::to_string(GetLastError());
        return false;
    }
    if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
        if (error) *error = "thread_suspend_failed:" + std::to_string(GetLastError());
        CloseHandle(thread);
        return false;
    }
    const bool ok = impl_->snapshotFromHandle(thread, static_cast<DWORD>(threadId), snapshot, error);
    ResumeThread(thread);
    CloseHandle(thread);
    return ok;
#else
    (void)threadId; (void)snapshot;
    if (error) *error = "windows_debugger_not_supported";
    return false;
#endif
}

bool WindowsDebuggerBackend::pauseThread(uint64_t threadId,
                                         cortex::target::ThreadRegisterSnapshot& snapshot,
                                         std::string* error) {
#if defined(_WIN32)
    if (error) error->clear();
    if (!ensureAttached(error) || threadId == 0) return false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->debugPause.active && impl_->debugPause.value.threadId == threadId) {
            snapshot = impl_->debugPause.value.registers;
            return true;
        }
        if (impl_->manualPaused.find(static_cast<DWORD>(threadId)) != impl_->manualPaused.end()) {
            if (error) *error = "thread_already_paused";
            return false;
        }
    }
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION,
                               FALSE, static_cast<DWORD>(threadId));
    if (!thread) {
        if (error) *error = "thread_open_failed:" + std::to_string(GetLastError());
        return false;
    }
    const DWORD previous = SuspendThread(thread);
    if (previous == static_cast<DWORD>(-1)) {
        if (error) *error = "thread_suspend_failed:" + std::to_string(GetLastError());
        CloseHandle(thread);
        return false;
    }
    if (previous != 0) {
        ResumeThread(thread);
        CloseHandle(thread);
        if (error) *error = "thread_already_suspended_by_other_actor";
        return false;
    }
    if (!impl_->snapshotFromHandle(thread, static_cast<DWORD>(threadId), snapshot, error)) {
        ResumeThread(thread);
        CloseHandle(thread);
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->manualPaused[static_cast<DWORD>(threadId)] = thread;
    return true;
#else
    (void)threadId; (void)snapshot;
    if (error) *error = "windows_debugger_not_supported";
    return false;
#endif
}

bool WindowsDebuggerBackend::continueThread(uint64_t threadId, std::string* error) {
#if defined(_WIN32)
    if (error) error->clear();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->debugPause.active && impl_->debugPause.value.threadId == threadId) {
        impl_->resumeCommand = Impl::ResumeCommand::Continue;
        impl_->cv.notify_all();
        return true;
    }
    const auto manual = impl_->manualPaused.find(static_cast<DWORD>(threadId));
    if (manual != impl_->manualPaused.end()) {
        ResumeThread(manual->second);
        CloseHandle(manual->second);
        impl_->manualPaused.erase(manual);
        return true;
    }
    if (error) *error = "thread_not_paused";
    return false;
#else
    (void)threadId;
    if (error) *error = "windows_debugger_not_supported";
    return false;
#endif
}

bool WindowsDebuggerBackend::stepThread(uint64_t threadId, uint32_t timeoutMs,
                                        cortex::target::ThreadRegisterSnapshot& snapshot,
                                        std::string* error) {
#if defined(_WIN32)
    if (error) error->clear();
    if (threadId == 0 || !ensureAttached(error)) return false;
    timeoutMs = std::clamp<uint32_t>(timeoutMs, 100, 120000);
    uint64_t before = 0;
    {
        std::unique_lock<std::mutex> lock(impl_->mutex);
        before = impl_->stepGeneration;
        if (impl_->debugPause.active && impl_->debugPause.value.threadId == threadId) {
            impl_->requestedStepThread = static_cast<DWORD>(threadId);
            impl_->resumeCommand = Impl::ResumeCommand::Step;
            impl_->cv.notify_all();
        } else {
            const auto manual = impl_->manualPaused.find(static_cast<DWORD>(threadId));
            if (manual == impl_->manualPaused.end()) {
                if (error) *error = "thread_not_paused";
                return false;
            }
            HANDLE thread = manual->second;
            if (!impl_->setExecutionState(thread, std::nullopt, true, false, error)) return false;
            impl_->requestedStepThread = static_cast<DWORD>(threadId);
            ResumeThread(thread);
            CloseHandle(thread);
            impl_->manualPaused.erase(manual);
        }
        if (!impl_->stepCv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
                return impl_->stepGeneration != before || impl_->stop.load();
            })) {
            if (error) *error = "debug_step_timeout";
            return false;
        }
        if (impl_->stepGeneration == before) {
            if (error) *error = "debug_step_interrupted";
            return false;
        }
        snapshot = impl_->stepSnapshot;
        return true;
    }
#else
    (void)threadId; (void)timeoutMs; (void)snapshot;
    if (error) *error = "windows_debugger_not_supported";
    return false;
#endif
}

bool WindowsDebuggerBackend::stepOverThread(uint64_t threadId, uint32_t timeoutMs,
                                            cortex::target::ThreadRegisterSnapshot& snapshot,
                                            std::string* error) {
#if defined(_WIN32)
    if (error) error->clear();
    timeoutMs = std::clamp<uint32_t>(timeoutMs, 100, 120000);
    cortex::target::ThreadRegisterSnapshot current;
    if (!readRegisters(threadId, current, error)) return false;
    std::vector<cortex::services::DisassemblyInstruction> instructions;
    if (!impl_->disassembly.Decode(current.instructionPointer, 1, instructions, error) || instructions.empty() ||
        Lower(instructions.front().mnemonic) != "call") {
        if (error) error->clear();
        return stepThread(threadId, timeoutMs, snapshot, error);
    }
    const uint64_t next = current.instructionPointer + instructions.front().bytes.size();
    if (next <= current.instructionPointer) {
        if (error) *error = "step_over_invalid_instruction_size";
        return false;
    }

    uint64_t before = 0;
    int sourceSoftwareId = -1;
    bool debugPause = false;
    HANDLE manualHandle = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        before = impl_->stepGeneration;
        if (impl_->debugPause.active && impl_->debugPause.value.threadId == threadId) {
            debugPause = true;
            const auto pending = impl_->pendingSoftwareRearm.find(static_cast<DWORD>(threadId));
            if (pending != impl_->pendingSoftwareRearm.end()) {
                sourceSoftwareId = pending->second;
                impl_->pendingSoftwareRearm.erase(pending);
            }
        } else {
            const auto manual = impl_->manualPaused.find(static_cast<DWORD>(threadId));
            if (manual == impl_->manualPaused.end()) {
                if (error) *error = "thread_not_paused";
                return false;
            }
            manualHandle = manual->second;
        }
    }

    if (!impl_->installTempStepOver(static_cast<DWORD>(threadId), next, sourceSoftwareId, error)) {
        if (sourceSoftwareId >= 0) {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->pendingSoftwareRearm[static_cast<DWORD>(threadId)] = sourceSoftwareId;
        }
        return false;
    }

    {
        std::unique_lock<std::mutex> lock(impl_->mutex);
        impl_->requestedStepThread = static_cast<DWORD>(threadId);
        if (debugPause) {
            impl_->resumeCommand = Impl::ResumeCommand::Continue;
            impl_->cv.notify_all();
        } else {
            ResumeThread(manualHandle);
            CloseHandle(manualHandle);
            impl_->manualPaused.erase(static_cast<DWORD>(threadId));
        }
        if (!impl_->stepCv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
                return impl_->stepGeneration != before || impl_->stop.load();
            })) {
            if (error) *error = "debug_step_over_timeout";
            return false;
        }
        if (impl_->stepGeneration == before) {
            if (error) *error = "debug_step_over_interrupted";
            return false;
        }
        snapshot = impl_->stepSnapshot;
        return true;
    }
#else
    (void)threadId; (void)timeoutMs; (void)snapshot;
    if (error) *error = "windows_debugger_not_supported";
    return false;
#endif
}
