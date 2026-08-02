#include "../sdk/include/cortex/diag.h"

void InstrumentedUpdate(void* player, int health) {
    CORTEX_DIAG_SCOPE("InstrumentedUpdate");
    CORTEX_DIAG_POINTER("player", player);
    CORTEX_DIAG_VALUE("health", health);
    CORTEX_DIAG_VALUE("ready", true);
    CORTEX_DIAG_VALUE("state", "running");
    CORTEX_DIAG_BREADCRUMB("update completed");
}

int main() {
    InstrumentedUpdate(nullptr, 100);
    return 0;
}
