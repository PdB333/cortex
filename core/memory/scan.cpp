#include "scan.h"
#include "memory.h"
#include "provenance.h"
#include "../process/modules.h"
#include "../debugger/debugger.h"

#include <windows.h>
#include <cstring>
#include <cmath>
#include <map>
#include <mutex>
#include <algorithm>
#include <sstream>
#include <unordered_set>
#include <cctype>
#include <utility>
#include <variant>
#include <limits>

namespace memscan {

namespace {

// Candidates cost ~16 bytes each (address + last value), so even a few
// million is a modest amount of RAM -- the cap exists to bound ScanNext's
// per-candidate re-read cost, not memory. An "unknown value" scan with no
// range restriction records EVERY aligned address in writable memory, which
// for a 32-bit process can be many millions of entries; anything beyond the
// cap is never even looked at (not just hidden from results) since scanning
// stops as soon as it's hit -- callers that know roughly where to look
// should pass rangeStart/rangeEnd to stay clear of this entirely.
constexpr size_t kMaxCandidates = 3000000;
constexpr size_t kMaxAobMatches = 5000;
constexpr size_t kMaxResultsPage = 1000;
// Never allocate/read an entire VirtualQuery region at once. Games and
// allocators commonly reserve+commit arenas hundreds of MiB large. Cheat
// Engine-style scanners walk them in bounded blocks instead of rejecting the
// whole arena. A smaller fallback isolates a transiently unreadable subrange
// without losing every readable byte around it.
constexpr size_t kScanBlockSize = 8u * 1024 * 1024;
constexpr size_t kScanFallbackBlockSize = 64u * 1024;
constexpr size_t kMaxChunkOverlap = 1u * 1024 * 1024;
constexpr size_t kStringChunkOverlap = 64u * 1024;
constexpr double kFloatEpsilon = 0.00001;
constexpr size_t kMaxPointerHits = 5000;
constexpr int kMaxPointerScanDepth = 7;
constexpr size_t kMaxPointerScanResults = 50;
constexpr size_t kMaxFrontierPerLevel = 200;
constexpr size_t kMaxMatchesPerNode = 32;
constexpr size_t kMaxPointerMapEntries = 4000000;
constexpr size_t kMaxStringHits = 5000;

#ifdef _WIN64
constexpr size_t kPtrSize = 8;
#else
constexpr size_t kPtrSize = 4;
#endif

struct Session {
    std::string type;
    size_t typeSize = 0;
    std::vector<uintptr_t> candidates;
    std::vector<ScanScalar> lastValues;
    std::vector<std::string> candidateTypes; // only populated when type == "all"
    bool excludeCortex = true;
    uint64_t candidateRangeId = 0;
    uint64_t valueRangeId = 0;
    uint64_t typeRangeId = 0;
};

std::mutex g_mutex;
std::map<int, Session> g_sessions;
int g_nextId = 1;

constexpr const char* kAllTypes[] = {"i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "float", "double"};

size_t TypeSize(const std::string& type) {
    if (type == "i8" || type == "u8") return 1;
    if (type == "i16" || type == "u16") return 2;
    if (type == "i32" || type == "u32" || type == "float") return 4;
    if (type == "i64" || type == "u64" || type == "double") return 8;
    return 0;
}

bool IsFloatType(const std::string& type) { return type == "float" || type == "double"; }

ScanScalar BytesToScalar(const uint8_t* raw, const std::string& type) {
    if (type == "i8") { int8_t v; memcpy(&v, raw, 1); return static_cast<int64_t>(v); }
    if (type == "u8") { uint8_t v; memcpy(&v, raw, 1); return static_cast<uint64_t>(v); }
    if (type == "i16") { int16_t v; memcpy(&v, raw, 2); return static_cast<int64_t>(v); }
    if (type == "u16") { uint16_t v; memcpy(&v, raw, 2); return static_cast<uint64_t>(v); }
    if (type == "i32") { int32_t v; memcpy(&v, raw, 4); return static_cast<int64_t>(v); }
    if (type == "u32") { uint32_t v; memcpy(&v, raw, 4); return static_cast<uint64_t>(v); }
    if (type == "i64") { int64_t v; memcpy(&v, raw, 8); return v; }
    if (type == "u64") { uint64_t v; memcpy(&v, raw, 8); return v; }
    if (type == "float") { float v; memcpy(&v, raw, 4); return static_cast<double>(v); }
    if (type == "double") { double v; memcpy(&v, raw, 8); return v; }
    return int64_t{0};
}

ScanScalar ParseScalar(const std::string& type, const std::string& text) {
    if (IsFloatType(type)) return std::stod(text);
    if (!type.empty() && type[0] == 'u') return static_cast<uint64_t>(std::stoull(text, nullptr, 0));
    return static_cast<int64_t>(std::stoll(text, nullptr, 0));
}

bool ValuesEqual(const std::string& type, const ScanScalar& a, const ScanScalar& b) {
    if (IsFloatType(type)) return std::fabs(std::get<double>(a) - std::get<double>(b)) < kFloatEpsilon;
    return a == b;
}

int CompareScalars(const ScanScalar& a, const ScanScalar& b) {
    if (a.index() != b.index()) return 0;
    if (std::holds_alternative<int64_t>(a)) {
        auto av = std::get<int64_t>(a), bv = std::get<int64_t>(b);
        return av < bv ? -1 : av > bv ? 1 : 0;
    }
    if (std::holds_alternative<uint64_t>(a)) {
        auto av = std::get<uint64_t>(a), bv = std::get<uint64_t>(b);
        return av < bv ? -1 : av > bv ? 1 : 0;
    }
    auto av = std::get<double>(a), bv = std::get<double>(b);
    return av < bv ? -1 : av > bv ? 1 : 0;
}

bool DeltaEquals(const std::string& type, const ScanScalar& high, const ScanScalar& low, const ScanScalar& delta) {
    if (IsFloatType(type)) return ValuesEqual(type, ScanScalar{std::get<double>(high) - std::get<double>(low)}, delta);
    if (std::holds_alternative<int64_t>(high)) {
        int64_t h = std::get<int64_t>(high), l = std::get<int64_t>(low), d = std::get<int64_t>(delta);
        if (d < 0 || h < l) return false;
        return static_cast<uint64_t>(h) - static_cast<uint64_t>(l) == static_cast<uint64_t>(d);
    }
    uint64_t h = std::get<uint64_t>(high), l = std::get<uint64_t>(low), d = std::get<uint64_t>(delta);
    return h >= l && h - l == d;
}

bool ReadValueAt(uintptr_t address, const std::string& type, ScanScalar& out) {
    std::vector<uint8_t> buf;
    size_t size = TypeSize(type);
    if (size == 0 || !memory::ReadBytes(address, size, buf)) return false;
    out = BytesToScalar(buf.data(), type);
    return true;
}

// Committed, non-guarded, readable regions, optionally narrowed further by
// AND-ing in writable/executable/copy-on-write requirements (mirrors Cheat
// Engine's scan-options checkboxes, which narrow when checked together
// rather than union). This is the single region walk every scan feature
// funnels through; EnumerateWritableRegions/EnumerateReadableRegions below
// are thin wrappers kept for existing call sites.
std::vector<MEMORY_BASIC_INFORMATION> EnumerateRegions(bool writableOnly, bool executableOnly, bool copyOnWriteOnly) {
    std::vector<MEMORY_BASIC_INFORMATION> regions;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t addr = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    uintptr_t maxAddr = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);

    MEMORY_BASIC_INFORMATION mbi;
    while (addr < maxAddr) {
        if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) != sizeof(mbi)) break;
        bool eligible = (mbi.State == MEM_COMMIT) && !(mbi.Protect & PAGE_GUARD) && mbi.Protect != PAGE_NOACCESS;
        if (eligible && writableOnly) {
            eligible = (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
        }
        if (eligible && executableOnly) {
            eligible = (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
        }
        if (eligible && copyOnWriteOnly) {
            eligible = (mbi.Protect & (PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)) != 0;
        }
        if (eligible) regions.push_back(mbi);
        uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= addr) break; // guard against a zero-size region looping forever
        addr = next;
    }
    return regions;
}

std::vector<MEMORY_BASIC_INFORMATION> EnumerateWritableRegions() { return EnumerateRegions(true, false, false); }

// Broader than EnumerateWritableRegions: any committed, non-guarded page the
// process itself can read, including read-only module sections (.rdata,
// .text) where literal strings and constants actually live -- a game's
// display strings are almost never in writable memory.
std::vector<MEMORY_BASIC_INFORMATION> EnumerateReadableRegions() { return EnumerateRegions(false, false, false); }

// Suspends every thread in the process except the caller's own (so the HTTP
// worker thread performing the scan never deadlocks itself). Returns the
// suspended threads' handles for a matching ResumeThreads call.
std::vector<HANDLE> SuspendOtherThreads() {
    std::vector<HANDLE> handles;
    DWORD myTid = GetCurrentThreadId();
    for (DWORD tid : dbg::ListThreadIds()) {
        if (tid == myTid) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME, FALSE, tid);
        if (!h) continue;
        if (SuspendThread(h) == static_cast<DWORD>(-1)) { CloseHandle(h); continue; }
        handles.push_back(h);
    }
    return handles;
}

