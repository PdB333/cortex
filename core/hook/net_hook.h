#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace nethook {

struct Event {
    uint64_t id;
    uint64_t tickMs;
    int      direction; // 0=recv 1=send
    int      socket;
    uint32_t size;
    std::string previewHex; // first N bytes as lowercase hex
};

// Hooks WSARecv/WSASend/recv/send (ws2_32.dll). Idempotent.
bool Init();

// Enable/disable capture without unhooking. Zero cost when disabled.
void SetCaptureEnabled(bool on);
bool IsCaptureEnabled();

// Copy of the current ring buffer of events (most recent first, up to max).
std::vector<Event> Snapshot(size_t max = 200);

} // namespace nethook
