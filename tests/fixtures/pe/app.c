#include <windows.h>
int WINAPI WinMain(HINSTANCE h, HINSTANCE p, LPSTR c, int s) {
    char text[64];
    wsprintfA(text, "Cortex fixture %d", GetTickCount() & 7);
    if (GetEnvironmentVariableA("CORTEX_FIXTURE_SHOW", NULL, 0)) MessageBoxA(NULL, text, "fixture", MB_OK);
    return 0;
}