void ResumeThreads(std::vector<HANDLE>& handles) {
    for (HANDLE h : handles) { ResumeThread(h); CloseHandle(h); }
    handles.clear();
}

bool IsReadableCommitted(const MEMORY_BASIC_INFORMATION& mbi) {
    return mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) &&
           (mbi.Protect & 0xFF) != PAGE_NOACCESS && (mbi.Protect & 0xFF) != 0;
}

// Visits [start,end) in bounded readable blocks. `primarySize` is the unique
// part owned by this callback; `buf` may contain up to `overlap` extra bytes
// from the following block so fixed-size values/patterns crossing a block
// boundary are still visible without emitting duplicate starting addresses.
template <typename Visitor>
bool ForEachReadableChunk(uintptr_t start, uintptr_t end, size_t overlap, Visitor&& visitor,
                          bool excludeCortex = true) {
    if (start >= end) return true;
    overlap = (std::min)(overlap, kMaxChunkOverlap);
    std::vector<uint8_t> buf;
    provenance::ScopedRange scratch;

    auto visitSegment = [&](uintptr_t segmentStart, uintptr_t segmentEnd) -> bool {
        for (uintptr_t blockBase = segmentStart; blockBase < segmentEnd;) {
            size_t primarySize = static_cast<size_t>((std::min)(
                static_cast<uintptr_t>(kScanBlockSize), segmentEnd - blockBase));
            size_t trailing = static_cast<size_t>((std::min)(
                static_cast<uintptr_t>(overlap), segmentEnd - blockBase - primarySize));
            if (memory::ReadBytes(blockBase, primarySize + trailing, buf)) {
                scratch.Reset(reinterpret_cast<uintptr_t>(buf.data()), buf.capacity());
                if (!visitor(blockBase, buf, primarySize)) return false;
            } else {
                // A single changing/guarded page must not discard an 8 MiB
                // block. Retry smaller windows and skip only those that are
                // genuinely unreadable at the time of the scan.
                uintptr_t fallbackEnd = blockBase + primarySize;
                for (uintptr_t smallBase = blockBase; smallBase < fallbackEnd;) {
                    size_t smallPrimary = static_cast<size_t>((std::min)(
                        static_cast<uintptr_t>(kScanFallbackBlockSize), fallbackEnd - smallBase));
                    size_t smallTrailing = static_cast<size_t>((std::min)(
                        static_cast<uintptr_t>(overlap), segmentEnd - smallBase - smallPrimary));
                    if (memory::ReadBytes(smallBase, smallPrimary + smallTrailing, buf) &&
                        (scratch.Reset(reinterpret_cast<uintptr_t>(buf.data()), buf.capacity()), true) &&
                        !visitor(smallBase, buf, smallPrimary)) return false;
                    smallBase += smallPrimary;
                }
            }
            blockBase += primarySize;
        }
        return true;
    };

    uintptr_t cursor = start;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(cursor), &mbi, sizeof(mbi)) != sizeof(mbi)) break;
        uintptr_t mbiBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        uintptr_t mbiEnd = mbi.RegionSize > (std::numeric_limits<uintptr_t>::max)() - mbiBase
            ? (std::numeric_limits<uintptr_t>::max)() : mbiBase + mbi.RegionSize;
        uintptr_t segmentEnd = (std::min)(end, mbiEnd);
        if (segmentEnd <= cursor) break;
        if (IsReadableCommitted(mbi) && !visitSegment(cursor, segmentEnd)) return false;
        cursor = segmentEnd;
    }
    return true;
}

} // namespace

