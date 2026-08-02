#pragma once

#include "../../sdk/include/cortex/diag_protocol.h"

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace hostdiag {

struct HeartbeatSnapshot {
    std::string source;
    DWORD threadId = 0;
    uint64_t lastTickMs = 0;
    uint64_t sequence = 0;
};

struct SharedSnapshot {
    bool available = false;
    bool ready = false;
    bool sameBitness = false;
    DWORD processId = 0;
    uint32_t pointerSize = 0;
    uint64_t startedTickMs = 0;
    uint64_t lastCoreHeartbeatMs = 0;
    CortexDiagSharedCrash crash{};
    std::vector<HeartbeatSnapshot> heartbeats;
};

class SharedClient {
public:
    SharedClient() = default;
    ~SharedClient();
    SharedClient(const SharedClient&) = delete;
    SharedClient& operator=(const SharedClient&) = delete;

    bool Open(DWORD processId, std::string& error);
    void Close();
    bool Wait(DWORD timeoutMs) const;
    bool Snapshot(SharedSnapshot& output, std::string& error) const;
    bool IsOpen() const { return state_ != nullptr; }

private:
    HANDLE mapping_ = nullptr;
    HANDLE event_ = nullptr;
    CortexDiagSharedState* state_ = nullptr;
};

struct DumpResult {
    bool success = false;
    DWORD error = 0;
    std::string path;
};

struct ThreadSnapshot {
    DWORD threadId = 0;
    uint64_t instruction = 0;
    uint64_t stackPointer = 0;
    uint64_t framePointer = 0;
    DWORD suspendCount = 0;
    DWORD error = 0;
};

std::string MakeCaptureDirectory(const std::string& root, const char* prefix, DWORD processId);
bool FindNewestCrashDirectory(const std::string& root, DWORD processId, std::string& output);
DumpResult WriteProcessDump(DWORD processId, const std::string& path,
                            const CortexDiagSharedCrash* crash, bool fullMemory);
bool CaptureThreads(DWORD processId, const std::string& path,
                    std::vector<ThreadSnapshot>* snapshots, std::string& error);
bool IsWindowResponsive(DWORD processId, bool& windowFound);
bool IsProcessAlive(HANDLE process);
uint64_t HeartbeatAgeMs(const SharedSnapshot& snapshot, const std::string& source,
                        uint64_t nowTickMs, bool& found);

} // namespace hostdiag
