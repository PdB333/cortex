// The diagnostics sources are intentionally included into the injected core's
// single translation unit. Remap private helper names that also exist in M1/M2
// so their anonymous namespaces cannot collide in that combined build.
#define Lower CortexSymbolizerLower
#define ExceptionName CortexSymbolizerExceptionName
#define FindNewestCrashDirectory CortexSymbolizerFindNewestCrashDirectory

#include "symbolizer_part1.inc"
#include "symbolizer_part2.inc"
#include "symbolizer_part3.inc"

#undef FindNewestCrashDirectory
#undef ExceptionName
#undef Lower