std::optional<Filter> ParseFilter(const std::string& s) {
    if (s == "exact") return Filter::Exact;
    if (s == "changed") return Filter::Changed;
    if (s == "unchanged") return Filter::Unchanged;
    if (s == "increased") return Filter::Increased;
    if (s == "decreased") return Filter::Decreased;
    if (s == "increased_by") return Filter::IncreasedBy;
    if (s == "decreased_by") return Filter::DecreasedBy;
    if (s == "greater_than") return Filter::GreaterThan;
    if (s == "less_than") return Filter::LessThan;
    if (s == "between") return Filter::Between;
    return std::nullopt;
}

int ScanNew(const std::string& type, std::optional<std::string> value,
            std::optional<uintptr_t> rangeStart, std::optional<uintptr_t> rangeEnd,
            size_t& outCount, bool& outTruncated, const ScanOptions& options) {
    outCount = 0;
    outTruncated = false;
    bool isAll = (type == "all");
    size_t typeSize = isAll ? 1 : TypeSize(type);
    if (!isAll && typeSize == 0) return -1;
    if (isAll && !value.has_value()) return -1; // see header: unknown-value "all" would explode the candidate set

    Session session;
    session.type = type;
    session.typeSize = isAll ? 0 : typeSize;
    session.excludeCortex = options.excludeCortex;

    std::map<std::string, ScanScalar> parsedQueries;
    if (value.has_value()) {
        if (isAll) {
            for (const char* t : kAllTypes) parsedQueries.emplace(t, ParseScalar(t, *value));
        } else {
            parsedQueries.emplace(type, ParseScalar(type, *value));
        }
    }

    std::vector<HANDLE> suspended;
    if (options.pauseProcess) suspended = SuspendOtherThreads();

    for (const auto& mbi : EnumerateRegions(options.writableOnly, options.executableOnly, options.copyOnWriteOnly)) {
        uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        uintptr_t regionEnd = regionBase + mbi.RegionSize;
        uintptr_t scanBase = rangeStart.has_value() ? (std::max)(regionBase, *rangeStart) : regionBase;
        uintptr_t scanEnd = rangeEnd.has_value() ? (std::min)(regionEnd, *rangeEnd) : regionEnd;
        if (scanBase >= scanEnd) continue;
        if (scanEnd - scanBase < typeSize) continue;

        // Step through offsets in absolute address terms (not relative to a
        // region or block), so arbitrary ranges and block boundaries never
        // change which aligned addresses are considered.
        uint32_t stride = options.alignment > 0 ? options.alignment : static_cast<uint32_t>(isAll ? 1 : typeSize);
        ForEachReadableChunk(scanBase, scanEnd, 7, [&](uintptr_t blockBase,
                                                       const std::vector<uint8_t>& buf,
                                                       size_t primarySize) {
            size_t misalign = blockBase % stride;
            size_t firstOff = misalign == 0 ? 0 : (stride - misalign);
            if (isAll) {
                for (size_t off = firstOff; off < primarySize; off += stride) {
                    for (const char* t : kAllTypes) {
                        size_t sz = TypeSize(t);
                        if (off + sz > buf.size()) continue;
                        if (options.excludeCortex && provenance::Contains(blockBase + off, sz)) continue;
                        ScanScalar v = BytesToScalar(buf.data() + off, t);
                        if (!ValuesEqual(t, v, parsedQueries.at(t))) continue;
                        session.candidates.push_back(blockBase + off);
                        session.lastValues.push_back(v);
                        session.candidateTypes.push_back(t);
                        if (session.candidates.size() >= kMaxCandidates) { outTruncated = true; break; }
                    }
                    if (outTruncated) break;
                }
            } else {
                for (size_t off = firstOff; off < primarySize && off + typeSize <= buf.size(); off += stride) {
                    if (options.excludeCortex && provenance::Contains(blockBase + off, typeSize)) continue;
                    ScanScalar v = BytesToScalar(buf.data() + off, type);
                    if (value.has_value() && !ValuesEqual(type, v, parsedQueries.at(type))) continue;
                    session.candidates.push_back(blockBase + off);
                    session.lastValues.push_back(v);
                    if (session.candidates.size() >= kMaxCandidates) { outTruncated = true; break; }
                }
            }
            auto track=[&](uint64_t& id,uintptr_t base,size_t size,const char* label){
                if(!base||!size)return;if(!id)id=provenance::Register(base,size,"scanner",label);else provenance::Resize(id,base,size);
            };
            track(session.candidateRangeId,reinterpret_cast<uintptr_t>(session.candidates.data()),session.candidates.capacity()*sizeof(uintptr_t),"scan_candidates");
            track(session.valueRangeId,reinterpret_cast<uintptr_t>(session.lastValues.data()),session.lastValues.capacity()*sizeof(ScanScalar),"scan_values");
            track(session.typeRangeId,reinterpret_cast<uintptr_t>(session.candidateTypes.data()),session.candidateTypes.capacity()*sizeof(std::string),"scan_types");
            return !outTruncated;
        }, options.excludeCortex);
        if (outTruncated) break;
    }

    if (options.pauseProcess) ResumeThreads(suspended);

    outCount = session.candidates.size();

    std::lock_guard<std::mutex> lock(g_mutex);
    int id = g_nextId++;
    g_sessions[id] = std::move(session);
    return id;
}

