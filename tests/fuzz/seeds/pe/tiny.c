__declspec(dllexport) int Add(int a, int b) { return a + b; }
__declspec(dllexport) int Loop(int n) { int s = 0; for (int i = 0; i < n; ++i) { if (i & 1) s += i; else s ^= i; } return s; }
static const char msg[] = "cortex tiny fixture";
int entry(void) { return Loop(Add(3, 4)) + (int)sizeof(msg); }
