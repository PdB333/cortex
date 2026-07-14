#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace freeze {

struct FreezeInfo {
    int id;
    uintptr_t address;
    std::string type;
    std::vector<uint8_t> valueBytes;
    std::string label;
    int64_t ttlMsRemaining = 0; // 0 = no expiry
};

// Restores previously registered freezes from the persistent project file
// (see core/project), then starts the background thread that continuously
// reapplies them. Idempotent; call once during DLL init, after
// project::Init().
void Init();
void Shutdown();

// Registers `address` to be rewritten with `valueBytes` (already encoded
// for `type`) roughly every 16ms -- the "Freeze" case from Cheat Engine:
// overwrites whatever the game wrote in between, for infinite health/ammo/
// etc without a code patch. If `ttlMs` is 0 (default), the freeze is
// persisted immediately to the project file and survives a DLL reload, same
// as before. If `ttlMs` is > 0, the freeze auto-removes itself once that
// many milliseconds elapse (useful for "freeze for N seconds while I test
// this" without a manual cleanup call) and is intentionally NOT persisted --
// a persisted expiry would silently restart its countdown on every reload,
// which is more surprising than just treating timed freezes as session-only.
// Returns the new freeze id.
int Add(uintptr_t address, const std::string& type, const std::vector<uint8_t>& valueBytes, const std::string& label,
        int64_t ttlMs = 0);
bool Remove(int id);
std::vector<FreezeInfo> List();

} // namespace freeze
