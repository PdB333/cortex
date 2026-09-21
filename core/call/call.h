#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace remotecall {

enum class Convention { Cdecl, Stdcall, Thiscall, Fastcall };

struct CallResult {
    bool ok = false;
    uint64_t returnValue = 0;
    std::string error;
    uint32_t threadId = 0;
    uint32_t exceptionCode = 0;
    uint64_t durationMs = 0;
};

// Installs the exception guard used by every native call.
void Init();
// Returns false when a call is still executing on another target thread. The
// DLL must not be unloaded while such a call can still return into Cortex.
bool Shutdown();

// Executes immediately on the calling thread. Kept for backwards
// compatibility with /call/function; prefer the scheduled variants below.
CallResult Invoke(uintptr_t address, const std::vector<uintptr_t>& args, Convention conv);

// Called once per rendered frame from Cortex's render hook. The first thread
// that reaches this pump becomes the current game/frame thread. Queued calls
// are executed here, before Cortex's fallback overlay work for that frame.
void PumpGameThread();

// Queues a call for the next frame and waits up to timeoutMs for completion.
CallResult InvokeOnGameThread(uintptr_t address,
                              const std::vector<uintptr_t>& args,
                              Convention conv,
                              uint32_t timeoutMs);

// Executes on a specific target thread without context hijacking. If the
// requested thread is the observed game/frame thread this uses the frame
// queue. Other threads are dispatched through a temporary WH_GETMESSAGE hook
// and therefore must own a Windows message queue. This deliberately fails
// cleanly for non-dispatchable worker threads instead of rewriting their CPU
// context/stack behind the game's back.
CallResult InvokeOnThread(uint32_t threadId,
                          uintptr_t address,
                          const std::vector<uintptr_t>& args,
                          Convention conv,
                          uint32_t timeoutMs);

uint32_t GameThreadId();
uint64_t LastGameThreadPumpMs();
size_t PendingGameThreadCalls();

} // namespace remotecall
