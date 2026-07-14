#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace remotecall {

enum class Convention { Cdecl, Stdcall, Thiscall };

struct CallResult {
    bool ok;
    uint64_t returnValue;
    std::string error;
};

// Installs the vectored exception handler used to catch crashes during
// Invoke(). Call once during DLL init/shutdown, like core/debugger does.
void Init();
void Shutdown();

// Calls the function at `address` with `args` (0-8 pointer-width values) on
// the calling (HTTP worker) thread -- NOT marshalled onto any of the game's
// own threads, so avoid calling functions that assume they only ever run on
// a specific thread (e.g. the render thread) unless that's known to be safe
// for this target. `conv` selects the x86 calling convention; ignored on
// x64, which has a single ABI.
//
// A vectored exception handler + setjmp/longjmp catches an access violation
// or illegal instruction raised during the call and turns it into a failed
// CallResult instead of crashing the host process -- but this is best
// effort: stack corruption from a wrong arg count/convention, or a crash on
// another thread the call kicks off, can still bring the game down.
CallResult Invoke(uintptr_t address, const std::vector<uintptr_t>& args, Convention conv);

} // namespace remotecall
