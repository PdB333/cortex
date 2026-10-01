#include "config.h"

#include <cstdio>

int main() {
    const config::Config defaults{};
    if (defaults.log_console) {
        std::fputs("FAIL: config::Config{}.log_console must default to false\n", stderr);
        return 1;
    }
    if (defaults.http_api_enabled) {
        std::fputs("FAIL: config::Config{}.http_api_enabled must default to false\n", stderr);
        return 2;
    }
    if (!defaults.project_directory.empty() || !defaults.session_directory.empty()) {
        std::fputs("FAIL: project/session directories must use runtime-local defaults\n", stderr);
        return 3;
    }
    if (defaults.session_history_limit != 25) {
        std::fputs("FAIL: session history retention must default to 25\n", stderr);
        return 4;
    }
    std::puts("PASS: Cortex runtime defaults are silent, native-pipe first, and bounded");
    return 0;
}
