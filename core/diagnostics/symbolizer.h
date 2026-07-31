#pragma once

#include <windows.h>

#include <cstddef>
#include <string>

namespace diagnostics {

struct SymbolizerOptions {
    bool enabled = true;
    std::string crashOutputDirectory;
    std::string symbolSearchPath;
    std::string externalToolPath;
    size_t maxFrames = 64;
};

bool SymbolizerInit(const SymbolizerOptions& options);
void SymbolizerShutdown();
bool IsSymbolizerEnabled();

// Writes stack.json, build_info.json and report.txt into an existing crash
// directory. It is best-effort and preserves module+RVA data when symbols are
// unavailable or DbgHelp is busy.
bool WriteSymbolizedCrash(const char* directory, PEXCEPTION_POINTERS exceptionPointers);

#ifdef CORTEX_DIAGNOSTICS_TESTING
namespace testing {
void ResetSymbolizer();
}
#endif

} // namespace diagnostics
