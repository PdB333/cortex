#include "call.h"
#include <windows.h>
#include <csetjmp>
#include <cstdio>

namespace remotecall {

namespace {

thread_local jmp_buf g_jmpBuf;
thread_local bool g_inCall = false;
thread_local DWORD g_lastExceptionCode = 0;

PVOID g_vehHandle = nullptr;

// Runs *before* the SEH-level handlers the game itself might have
// installed, since Init() registers it with AddVectoredExceptionHandler(1, ...)
// (first in the chain). Only intercepts exceptions raised while this
// thread is inside Invoke() -- any other exception is passed on untouched.
LONG CALLBACK CallVEH(PEXCEPTION_POINTERS info) {
    if (g_inCall) {
        g_lastExceptionCode = info->ExceptionRecord->ExceptionCode;
        longjmp(g_jmpBuf, 1);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// Generates Dispatch_<SUFFIX>(addr, args), a switch over 0-8 args that
// reinterpret_casts `addr` to a function pointer of the matching arity and
// calling convention, then calls it. A real signature (not a variadic one)
// is required so the compiler emits the correct argument-popping code for
// stdcall/thiscall -- calling through a mismatched arity is exactly the
// kind of crash Init()'s VEH exists to catch.
#define CORTEX_DEFINE_DISPATCH(CONV, SUFFIX)                                                                     \
    typedef uintptr_t(CONV *Fn0_##SUFFIX)();                                                                     \
    typedef uintptr_t(CONV *Fn1_##SUFFIX)(uintptr_t);                                                            \
    typedef uintptr_t(CONV *Fn2_##SUFFIX)(uintptr_t, uintptr_t);                                                 \
    typedef uintptr_t(CONV *Fn3_##SUFFIX)(uintptr_t, uintptr_t, uintptr_t);                                      \
    typedef uintptr_t(CONV *Fn4_##SUFFIX)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);                           \
    typedef uintptr_t(CONV *Fn5_##SUFFIX)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);                \
    typedef uintptr_t(CONV *Fn6_##SUFFIX)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);     \
    typedef uintptr_t(CONV *Fn7_##SUFFIX)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t,      \
                                           uintptr_t);                                                           \
    typedef uintptr_t(CONV *Fn8_##SUFFIX)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t,      \
                                           uintptr_t, uintptr_t);                                                \
    uintptr_t Dispatch_##SUFFIX(void* addr, const std::vector<uintptr_t>& a) {                                  \
        switch (a.size()) {                                                                                      \
            case 0: return reinterpret_cast<Fn0_##SUFFIX>(addr)();                                               \
            case 1: return reinterpret_cast<Fn1_##SUFFIX>(addr)(a[0]);                                           \
            case 2: return reinterpret_cast<Fn2_##SUFFIX>(addr)(a[0], a[1]);                                     \
            case 3: return reinterpret_cast<Fn3_##SUFFIX>(addr)(a[0], a[1], a[2]);                               \
            case 4: return reinterpret_cast<Fn4_##SUFFIX>(addr)(a[0], a[1], a[2], a[3]);                         \
            case 5: return reinterpret_cast<Fn5_##SUFFIX>(addr)(a[0], a[1], a[2], a[3], a[4]);                   \
            case 6: return reinterpret_cast<Fn6_##SUFFIX>(addr)(a[0], a[1], a[2], a[3], a[4], a[5]);             \
            case 7: return reinterpret_cast<Fn7_##SUFFIX>(addr)(a[0], a[1], a[2], a[3], a[4], a[5], a[6]);       \
            case 8: return reinterpret_cast<Fn8_##SUFFIX>(addr)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]); \
            default: return 0;                                                                                   \
        }                                                                                                        \
    }

CORTEX_DEFINE_DISPATCH(__cdecl, Cdecl)
CORTEX_DEFINE_DISPATCH(__stdcall, Stdcall)
CORTEX_DEFINE_DISPATCH(__thiscall, Thiscall)

#undef CORTEX_DEFINE_DISPATCH

} // namespace

void Init() {
    if (g_vehHandle) return;
    g_vehHandle = AddVectoredExceptionHandler(1, CallVEH);
}

void Shutdown() {
    if (g_vehHandle) {
        RemoveVectoredExceptionHandler(g_vehHandle);
        g_vehHandle = nullptr;
    }
}

CallResult Invoke(uintptr_t address, const std::vector<uintptr_t>& args, Convention conv) {
    if (args.size() > 8) return {false, 0, "too_many_args_max_8"};
    void* addr = reinterpret_cast<void*>(address);

    g_inCall = true;
    if (setjmp(g_jmpBuf) == 0) {
        uintptr_t ret;
        switch (conv) {
            case Convention::Stdcall: ret = Dispatch_Stdcall(addr, args); break;
            case Convention::Thiscall: ret = Dispatch_Thiscall(addr, args); break;
            default: ret = Dispatch_Cdecl(addr, args); break;
        }
        g_inCall = false;
        return {true, static_cast<uint64_t>(ret), ""};
    } else {
        g_inCall = false;
        char buf[64];
        snprintf(buf, sizeof(buf), "exception_0x%08lX_during_call", static_cast<unsigned long>(g_lastExceptionCode));
        return {false, 0, std::string(buf)};
    }
}

} // namespace remotecall