bool ScanNext(int sessionId, Filter filter, std::optional<std::string> value, std::optional<std::string> value2,
              size_t& outCount, bool pauseProcess) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_sessions.find(sessionId);
    if (it == g_sessions.end()) return false;
    Session& session = it->second;
    bool isAll = session.type == "all";

    std::vector<HANDLE> suspended;
    if (pauseProcess) suspended = SuspendOtherThreads();

    std::vector<uintptr_t> newCandidates;
    std::vector<ScanScalar> newValues;
    std::vector<std::string> newTypes;
    newCandidates.reserve(session.candidates.size());
    newValues.reserve(session.candidates.size());
    if (isAll) newTypes.reserve(session.candidates.size());

    for (size_t i = 0; i < session.candidates.size(); ++i) {
        const std::string& t = isAll ? session.candidateTypes[i] : session.type;
        if (session.excludeCortex && provenance::Contains(session.candidates[i], TypeSize(t))) continue;
        ScanScalar current;
        if (!ReadValueAt(session.candidates[i], t, current)) continue;
        const ScanScalar& last = session.lastValues[i];
        std::optional<ScanScalar> query = value.has_value() ? std::optional<ScanScalar>(ParseScalar(t, *value)) : std::nullopt;
        std::optional<ScanScalar> query2 = value2.has_value() ? std::optional<ScanScalar>(ParseScalar(t, *value2)) : std::nullopt;

        bool keep = false;
        switch (filter) {
            case Filter::Exact:       keep = query.has_value() && ValuesEqual(t, current, *query); break;
            case Filter::Changed:     keep = !ValuesEqual(t, current, last); break;
            case Filter::Unchanged:   keep = ValuesEqual(t, current, last); break;
            case Filter::Increased:   keep = CompareScalars(current, last) > 0; break;
            case Filter::Decreased:   keep = CompareScalars(current, last) < 0; break;
            case Filter::IncreasedBy: keep = query.has_value() && DeltaEquals(t, current, last, *query); break;
            case Filter::DecreasedBy: keep = query.has_value() && DeltaEquals(t, last, current, *query); break;
            case Filter::GreaterThan: keep = query.has_value() && CompareScalars(current, *query) > 0; break;
            case Filter::LessThan:    keep = query.has_value() && CompareScalars(current, *query) < 0; break;
            case Filter::Between:     keep = query.has_value() && query2.has_value() && CompareScalars(current, *query) >= 0 && CompareScalars(current, *query2) <= 0; break;
        }
        if (keep) {
            newCandidates.push_back(session.candidates[i]);
            newValues.push_back(current);
            if (isAll) newTypes.push_back(t);
        }
    }

    session.candidates = std::move(newCandidates);
    session.lastValues = std::move(newValues);
    if (isAll) session.candidateTypes = std::move(newTypes);
    provenance::Resize(session.candidateRangeId,reinterpret_cast<uintptr_t>(session.candidates.data()),session.candidates.capacity()*sizeof(uintptr_t));
    provenance::Resize(session.valueRangeId,reinterpret_cast<uintptr_t>(session.lastValues.data()),session.lastValues.capacity()*sizeof(ScanScalar));
    if(isAll)provenance::Resize(session.typeRangeId,reinterpret_cast<uintptr_t>(session.candidateTypes.data()),session.candidateTypes.capacity()*sizeof(std::string));
    outCount = session.candidates.size();

    if (pauseProcess) ResumeThreads(suspended);
    return true;
}

