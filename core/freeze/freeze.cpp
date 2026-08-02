#include "freeze.h"
#include "../memory/memory.h"
#include "../project/project.h"

#include <windows.h>
#include <atomic>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>

namespace freeze {

namespace {

constexpr DWORD kIntervalMs = 16; // ~60Hz, matches a typical frame

struct Entry {
    uintptr_t address;
    std::string type;
    std::vector<uint8_t> valueBytes;
    std::string label;
    int64_t ttlMs = 0;
    ULONGLONG expiresAt = 0; // 0 = no expiry; GetTickCount64() value
};

std::mutex g_mutex;
std::map<int, Entry> g_entries;
int g_nextId = 1;
std::atomic<bool> g_running{false};
HANDLE g_thread = nullptr;

std::string HexAddr(uintptr_t a) {
    std::ostringstream s;
    s << "0x" << std::hex << a;
    return s.str();
}

uintptr_t ParseHexAddr(const std::string& s) {
    return static_cast<uintptr_t>(std::stoull(s, nullptr, 0));
}

std::string BytesToHex(const std::vector<uint8_t>& buf) {
    std::ostringstream hex;
    for (uint8_t b : buf) hex << std::setw(2) << std::setfill('0') << std::hex << static_cast<int>(b);
    return hex.str();
}

std::vector<uint8_t> HexToBytes(const std::string& hexIn) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < hexIn.size(); i += 2) {
        out.push_back(static_cast<uint8_t>(std::stoi(hexIn.substr(i, 2), nullptr, 16)));
    }
    return out;
}

// Caller must hold g_mutex. Timed (ttlMs > 0) entries are session-only and
// deliberately excluded -- see the rationale on freeze::Add in freeze.h.
void SyncToProject() {
    project::json arr = project::json::array();
    for (auto& [id, e] : g_entries) {
        if (e.ttlMs > 0) continue;
        arr.push_back({{"address", HexAddr(e.address)}, {"type", e.type},
                        {"value_bytes", BytesToHex(e.valueBytes)}, {"label", e.label}});
    }
    project::SetFreezes(arr);
}

DWORD WINAPI FreezeThread(LPVOID) {
    while (g_running) {
        std::vector<std::pair<uintptr_t, std::vector<uint8_t>>> snapshot;
        ULONGLONG now = GetTickCount64();
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            std::vector<int> expired;
            for (auto& [id, e] : g_entries) {
                if (e.expiresAt != 0 && now >= e.expiresAt) { expired.push_back(id); continue; }
                snapshot.emplace_back(e.address, e.valueBytes);
            }
            if (!expired.empty()) {
                for (int id : expired) g_entries.erase(id);
                SyncToProject();
            }
        }
        for (auto& [addr, bytes] : snapshot) memory::WriteBytes(addr, bytes);
        Sleep(kIntervalMs);
    }
    return 0;
}

} // namespace

void Init() {
    if (g_running.exchange(true)) return;

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const auto& item : project::GetFreezes()) {
            Entry e;
            e.address = ParseHexAddr(item.value("address", std::string("0x0")));
            e.type = item.value("type", std::string(""));
            e.valueBytes = HexToBytes(item.value("value_bytes", std::string("")));
            e.label = item.value("label", std::string(""));
            g_entries[g_nextId++] = e;
        }
    }

    g_thread = CreateThread(nullptr, 0, FreezeThread, nullptr, 0, nullptr);
}

void Shutdown() {
    if (!g_running.exchange(false)) return;
    if (g_thread) {
        WaitForSingleObject(g_thread, 2000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
}

int Add(uintptr_t address, const std::string& type, const std::vector<uint8_t>& valueBytes, const std::string& label,
        int64_t ttlMs) {
    int id = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        id = g_nextId++;
        Entry e{address, type, valueBytes, label, ttlMs, 0};
        if (ttlMs > 0) e.expiresAt = GetTickCount64() + static_cast<ULONGLONG>(ttlMs);
        g_entries[id] = std::move(e);
        SyncToProject();
    }

    // Apply the requested value before returning from the API call. The worker
    // thread continues to reassert it at the normal interval afterwards.
    memory::WriteBytes(address, valueBytes);
    return id;
}

bool Remove(int id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    bool removed = g_entries.erase(id) > 0;
    if (removed) SyncToProject();
    return removed;
}

std::vector<FreezeInfo> List() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<FreezeInfo> out;
    ULONGLONG now = GetTickCount64();
    for (auto& [id, e] : g_entries) {
        int64_t remaining = 0;
        if (e.expiresAt != 0) remaining = e.expiresAt > now ? static_cast<int64_t>(e.expiresAt - now) : 0;
        out.push_back(FreezeInfo{id, e.address, e.type, e.valueBytes, e.label, remaining});
    }
    return out;
}

} // namespace freeze