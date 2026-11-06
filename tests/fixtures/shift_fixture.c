/* Two builds of this file stand for two versions of one program
 * (cortex_evidence_tests): build B (CORTEX_SHIFT) inserts a function
 * before cortex_shift_target, so the target moves to another RVA with the
 * same code, and changes cortex_shift_changed. Never run by the tests. */
#include <stdio.h>

static int g_counter;

#ifdef CORTEX_SHIFT
__declspec(dllexport) __attribute__((noinline)) int cortex_shift_padding(int x) {
    int sum = 0;
    for (int i = 0; i < x; ++i) sum += (i * i) ^ x;
    return sum;
}
#endif

__declspec(dllexport) __attribute__((noinline)) int cortex_shift_target(int amount) {
    g_counter += amount;
    if (g_counter > 100) g_counter = 100;
    return g_counter * 3 + amount;
}

__declspec(dllexport) __attribute__((noinline)) int cortex_shift_changed(int value) {
#ifdef CORTEX_SHIFT
    return value * 7 - 3;
#else
    return value * 5 + 1;
#endif
}

int main(int argc, char** argv) {
    (void)argv;
    int result = 0;
    for (int i = 0; i < argc; ++i) {
        result += cortex_shift_target(i) + cortex_shift_changed(i);
#ifdef CORTEX_SHIFT
        result += cortex_shift_padding(i);
#endif
    }
    printf("%d\n", result);
    return 0;
}