bool ScanResults(int sessionId, size_t offset, size_t limit, std::vector<ScanResult>& out, size_t& outTotal) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_sessions.find(sessionId);
    if (it == g_sessions.end()) return false;
    Session& session = it->second;
    bool isAll = session.type == "all";

    outTotal = session.candidates.size();
    limit = std::min(limit, kMaxResultsPage);
    for (size_t i = offset; i < session.candidates.size() && out.size() < limit; ++i) {
        const std::string& t = isAll ? session.candidateTypes[i] : session.type;
        ScanScalar v;
        if (!ReadValueAt(session.candidates[i], t, v)) continue;
        out.push_back({session.candidates[i], v, isAll ? t : std::string()});
    }
    return true;
}

bool ScanReset(int sessionId) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it=g_sessions.find(sessionId);if(it==g_sessions.end())return false;
    provenance::Unregister(it->second.candidateRangeId);provenance::Unregister(it->second.valueRangeId);provenance::Unregister(it->second.typeRangeId);
    g_sessions.erase(it);return true;
}

std::vector<SessionInfo> ScanList() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<SessionInfo> out;
    for (const auto& [id, session] : g_sessions) {
        out.push_back({id, session.type, session.candidates.size()});
    }
    return out;
}

bool ScanIntersect(const std::vector<int>& sessionIds, std::vector<uintptr_t>& outAddresses) {
    outAddresses.clear();
    if (sessionIds.size() < 2) return false;
    std::lock_guard<std::mutex> lock(g_mutex);

    std::vector<std::unordered_set<uintptr_t>> sets;
    sets.reserve(sessionIds.size());
    for (int id : sessionIds) {
        auto it = g_sessions.find(id);
        if (it == g_sessions.end()) return false;
        sets.emplace_back(it->second.candidates.begin(), it->second.candidates.end());
    }

    size_t smallest = 0;
    for (size_t i = 1; i < sets.size(); ++i) {
        if (sets[i].size() < sets[smallest].size()) smallest = i;
    }

    for (uintptr_t addr : sets[smallest]) {
        bool inAll = true;
        for (size_t i = 0; i < sets.size(); ++i) {
            if (i == smallest) continue;
            if (!sets[i].count(addr)) { inAll = false; break; }
        }
        if (inAll) outAddresses.push_back(addr);
    }
    return true;
}

std::vector<uintptr_t> AobScan(const std::string& pattern, const std::string& moduleName, bool& outTruncated) {
    outTruncated = false;
    std::vector<uintptr_t> matches;

    std::vector<std::optional<uint8_t>> needle;
    {
        std::string token;
        std::istringstream iss(pattern);
        while (iss >> token) {
            if (token == "??" || token == "?") needle.push_back(std::nullopt);
            else needle.push_back(static_cast<uint8_t>(std::stoi(token, nullptr, 16)));
        }
    }
    if (needle.empty()) return matches;

    std::vector<std::pair<uintptr_t, size_t>> ranges;
    if (!moduleName.empty()) {
        for (const auto& m : process::ListModules()) {
            if (m.name == moduleName) { ranges.push_back({m.base, m.size}); break; }
        }
        if (ranges.empty()) return matches; // module not found
    } else {
        // A global AOB scan must cover private and mapped allocations as well
        // as image modules. Runtime objects (game entities, inventories, JIT
        // code, etc.) normally live in heaps and would otherwise be invisible.
        for (const auto& mbi : EnumerateReadableRegions()) {
            ranges.push_back({reinterpret_cast<uintptr_t>(mbi.BaseAddress), mbi.RegionSize});
        }
    }

    for (auto& range : ranges) {
        uintptr_t base = range.first;
        size_t size = range.second;
        if (size < needle.size() || needle.size() - 1 > kMaxChunkOverlap) continue;
        uintptr_t end = size > (std::numeric_limits<uintptr_t>::max)() - base
            ? (std::numeric_limits<uintptr_t>::max)() : base + size;
        ForEachReadableChunk(base, end, needle.size() - 1,
            [&](uintptr_t blockBase, const std::vector<uint8_t>& buf, size_t primarySize) {
                for (size_t off = 0; off < primarySize && off + needle.size() <= buf.size(); ++off) {
                    if (provenance::Contains(blockBase + off, needle.size())) continue;
                    bool match = true;
                    for (size_t i = 0; i < needle.size(); ++i) {
                        if (needle[i].has_value() && buf[off + i] != *needle[i]) { match = false; break; }
                    }
                    if (match) {
                        matches.push_back(blockBase + off);
                        if (matches.size() >= kMaxAobMatches) { outTruncated = true; break; }
                    }
                }
                return !outTruncated;
            }
        );
        if (outTruncated) break;
    }
    return matches;
}

