#include "dissect.h"
#include "../memory/memory.h"

#include <map>
#include <mutex>

namespace dissect {

namespace {

std::mutex g_mutex;
std::map<int, Snapshot> g_snapshots;
int g_nextId = 1;

} // namespace

int TakeSnapshot(uintptr_t address, size_t size) {
    std::vector<uint8_t> bytes;
    if (!memory::ReadBytes(address, size, bytes)) return -1;

    std::lock_guard<std::mutex> lock(g_mutex);
    int id = g_nextId++;
    g_snapshots[id] = Snapshot{address, std::move(bytes)};
    return id;
}

bool DeleteSnapshot(int id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_snapshots.erase(id) > 0;
}

std::vector<SnapshotInfo> ListSnapshots() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<SnapshotInfo> out;
    for (const auto& [id, snap] : g_snapshots) {
        out.push_back({id, snap.address, snap.bytes.size()});
    }
    return out;
}

bool DiffSnapshots(int idA, int idB, std::vector<DiffRange>& outDiffs, std::string& outError) {
    outDiffs.clear();

    std::lock_guard<std::mutex> lock(g_mutex);
    auto itA = g_snapshots.find(idA);
    auto itB = g_snapshots.find(idB);
    if (itA == g_snapshots.end()) {
        outError = "unknown_snapshot_a";
        return false;
    }
    if (itB == g_snapshots.end()) {
        outError = "unknown_snapshot_b";
        return false;
    }

    const Snapshot& a = itA->second;
    const Snapshot& b = itB->second;
    if (a.bytes.size() != b.bytes.size()) {
        outError = "size_mismatch";
        return false;
    }

    size_t size = a.bytes.size();
    size_t i = 0;
    while (i < size) {
        if (a.bytes[i] == b.bytes[i]) {
            ++i;
            continue;
        }
        size_t start = i;
        while (i < size && a.bytes[i] != b.bytes[i]) ++i;

        DiffRange range;
        range.offset = static_cast<int64_t>(start);
        range.before.assign(a.bytes.begin() + start, a.bytes.begin() + i);
        range.after.assign(b.bytes.begin() + start, b.bytes.begin() + i);
        outDiffs.push_back(std::move(range));
    }

    return true;
}

} // namespace dissect
