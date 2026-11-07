#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
static LONG WINAPI Filter(EXCEPTION_POINTERS* info) {
    HANDLE file = CreateFileA("crash.dmp", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    MINIDUMP_EXCEPTION_INFORMATION mei = {GetCurrentThreadId(), info, FALSE};
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpNormal, &mei, NULL, NULL);
    CloseHandle(file);
    ExitProcess(3);
    return EXCEPTION_EXECUTE_HANDLER;
}
__declspec(noinline) void CortexFixtureCrash(volatile int* p) { *p = 42; }
int main(void) {
    SetUnhandledExceptionFilter(Filter);
    CortexFixtureCrash((volatile int*)0x10);
    return 0;
}
