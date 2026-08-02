#define CORTEX_DIAGNOSTICS_TESTING 1

// The injected DLL currently compiles both implementation files into its
// lifecycle translation unit. This smoke target catches symbol collisions and
// linkage regressions in that exact arrangement.
#include "../core/diagnostics/diagnostics.cpp"
#include "../core/diagnostics/registry.cpp"

int main() {
    diagnostics::testing::ResetState();
    diagnostics::testing::ResetRegistry();
    return 0;
}
