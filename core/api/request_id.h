#pragma once

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

namespace api::request_id {

inline std::string Format(uint64_t tickMs, uint32_t processId, uint64_t sequence) {
    std::ostringstream out;
    out << "cx-" << std::hex << std::nouppercase
        << tickMs << '-' << processId << '-' << sequence;
    return out.str();
}

} // namespace api::request_id
