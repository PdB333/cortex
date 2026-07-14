#include "debugger/debugger.h"

namespace dbg {
std::vector<DWORD> ListThreadIds() { return {GetCurrentThreadId()}; }
}
