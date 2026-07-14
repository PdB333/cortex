#include "symbols.h"

#include <windows.h>
#include <dbghelp.h>
#include <mutex>
#include <cstring>

namespace symbols {

namespace {
std::mutex g_mutex; // DbgHelp is not thread-safe; serialize all calls
bool g_initialized = false;
}

void Init() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_initialized) return;
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS);
    g_initialized = SymInitialize(GetCurrentProcess(), nullptr, TRUE) == TRUE;
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized) return;
    SymCleanup(GetCurrentProcess());
    g_initialized = false;
}

std::optional<SymbolInfo> Resolve(uintptr_t address) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized) return std::nullopt;

    uint8_t buf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
    PSYMBOL_INFO sym = reinterpret_cast<PSYMBOL_INFO>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = MAX_SYM_NAME;

    DWORD64 displacement = 0;
    if (!SymFromAddr(GetCurrentProcess(), static_cast<DWORD64>(address), &displacement, sym)) {
        return std::nullopt;
    }

    SymbolInfo out;
    out.name.assign(sym->Name, sym->NameLen);
    out.symbolAddress = static_cast<uintptr_t>(sym->Address);
    out.displacement = displacement;
    return out;
}

std::optional<LineInfo> ResolveLine(uintptr_t address) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized) return std::nullopt;

    IMAGEHLP_LINE64 line = {};
    line.SizeOfStruct = sizeof(line);
    DWORD displacement = 0;
    if (!SymGetLineFromAddr64(GetCurrentProcess(), static_cast<DWORD64>(address), &displacement, &line)) {
        return std::nullopt;
    }

    LineInfo out;
    out.file = line.FileName ? line.FileName : "";
    out.line = line.LineNumber;
    return out;
}

std::optional<uintptr_t> Lookup(const std::string& name) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized) return std::nullopt;

    uint8_t buf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
    PSYMBOL_INFO sym = reinterpret_cast<PSYMBOL_INFO>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = MAX_SYM_NAME;

    if (!SymFromName(GetCurrentProcess(), name.c_str(), sym)) return std::nullopt;
    return static_cast<uintptr_t>(sym->Address);
}

} // namespace symbols