std::vector<PointerHit> FindPointersTo(uintptr_t target, uint32_t maxOffset, bool& outTruncated) {
    outTruncated = false;
    std::vector<PointerHit> hits;
    uintptr_t lowBound = (target > static_cast<uintptr_t>(maxOffset)) ? (target - maxOffset) : 0;

    for (const auto& mbi : EnumerateWritableRegions()) {
        uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        size_t regionSize = mbi.RegionSize;
        if (regionSize < kPtrSize) continue;
        uintptr_t regionEnd = regionSize > (std::numeric_limits<uintptr_t>::max)() - regionBase
            ? (std::numeric_limits<uintptr_t>::max)() : regionBase + regionSize;
        ForEachReadableChunk(regionBase, regionEnd, kPtrSize - 1,
            [&](uintptr_t blockBase, const std::vector<uint8_t>& buf, size_t primarySize) {
                size_t misalign = blockBase % kPtrSize;
                size_t firstOff = misalign == 0 ? 0 : (kPtrSize - misalign);
                for (size_t off = firstOff; off < primarySize && off + kPtrSize <= buf.size(); off += kPtrSize) {
                    if (provenance::Contains(blockBase + off, kPtrSize)) continue;
                    uintptr_t v;
#ifdef _WIN64
                    uint64_t raw; memcpy(&raw, buf.data() + off, 8); v = static_cast<uintptr_t>(raw);
#else
                    uint32_t raw; memcpy(&raw, buf.data() + off, 4); v = static_cast<uintptr_t>(raw);
#endif
                    if (v < lowBound || v > target) continue;
                    hits.push_back(PointerHit{blockBase + off, static_cast<int64_t>(target - v)});
                    if (hits.size() >= kMaxPointerHits) { outTruncated = true; break; }
                }
                return !outTruncated;
            });
        if (outTruncated) break;
    }
    return hits;
}

std::vector<PointerPathResult> PointerScan(uintptr_t target, int maxDepth, uint32_t maxOffset, bool& outTruncated) {
    outTruncated = false;
    std::vector<PointerPathResult> results;
    maxDepth = std::clamp(maxDepth, 1, kMaxPointerScanDepth);

    // Build a one-shot map of every pointer-looking slot in writable memory
    // (address -> value), sorted by value so each BFS level can binary-search
    // for "what points near this address" instead of rescanning memory.
    struct PtrMapEntry { uintptr_t address; uintptr_t value; };
    std::vector<PtrMapEntry> ptrMap;
    ptrMap.reserve(kMaxPointerMapEntries);
    provenance::ScopedRange pointerMapMemory(reinterpret_cast<uintptr_t>(ptrMap.data()),
                                              ptrMap.capacity() * sizeof(PtrMapEntry),
                                              "scanner", "pointer_map");
    {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        uintptr_t minAddr = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
        uintptr_t maxAddrSpace = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);

        for (const auto& mbi : EnumerateWritableRegions()) {
            uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            size_t regionSize = mbi.RegionSize;
            if (regionSize < kPtrSize) continue;
            uintptr_t regionEnd = regionSize > (std::numeric_limits<uintptr_t>::max)() - regionBase
                ? (std::numeric_limits<uintptr_t>::max)() : regionBase + regionSize;
            ForEachReadableChunk(regionBase, regionEnd, kPtrSize - 1,
                [&](uintptr_t blockBase, const std::vector<uint8_t>& buf, size_t primarySize) {
                    size_t misalign = blockBase % kPtrSize;
                    size_t firstOff = misalign == 0 ? 0 : (kPtrSize - misalign);
                    for (size_t off = firstOff; off < primarySize && off + kPtrSize <= buf.size(); off += kPtrSize) {
                        if (provenance::Contains(blockBase + off, kPtrSize)) continue;
                        uintptr_t v;
#ifdef _WIN64
                        uint64_t raw; memcpy(&raw, buf.data() + off, 8); v = static_cast<uintptr_t>(raw);
#else
                        uint32_t raw; memcpy(&raw, buf.data() + off, 4); v = static_cast<uintptr_t>(raw);
#endif
                        if (v < minAddr || v > maxAddrSpace) continue;
                        ptrMap.push_back({blockBase + off, v});
                        if (ptrMap.size() >= kMaxPointerMapEntries) break;
                    }
                    return ptrMap.size() < kMaxPointerMapEntries;
                });
            if (ptrMap.size() >= kMaxPointerMapEntries) { outTruncated = true; break; }
        }
    }
    if (ptrMap.empty()) return results;
    std::sort(ptrMap.begin(), ptrMap.end(), [](const PtrMapEntry& a, const PtrMapEntry& b) { return a.value < b.value; });

    auto modules = process::ListModules();
    auto findStaticModule = [&](uintptr_t addr) -> std::optional<std::pair<std::string, int64_t>> {
        for (const auto& m : modules) {
            if (addr >= m.base && addr < m.base + m.size) return std::make_pair(m.name, static_cast<int64_t>(addr - m.base));
        }
        return std::nullopt;
    };

    // A frontier node's `addr` is the address we're looking for predecessors
    // of; `offsetsSoFar` accumulates deref offsets in discovery order
    // (target-relative, innermost first) -- reversed into base-to-target
    // application order once a path terminates at a static root.
    struct FrontierNode { uintptr_t addr; std::vector<int64_t> offsetsSoFar; };
    std::vector<FrontierNode> frontier{{target, {}}};
    std::unordered_set<uintptr_t> visited{target};

    for (int depth = 0; depth < maxDepth && !frontier.empty() && results.size() < kMaxPointerScanResults; ++depth) {
        std::vector<FrontierNode> nextFrontier;
        for (const auto& node : frontier) {
            if (results.size() >= kMaxPointerScanResults) break;
            uintptr_t lowBound = (node.addr > static_cast<uintptr_t>(maxOffset)) ? (node.addr - maxOffset) : 0;

            auto lo = std::lower_bound(ptrMap.begin(), ptrMap.end(), lowBound,
                [](const PtrMapEntry& e, uintptr_t v) { return e.value < v; });

            size_t matchCount = 0;
            for (auto it = lo; it != ptrMap.end() && it->value <= node.addr; ++it) {
                if (matchCount >= kMaxMatchesPerNode) { outTruncated = true; break; }
                matchCount++;
                uintptr_t P = it->address;
                if (visited.count(P)) continue;

                int64_t offsetHere = static_cast<int64_t>(node.addr) - static_cast<int64_t>(it->value);
                std::vector<int64_t> newOffsets = node.offsetsSoFar;
                newOffsets.push_back(offsetHere);

                auto staticHit = findStaticModule(P);
                if (staticHit.has_value()) {
                    std::vector<int64_t> ordered(newOffsets.rbegin(), newOffsets.rend());
                    results.push_back({staticHit->first, staticHit->second, ordered});
                    if (results.size() >= kMaxPointerScanResults) break;
                } else if (depth + 1 < maxDepth) {
                    visited.insert(P);
                    if (nextFrontier.size() < kMaxFrontierPerLevel) {
                        nextFrontier.push_back({P, newOffsets});
                    } else {
                        outTruncated = true;
                    }
                }
            }
        }
        frontier = std::move(nextFrontier);
    }

    return results;
}

