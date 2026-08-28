#include "scan_service.h"

#include <algorithm>

namespace cortex::services {

bool ScanService::Exact(const std::vector<uint8_t>& needle,
                        std::vector<ScanResult>& results,
                        size_t maxResults,
                        std::string* error) const {
    if (error) error->clear();
    results.clear();
    if (needle.empty() || needle.size() > 4096 || maxResults == 0) {
        if (error) *error = "invalid_scan_pattern";
        return false;
    }

    std::string regionError;
    const auto regions = memory_.Regions(&regionError);
    if (!regionError.empty()) {
        if (error) *error = regionError;
        return false;
    }

    constexpr size_t kChunkSize = 1024 * 1024;
    std::vector<uint8_t> carry;

    for (const auto& region : regions) {
        if (!region.readable || region.size == 0) continue;
        carry.clear();

        uint64_t offset = 0;
        while (offset < region.size) {
            const size_t request = static_cast<size_t>(std::min<uint64_t>(kChunkSize, region.size - offset));
            std::vector<uint8_t> chunk;
            std::string readError;
            if (!memory_.Read(region.base + offset, request, chunk, &readError)) {
                carry.clear();
                offset += request;
                continue;
            }

            std::vector<uint8_t> window;
            window.reserve(carry.size() + chunk.size());
            window.insert(window.end(), carry.begin(), carry.end());
            window.insert(window.end(), chunk.begin(), chunk.end());

            auto it = window.begin();
            while (window.size() >= needle.size() && it <= window.end() - static_cast<std::ptrdiff_t>(needle.size())) {
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

} // namespace cortex::services
