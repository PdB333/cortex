#pragma once

#include <windows.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

// The injected core provides the ABI exports. Public mod code includes
// diag_c.h directly without CORTEX_DIAG_EXPORTS, so it remains a runtime-only
// consumer with no import-library dependency.
#ifndef CORTEX_DIAG_EXPORTS
#define CORTEX_DIAG_EXPORTS 1
#define CORTEX_DIAG_UNDEFINE_EXPORTS 1
#endif
#include "../../sdk/include/cortex/diag_c.h"
#ifdef CORTEX_DIAG_UNDEFINE_EXPORTS
#undef CORTEX_DIAG_UNDEFINE_EXPORTS
#undef CORTEX_DIAG_EXPORTS
#endif

#include <cstddef>
#include <cstdint>
#include <string>

namespace diagnostics {

constexpr size_t kBreadcrumbCapacity = 512;
constexpr size_t kBreadcrumbCategorySize = 32;
constexpr size_t kBreadcrumbMessageSize = 160;
constexpr size_t kCrashPathSize = 1024;

struct Options {
    bool enabled = true;
    bool writeMinidump = true;
    std::string outputDirectory;
};

enum class AccessType : uint8_t {
    Unknown = 0,
    Read,
    Write,
    Execute,
};

struct Breadcrumb {
    uint64_t sequence = 0;
    uint64_t timestampMs = 0;
    DWORD threadId = 0;
    uintptr_t caller = 0;
    char category[kBreadcrumbCategorySize]{};
    char message[kBreadcrumbMessageSize]{};
};

struct CrashContext {
    uint32_t schemaVersion = 1;
    DWORD processId = 0;
    DWORD threadId = 0;
    DWORD exceptionCode = 0;
    uintptr_t instruction = 0;
    uintptr_t accessedAddress = 0;
    AccessType accessType = AccessType::Unknown;
    uintptr_t moduleBase = 0;
    uintptr_t moduleRva = 0;
    char moduleName[MAX_PATH]{};
    char modulePath[kCrashPathSize]{};
    CONTEXT registers{};
};

// Installs a last-chance Windows unhandled-exception filter. Unlike the
// debugger VEH, this runs only after normal SEH/VEH handlers declined the
// exception, avoiding crash reports for exceptions that a game recovers from.
bool Init(const Options& options);
void Shutdown();
bool IsEnabled();

// Fixed-capacity, process-wide diagnostic breadcrumbs. Old entries are
// overwritten after kBreadcrumbCapacity events. Safe to call from hot hooks.
void BreadcrumbLog(const char* category, const char* message, uintptr_t caller = 0);
size_t SnapshotBreadcrumbs(Breadcrumb* output, size_t capacity, uint64_t* dropped = nullptr);

#ifdef CORTEX_DIAGNOSTICS_TESTING
namespace testing {
void ResetState();
bool EscapeJson(const char* input, char* output, size_t outputSize);
bool BuildCrashContext(PEXCEPTION_POINTERS info, CrashContext& output);
bool WriteReport(const char* directory, const CrashContext& context,
                 bool dumpAttempted, bool dumpWritten, DWORD dumpError);
} // namespace testing
#endif

} // namespace diagnostics
