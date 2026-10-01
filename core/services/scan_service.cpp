#include "scan_service.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <type_traits>

namespace cortex::services {
namespace {

bool IsCancelled(const std::atomic_bool* cancelled) {
    return cancelled && cancelled->load(std::memory_order_relaxed);
}

template <typename T>
bool CompareTyped(const std::vector<uint8_t>& current,
                  const std::vector<uint8_t>& previous,
                  ScanComparison comparison) {
    if (current.size() != sizeof(T) || previous.size() != sizeof(T)) return false;
    T now{};
    T before{};
    std::memcpy(&now, current.data(), sizeof(T));
    std::memcpy(&before, previous.data(), sizeof(T));
    if constexpr (std::is_floating_point_v<T>) {
        if (!std::isfinite(now) || !std::isfinite(before)) return false;
    }
    switch (comparison) {
        case ScanComparison::Increased: return now > before;
        case ScanComparison::Decreased: return now < before;
        default: return false;
    }
}

bool MatchesRefinement(const std::vector<uint8_t>& current,
                       const std::vector<uint8_t>& previous,
                       ScanValueKind kind,
                       ScanComparison comparison,
                       const std::vector<uint8_t>& exactValue) {
    switch (comparison) {
        case ScanComparison::Exact:
            return !exactValue.empty() && current == exactValue;
        case ScanComparison::Changed:
            return current != previous;
        case ScanComparison::Unchanged:
            return current == previous;
        case ScanComparison::Increased:
        case ScanComparison::Decreased:
            break;
    }

    switch (kind) {
        case ScanValueKind::I32: return CompareTyped<int32_t>(current, previous, comparison);
        case ScanValueKind::I64: return CompareTyped<int64_t>(current, previous, comparison);
        case ScanValueKind::F32: return CompareTyped<float>(current, previous, comparison);
        case ScanValueKind::F64: return CompareTyped<double>(current, previous, comparison);
        default: return false;
    }
}

} // namespace

bool ScanService::Exact(const target::SessionPtr& session,
                        const std::vector<uint8_t>& needle,
                        std::vector<ScanResult>& results,
                        size_t maxResults,
                        std::string* error,
                        const std::atomic_bool* cancelled) {
    if (error) error->clear();
    results.clear();
    if (!session || !session->Alive()) {
        if (error) *error = "no_active_session";
        return false;
    }
    if (!session->Capabilities().Has(target::Capability::MemoryScan) ||
        !session->Capabilities().Has(target::Capability::MemoryRead)) {
        if (error) *error = "memory_scan_not_supported";
        return false;
    }
    if (needle.empty() || needle.size() > 4096 || maxResults == 0) {
        if (error) *error = "invalid_scan_pattern";
        return false;
    }

    const auto regions = session->MemoryRegions();
    constexpr size_t kChunkSize = 1024 * 1024;
    std::vector<uint8_t> carry;

    for (const auto& region : regions) {
        if (IsCancelled(cancelled)) {
            if (error) *error = "scan_cancelled";
            return false;
        }
        if (!region.readable || region.size == 0) continue;
        carry.clear();

        uint64_t offset = 0;
        while (offset < region.size) {
            if (IsCancelled(cancelled)) {
                if (error) *error = "scan_cancelled";
                return false;
            }

            const size_t request = static_cast<size_t>(std::min<uint64_t>(kChunkSize, region.size - offset));
            std::vector<uint8_t> chunk(request);
            size_t read = 0;
            if (!session->ReadMemory(region.base + offset, chunk.data(), chunk.size(), &read) || read != chunk.size()) {
                carry.clear();
                offset += request;
                continue;
            }

            std::vector<uint8_t> window;
            window.reserve(carry.size() + chunk.size());
            window.insert(window.end(), carry.begin(), carry.end());
            window.insert(window.end(), chunk.begin(), chunk.end());

            auto it = window.begin();
            while (window.size() >= needle.size() &&
                   it <= window.end() - static_cast<std::ptrdiff_t>(needle.size())) {
                it = std::search(it, window.end(), needle.begin(), needle.end());
                if (it == window.end()) break;
                const size_t index = static_cast<size_t>(std::distance(window.begin(), it));
                const uint64_t carryBase = region.base + offset - static_cast<uint64_t>(carry.size());
                results.push_back({carryBase + index, needle});
                if (results.size() >= maxResults) return true;
                ++it;
            }

            const size_t keep = needle.size() > 1 ? std::min(needle.size() - 1, window.size()) : 0;
            carry.assign(window.end() - static_cast<std::ptrdiff_t>(keep), window.end());
            offset += request;
        }
    }

    return true;
}

bool ScanService::Refine(const target::SessionPtr& session,
                         const std::vector<ScanResult>& previous,
                         ScanValueKind kind,
                         ScanComparison comparison,
                         const std::vector<uint8_t>& exactValue,
                         std::vector<ScanResult>& results,
                         std::string* error,
                         const std::atomic_bool* cancelled) {
    if (error) error->clear();
    results.clear();
    if (!session || !session->Alive()) {
        if (error) *error = "no_active_session";
        return false;
    }
    if (!session->Capabilities().Has(target::Capability::MemoryRead)) {
        if (error) *error = "memory_read_not_supported";
        return false;
    }
    if (previous.empty()) {
        if (error) *error = "no_previous_scan";
        return false;
    }
    if (comparison == ScanComparison::Exact) {
        if (exactValue.empty()) {
            if (error) *error = "invalid_scan_value";
            return false;
        }
        if (!previous.front().value.empty() && exactValue.size() != previous.front().value.size()) {
            if (error) *error = "scan_value_size_changed";
            return false;
        }
    }
    if ((comparison == ScanComparison::Increased || comparison == ScanComparison::Decreased) &&
        (kind == ScanValueKind::Bytes || kind == ScanValueKind::String)) {
        if (error) *error = "comparison_not_supported_for_type";
        return false;
    }

    results.reserve(previous.size());
    for (const auto& old : previous) {
        if (IsCancelled(cancelled)) {
            if (error) *error = "scan_cancelled";
            return false;
        }
        if (old.value.empty()) continue;

        std::vector<uint8_t> current(old.value.size());
        size_t read = 0;
        if (!session->ReadMemory(old.address, current.data(), current.size(), &read) || read != current.size())
            continue;

        if (MatchesRefinement(current, old.value, kind, comparison, exactValue))
            results.push_back({old.address, std::move(current)});
    }
    return true;
}

} // namespace cortex::services
