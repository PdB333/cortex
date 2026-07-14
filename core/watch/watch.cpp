#include "watch.h"
#include "../memory/memory.h"
#include "../events/events.h"

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>

namespace watch {

namespace {

// Lighter than freeze's 16ms -- a watch only needs to notice a change
// promptly enough to beat an AI polling over HTTP every few seconds, not to
// fight the game for control of the value every frame.
constexpr DWORD kIntervalMs = 100;
constexpr size_t kMaxEvents = 1000;

struct Entry {
    uintptr_t address;
    std::string type;
    std::string label;
    double lastValue = 0.0;
    bool hasValue = false;
};

std::mutex g_mutex;
std::map<int, Entry> g_entries;
int g_nextId = 1;
std::deque<ChangeEvent> g_events;
std::atomic<bool> g_running{false};
HANDLE g_thread = nullptr;

size_t TypeSize(const std::string& type) {
    if (type == "i8" || type == "u8") return 1;
    if (type == "i16" || type == "u16") return 2;
    if (type == "i32" || type == "u32" || type == "float") return 4;
    if (type == "i64" || type == "u64" || type == "double") return 8;
    return 0;
}

bool IsFloatType(const std::string& type) { return type == "float" || type == "double"; }

double BytesToDouble(const uint8_t* raw, const std::string& type) {
    if (type == "i8") { int8_t v; memcpy(&v, raw, 1); return v; }
    if (type == "u8") { uint8_t v; memcpy(&v, raw, 1); return v; }
    if (type == "i16") { int16_t v; memcpy(&v, raw, 2); return v; }
    if (type == "u16") { uint16_t v; memcpy(&v, raw, 2); return v; }
    if (type == "i32") { int32_t v; memcpy(&v, raw, 4); return v; }
    if (type == "u32") { uint32_t v; memcpy(&v, raw, 4); return v; }
    if (type == "i64") { int64_t v; memcpy(&v, raw, 8); return static_cast<double>(v); }
    if (type == "u64") { uint64_t v; memcpy(&v, raw, 8); return static_cast<double>(v); }
    if (type == "float") { float v; memcpy(&v, raw, 4); return v; }
    if (type == "double") { double v; memcpy(&v, raw, 8); return v; }
    return 0.0;
}

bool ValuesEqual(const std::string& type, double a, double b) {
    if (IsFloatType(type)) return std::fabs(a - b) < 0.00001;
    return a == b;
}

std::string FormatValue(const std::string& type, double v) {
    char buf[64];
    if (IsFloatType(type)) snprintf(buf, sizeof(buf), "%g", v);
    else snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
    return buf;
}

long long NowMs() { return static_cast<long long>(GetTickCount64()); }

// Caller must hold g_mutex.
void PushEvent(ChangeEvent ev) {
    events::Publish("watch.change", "{\"watch_id\":" + std::to_string(ev.watch_id) + "}");
    g_events.push_back(std::move(ev));
    while (g_events.size() > kMaxEvents) g_events.pop_front();
}

DWORD WINAPI WatchThread(LPVOID) {
    while (g_running) {
        std::vector<std::pair<int, uintptr_t>> toRead;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            for (auto& [id, e] : g_entries) toRead.emplace_back(id, e.address);
        }

        for (auto& [id, addr] : toRead) {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_entries.find(id);
            if (it == g_entries.end()) continue;
            Entry& e = it->second;

            std::vector<uint8_t> buf;
            size_t size = TypeSize(e.type);
            if (size == 0 || !memory::ReadBytes(e.address, size, buf)) continue;
            double current = BytesToDouble(buf.data(), e.type);

            if (!e.hasValue) {
                e.lastValue = current;
                e.hasValue = true;
                continue;
            }
            if (!ValuesEqual(e.type, current, e.lastValue)) {
                PushEvent(ChangeEvent{id, e.address, e.label, FormatValue(e.type, e.lastValue),
                                       FormatValue(e.type, current), NowMs()});
                e.lastValue = current;
            }
        }

        Sleep(kIntervalMs);
    }
    return 0;
}

} // namespace

void Init() {
    if (g_running.exchange(true)) return;
    g_thread = CreateThread(nullptr, 0, WatchThread, nullptr, 0, nullptr);
}

void Shutdown() {
    if (!g_running.exchange(false)) return;
    if (g_thread) {
        WaitForSingleObject(g_thread, 2000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
}

int Add(uintptr_t address, const std::string& type, const std::string& label) {
    if (TypeSize(type) == 0) return -1;
    std::lock_guard<std::mutex> lock(g_mutex);
    int id = g_nextId++;
    g_entries[id] = Entry{address, type, label, 0.0, false};
    return id;
}

bool Remove(int id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_entries.erase(id) > 0;
}

std::vector<WatchInfo> List() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<WatchInfo> out;
    for (auto& [id, e] : g_entries) out.push_back(WatchInfo{id, e.address, e.type, e.label});
    return out;
}

std::vector<ChangeEvent> DrainEvents() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<ChangeEvent> out(g_events.begin(), g_events.end());
    g_events.clear();
    return out;
}

} // namespace watch
