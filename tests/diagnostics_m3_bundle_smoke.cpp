#define CORTEX_DIAGNOSTICS_TESTING 1

#include "../core/diagnostics/diagnostics.cpp"
#include "../core/diagnostics/registry.cpp"
#include "../core/diagnostics/symbolizer.cpp"

int main() {
    diagnostics::testing::ResetState();
    diagnostics::testing::ResetRegistry();
    diagnostics::testing::ResetSymbolizer();
    return 0;
}
