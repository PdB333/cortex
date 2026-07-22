#include "net_hook.h"
#include "../log.h"

#include <winsock2.h>
#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <sstream>
#include <iomanip>

namespace nethook {

namespace {

using recv_t     = int  (WSAAPI*)(SOCKET, char*, int, int);
using send_t     = int  (WSAAPI*)(SOCKET, const char*, int, int);
using WSARecv_t  = int  (WSAAPI*)(SOCKET, LPWSABUF, DWORD, LPDWORD, LPDWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
using WSASend_t  = int  (WSAAPI*)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD,   LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);

recv_t     oRecv    = nullptr;
send_t     oSend    = nullptr;
WSARecv_t  oWSARecv = nullptr;
WSASend_t  oWSASend = nullptr;

std::atomic<bool> g_captureEnabled{false};
std::atomic<uint64_t> g_nextId{1};
std::mutex g_mutex;
std::deque<Event> g_events;
constexpr size_t kMax = 512;

std::string ToHex(const void* data, uint32_t n) {
    static constexpr char h[] = "0123456789abcdef";
    const uint32_t take = n > 64 ? 64 : n; // preview only
    std::string out;
    out.reserve(take * 2);
    auto* p = static_cast<const uint8_t*>(data);
    for (uint32_t i = 0; i < take; ++i) {
        out.push_back(h[p[i] >> 4]);
        out.push_back(h[p[i] & 0xF]);
    }
    return out;
}

void Push(int dir, SOCKET s, const void* data, uint32_t n) {
    if (!g_captureEnabled.load(std::memory_order_relaxed)) return;
    if (n == 0 || !data) return;
    Event e;
    e.id = g_nextId.fetch_add(1);
    e.tickMs = GetTickCount64();
    e.direction = dir;
    e.socket = (int)s;
    e.size = n;
    e.previewHex = ToHex(data, n);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_events.push_back(std::move(e));
    while (g_events.size() > kMax) g_events.pop_front();
}

int WSAAPI hkRecv(SOCKET s, char* buf, int len, int flags) {
    int r = oRecv(s, buf, len, flags);
    if (r > 0) Push(0, s, buf, (uint32_t)r);
    return r;
}
int WSAAPI hkSend(SOCKET s, const char* buf, int len, int flags) {
    int r = oSend(s, buf, len, flags);
    if (r > 0) Push(1, s, buf, (uint32_t)r);
    return r;
}
int WSAAPI hkWSARecv(SOCKET s, LPWSABUF b, DWORD n, LPDWORD got, LPDWORD flags,
                     LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr) {
    int r = oWSARecv(s, b, n, got, flags, ov, cr);
    // Synchronous completion only -- overlapped I/O would need us to trap the
    // completion routine too; keep it simple for a first pass.
    if (r == 0 && got && *got > 0 && b && b[0].buf) Push(0, s, b[0].buf, *got);
    return r;
}
int WSAAPI hkWSASend(SOCKET s, LPWSABUF b, DWORD n, LPDWORD sent, DWORD flags,
                     LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr) {
    if (b && n > 0 && b[0].buf) Push(1, s, b[0].buf, b[0].len);
    return oWSASend(s, b, n, sent, flags, ov, cr);
}

template <typename F>
bool Hook(HMODULE m, const char* name, F target, void** orig) {
    void* addr = (void*)GetProcAddress(m, name);
    if (!addr) { dbglog::Line("nethook: %s not found", name); return false; }
    if (MH_CreateHook(addr, (void*)target, orig) != MH_OK) return false;
    return MH_EnableHook(addr) == MH_OK;
}

std::atomic<bool> g_inited{false};

} // namespace

bool Init() {
    if (g_inited.exchange(true)) return true;
    HMODULE ws2 = LoadLibraryA("ws2_32.dll");
    if (!ws2) return false;
    bool ok = true;
    ok &= Hook(ws2, "recv",     &hkRecv,    (void**)&oRecv);
    ok &= Hook(ws2, "send",     &hkSend,    (void**)&oSend);
    ok &= Hook(ws2, "WSARecv",  &hkWSARecv, (void**)&oWSARecv);
    ok &= Hook(ws2, "WSASend",  &hkWSASend, (void**)&oWSASend);
    dbglog::Line("nethook: init ok=%d", (int)ok);
    return ok;
}

void SetCaptureEnabled(bool on) { g_captureEnabled.store(on); }
bool IsCaptureEnabled() { return g_captureEnabled.load(); }

std::vector<Event> Snapshot(size_t max) {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<Event> out;
    size_t n = g_events.size();
    size_t take = n < max ? n : max;
    out.reserve(take);
    for (auto it = g_events.rbegin(); it != g_events.rend() && out.size() < take; ++it)
        out.push_back(*it);
    return out;
}

} // namespace nethook