namespace {

bool IsPrintableAscii(uint8_t b) { return b >= 0x20 && b < 0x7F; }

std::string ToLowerCopy(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool CiContains(const std::string& haystack, const std::string& needleLower) {
    if (needleLower.empty()) return true;
    return ToLowerCopy(haystack).find(needleLower) != std::string::npos;
}

void ScanBufferForAscii(const uint8_t* data, size_t size, size_t primarySize, uintptr_t base, size_t minLength,
                         const std::string& needleLower, std::vector<StringHit>& hits, bool& truncated) {
    size_t i = 0;
    while (i < primarySize) {
        if (provenance::Contains(base + i)) { i++; continue; }
        if (!IsPrintableAscii(data[i])) { i++; continue; }
        size_t start = i;
        while (i < size && IsPrintableAscii(data[i])) i++;
        size_t len = i - start;
        if (len >= minLength) {
            std::string s(reinterpret_cast<const char*>(data + start), len);
            if (CiContains(s, needleLower)) {
                hits.push_back({base + start, s});
                if (hits.size() >= kMaxStringHits) { truncated = true; return; }
            }
        }
    }
}

void ScanBufferForUtf16(const uint8_t* data, size_t size, size_t primarySize, uintptr_t base, size_t minLength,
                         const std::string& needleLower, std::vector<StringHit>& hits, bool& truncated) {
    if (size < 2) return;
    size_t i = 0;
    while (i < primarySize && i + 1 < size) {
        if (!(IsPrintableAscii(data[i]) && data[i + 1] == 0)) { i += 2; continue; }
        if (provenance::Contains(base + i, 2)) { i += 2; continue; }
        size_t start = i;
        std::string s;
        while (i + 1 < size && IsPrintableAscii(data[i]) && data[i + 1] == 0) {
            s.push_back(static_cast<char>(data[i]));
            i += 2;
        }
        if (s.size() >= minLength) {
            if (CiContains(s, needleLower)) {
                hits.push_back({base + start, s});
                if (hits.size() >= kMaxStringHits) { truncated = true; return; }
            }
        }
    }
}

} // namespace

std::vector<StringHit> StringScan(size_t minLength, const std::string& contains, const std::string& moduleName,
                                   StringEncoding encoding, bool& outTruncated) {
    outTruncated = false;
    std::vector<StringHit> hits;
    if (minLength == 0) minLength = 4;
    std::string needleLower = ToLowerCopy(contains);

    std::vector<std::pair<uintptr_t, size_t>> ranges;
    if (!moduleName.empty()) {
        for (const auto& m : process::ListModules()) {
            if (m.name == moduleName) { ranges.push_back({m.base, m.size}); break; }
        }
        if (ranges.empty()) return hits; // module not found
    } else {
        for (const auto& mbi : EnumerateReadableRegions()) {
            ranges.push_back({reinterpret_cast<uintptr_t>(mbi.BaseAddress), mbi.RegionSize});
        }
    }

    for (auto& range : ranges) {
        uintptr_t base = range.first;
        size_t size = range.second;
        if (size == 0) continue;
        uintptr_t end = size > (std::numeric_limits<uintptr_t>::max)() - base
            ? (std::numeric_limits<uintptr_t>::max)() : base + size;
        ForEachReadableChunk(base, end, kStringChunkOverlap,
            [&](uintptr_t blockBase, const std::vector<uint8_t>& buf, size_t primarySize) {
                if (encoding == StringEncoding::Ascii)
                    ScanBufferForAscii(buf.data(), buf.size(), primarySize, blockBase, minLength,
                                       needleLower, hits, outTruncated);
                else
                    ScanBufferForUtf16(buf.data(), buf.size(), primarySize, blockBase, minLength,
                                       needleLower, hits, outTruncated);
                return !outTruncated;
            });

        if (outTruncated) break;
    }
    return hits;
}

std::vector<CodeCave> FindCodeCaves(size_t minSize, const std::string& moduleName, bool& outTruncated) {
    outTruncated = false;
    std::vector<CodeCave> caves;
    if (minSize == 0) minSize = 16;

    std::vector<std::pair<uintptr_t, size_t>> ranges;
    if (!moduleName.empty()) {
        for (const auto& m : process::ListModules()) {
            if (m.name == moduleName) { ranges.push_back({m.base, m.size}); break; }
        }
        if (ranges.empty()) return caves; // module not found
    } else {
        for (const auto& mbi : EnumerateRegions(false, true, false)) {
            ranges.push_back({reinterpret_cast<uintptr_t>(mbi.BaseAddress), mbi.RegionSize});
        }
    }

    for (auto& range : ranges) {
        uintptr_t base = range.first;
        size_t size = range.second;
        if (size < minSize) continue;
        uintptr_t end = size > (std::numeric_limits<uintptr_t>::max)() - base
            ? (std::numeric_limits<uintptr_t>::max)() : base + size;
        size_t overlap = minSize > 0 ? minSize - 1 : 0;
        ForEachReadableChunk(base, end, overlap,
            [&](uintptr_t blockBase, const std::vector<uint8_t>& buf, size_t primarySize) {
                size_t i = 0;
                while (i < primarySize) {
                    if (provenance::Contains(blockBase + i)) { i++; continue; }
                    uint8_t b = buf[i];
                    if (b != 0x00 && b != 0xCC) { i++; continue; }
                    size_t start = i;
                    while (i < buf.size() && buf[i] == b) i++;
                    size_t len = i - start;
                    if (len >= minSize) {
                        caves.push_back({blockBase + start, len});
                        if (caves.size() >= kMaxAobMatches) { outTruncated = true; break; }
                    }
                }
                return !outTruncated;
            });
        if (outTruncated) break;
    }
    return caves;
}

std::vector<MemoryRegionInfo> ListRegions() {
    std::vector<MemoryRegionInfo> out;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t addr = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    uintptr_t maxAddr = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);

