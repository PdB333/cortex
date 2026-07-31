#include <cortex/diag.h>

#include <cstdint>

int main() {
    const uint64_t hook = cortex::diag::RegisterHook(
        GetModuleHandleA(nullptr), "AbsentCortexHook", "custom",
        reinterpret_cast<uintptr_t>(&main), reinterpret_cast<uintptr_t>(&main), 0, 5);
    {
        cortex::diag::HookInvocation invocation(hook);
        cortex::diag::HookException(hook, EXCEPTION_ACCESS_VIOLATION);
    }
    cortex::diag::UnregisterHook(hook);
    return hook == 0 ? 0 : 1;
}
