#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dissect {

// Cheat Engine's "structure dissect" workflow: snapshot the raw bytes at an
// address now, do something in-game (or just wait for a tick), snapshot
// again, then diff the two to see which byte ranges actually changed --
// narrowing an unknown blob of memory down to its live fields without
// knowing the layout up front. Snapshots are in-memory only (like
// core/watch), not persisted to project.json -- a short-lived exploration
// aid, not a standing setup worth surviving a DLL reload.

struct Snapshot {
    uintptr_t address;
    std::vector<uint8_t> bytes;
};

// Reads `size` bytes at `address` and stores them under a new id. Returns -1
// if the read fails.
int TakeSnapshot(uintptr_t address, size_t size);

bool DeleteSnapshot(int id);

struct SnapshotInfo {
    int id;
    uintptr_t address;
    size_t size;
};
std::vector<SnapshotInfo> ListSnapshots();

struct DiffRange {
    int64_t offset;      // relative to the snapshot's base address
    std::vector<uint8_t> before;
    std::vector<uint8_t> after;
};

// Diffs two same-size snapshots byte-by-byte, coalescing adjacent differing
// bytes into contiguous ranges. Fails (returns false) if either id is
// unknown or the two snapshots have different sizes.
bool DiffSnapshots(int idA, int idB, std::vector<DiffRange>& outDiffs, std::string& outError);

} // namespace dissect