    auto modules = process::ListModules();
    auto findModule = [&](uintptr_t a) -> std::string {
        for (const auto& m : modules) {
            if (a >= m.base && a < m.base + m.size) return m.name;
        }
        return std::string();
    };

    MEMORY_BASIC_INFORMATION mbi;
    while (addr < maxAddr) {
        if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) != sizeof(mbi)) break;

        MemoryRegionInfo info;
        info.base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        info.size = mbi.RegionSize;
        info.state = mbi.State == MEM_COMMIT ? "commit" : (mbi.State == MEM_RESERVE ? "reserve" : "free");
        info.type = mbi.Type == MEM_IMAGE ? "image" : (mbi.Type == MEM_MAPPED ? "mapped" : (mbi.Type == MEM_PRIVATE ? "private" : ""));

        if (mbi.State == MEM_COMMIT) {
            DWORD prot = mbi.Protect & 0xFF; // strip PAGE_GUARD/PAGE_NOCACHE/PAGE_WRITECOMBINE modifier bits
            bool r = false, w = false, x = false;
            switch (prot) {
                case PAGE_READONLY: r = true; break;
                case PAGE_READWRITE: r = true; w = true; break;
                case PAGE_WRITECOPY: r = true; w = true; break;
                case PAGE_EXECUTE: x = true; break;
                case PAGE_EXECUTE_READ: r = true; x = true; break;
                case PAGE_EXECUTE_READWRITE: r = true; w = true; x = true; break;
                case PAGE_EXECUTE_WRITECOPY: r = true; w = true; x = true; break;
                default: break;
            }
            info.protect = std::string(r ? "r" : "-") + (w ? "w" : "-") + (x ? "x" : "-");
        } else {
            info.protect = "---";
        }
        info.moduleName = info.state == "commit" ? findModule(info.base) : std::string();
        out.push_back(info);

        uintptr_t next = info.base + mbi.RegionSize;
        if (next <= addr) break;
        addr = next;
    }
    return out;
}

} // namespace memscan
