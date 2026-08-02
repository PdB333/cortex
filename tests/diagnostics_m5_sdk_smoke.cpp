#include <cortex/diag.h>

int main() {
    CORTEX_DIAG_HEARTBEAT("render");
    cortex::diag::Heartbeat("game_loop");
    return 0;
}
