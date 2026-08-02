#pragma once

#include <windows.h>
#include <stdint.h>

#define CORTEX_DIAG_ABI_VERSION 1u

#if defined(CORTEX_DIAG_EXPORTS)
#define CORTEX_DIAG_API __declspec(dllexport)
#else
#define CORTEX_DIAG_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CortexDiagModInfo {
    uint32_t struct_size;
    uint32_t abi_version;
    HMODULE module;
    const char* name;
    const char* version;
    const char* author;
    const char* git_commit;
    const char* build_id;
    const char* source_root;
    const char* symbol_path;
} CortexDiagModInfo;

typedef struct CortexDiagHookInfo {
    uint32_t struct_size;
    uint32_t abi_version;
    HMODULE owner_module;
    const char* name;
    const char* library;
    uintptr_t target;
    uintptr_t detour;
    uintptr_t trampoline;
    uint32_t overwrite_size;
    const uint8_t* original_bytes;
    uint32_t original_size;
    const uint8_t* installed_bytes;
    uint32_t installed_size;
} CortexDiagHookInfo;

CORTEX_DIAG_API BOOL CortexDiagRegisterMod(const CortexDiagModInfo* info);
CORTEX_DIAG_API void CortexDiagUnregisterMod(HMODULE module);
CORTEX_DIAG_API void CortexDiagBreadcrumb(const char* category, const char* message);
CORTEX_DIAG_API void CortexDiagHeartbeat(const char* source);

CORTEX_DIAG_API uint64_t CortexDiagScopeEnter(const char* name, const char* file, int line);
CORTEX_DIAG_API void CortexDiagScopeExit(uint64_t scope_id);

CORTEX_DIAG_API void CortexDiagValuePointer(const char* name, const void* value);
CORTEX_DIAG_API void CortexDiagValueInt64(const char* name, int64_t value);
CORTEX_DIAG_API void CortexDiagValueUInt64(const char* name, uint64_t value);
CORTEX_DIAG_API void CortexDiagValueDouble(const char* name, double value);
CORTEX_DIAG_API void CortexDiagValueBool(const char* name, BOOL value);
CORTEX_DIAG_API void CortexDiagValueText(const char* name, const char* value);

CORTEX_DIAG_API uint64_t CortexDiagRegisterHook(const CortexDiagHookInfo* info);
CORTEX_DIAG_API void CortexDiagUnregisterHook(uint64_t hook_id);
CORTEX_DIAG_API uint32_t CortexDiagHookEnter(uint64_t hook_id);
CORTEX_DIAG_API void CortexDiagHookLeave(uint64_t hook_id);
CORTEX_DIAG_API void CortexDiagHookException(uint64_t hook_id, DWORD exception_code);

#ifdef __cplusplus
}
#endif
