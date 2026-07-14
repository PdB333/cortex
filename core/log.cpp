#include "log.h"
#include "config.h"
#include <windows.h>
#include <cstdarg>
#include <mutex>
#include <chrono>

namespace dbglog {

namespace {
    std::mutex g_mutex;
    std::string g_path;
}

void Init() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_path = config::GetModuleDir() + "\\cortex_debug.log";
    FILE* f = nullptr;
    fopen_s(&f, g_path.c_str(), "w");
    if (f) fclose(f);
}

void Line(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_path.empty()) return;
    FILE* f = nullptr;
    fopen_s(&f, g_path.c_str(), "a");
    if (!f) return;

    // Wall-clock timestamp so gaps between lines (e.g. an engine hitch
    // stalling EndScene) are measurable after the fact instead of only
    // being inferable from log-line ordering.
    using namespace std::chrono;
    auto now = system_clock::now();
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    time_t tt = system_clock::to_time_t(now);
    tm local_tm;
    localtime_s(&local_tm, &tt);
    fprintf(f, "[%02d:%02d:%02d.%03d] ", local_tm.tm_hour, local_tm.tm_min, local_tm.tm_sec,
            static_cast<int>(ms.count()));

    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fprintf(f, "\n");
    fclose(f);
}

} // namespace dbglog
