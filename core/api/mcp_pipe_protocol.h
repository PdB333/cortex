#pragma once

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

namespace api::mcp_pipe_protocol {

inline constexpr std::uint32_t kMaxFrameBytes = 16u * 1024u * 1024u;

// The pipe endpoint is derived from the already-random API token so the host
// and injected runtime can rendezvous without another discovery file. The
// token itself is still sent inside the envelope and validated in constant
// time; the 64-bit hash is only an endpoint identifier, not authentication.
inline std::string PipeNameForToken(const std::string& token) {
    std::uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : token) {
        hash ^= static_cast<std::uint64_t>(c);
        hash *= 1099511628211ull;
    }
    std::ostringstream out;
    out << "\\\\.\\pipe\\cortex-mcp-"
        << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}

} // namespace api::mcp_pipe_protocol
