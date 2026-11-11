// Owned compile-only PDB fixture. The produced executable is never run.
struct Point { int x; int y; };
volatile int cortex_sink = 0;
__declspec(noinline) int CortexPdbAdd(Point p) { return p.x + p.y; }
extern "C" int cortex_entry() { Point p = {7, 11}; cortex_sink = CortexPdbAdd(p); return cortex_sink; }
