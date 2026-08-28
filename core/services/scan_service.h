#pragma once

#include "target/session.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cortex::services {

enum class ScanValueKind : uint8_t {
    Bytes = 0,
    I32,
    I64,
    F32,
    F64,
    String
};

enum class ScanComparison : uint8_t {
    Changed = 0,
    Increased,
    Decreased
};

struct ScanResult {
    uint64_t address = 0;
    std::vector<uint8_t> value;
};

class ScanService {
public:
    static bool Exact(const target::SessionPtr& session,
                      const std::vector<uint8_t>& needle,
                      std::vector<ScanResult>& results,
                      size_t maxResults = 5000,
                      std::string* error = nullptr,
                      const std::atomic_bool* cancelled = nullptr);

    static bool Refine(const target::SessionPtr& session,
                       const std::vector<ScanResult>& previous,
                       ScanValueKind kind,
                       ScanComparison comparison,
                       std::vector<ScanResult>& results,
                       std::string* error = nullptr,
                       const std::atomic_bool* cancelled = nullptr);
};

} // namespace cortex::services
