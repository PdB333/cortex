#include "modules.h"
#include <windows.h>
#include <tlhelp32.h>

namespace process {

std::vector<ModuleInfo> ListModules() {
    std::vector<ModuleInfo> result;

    DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return result;

    MODULEENTRY32 me;
    me.dwSize = sizeof(me);
    if (Module32First(snap, &me)) {
        do {
            ModuleInfo info;
            info.name = me.szModule;
            info.base = reinterpret_cast<uintptr_t>(me.modBaseAddr);
            info.size = me.modBaseSize;
            result.push_back(std::move(info));
        } while (Module32Next(snap, &me));
    }

    CloseHandle(snap);
    return result;
}

} // namespace process
