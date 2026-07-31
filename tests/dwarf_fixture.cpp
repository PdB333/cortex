#include <windows.h>
#include <cstdint>
#include <cstdio>

__attribute__((noinline)) int DwarfTarget(int value) {
    volatile int result = value * 3 + 1;
    return result;
}

int main() {
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    const uintptr_t address = reinterpret_cast<uintptr_t>(&DwarfTarget);
    std::printf("0x%llX\n", static_cast<unsigned long long>(address - base));
    return DwarfTarget(7) == 22 ? 0 : 1;
}
