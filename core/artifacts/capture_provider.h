#pragma once

// Network captures, read offline: pcap (micro/nanosecond, either byte order)
// and pcapng (sections, interfaces, enhanced/simple packets, decryption
// secret blocks). Packets are dissected only far enough to summarize what a
// capture contains: link, IPv4/IPv6, TCP/UDP flows, TLS ClientHello server
// names, HTTP request lines and DNS questions. Nothing is replayed or sent.

#include "artifact.h"

namespace cortex::artifacts {

class PcapProvider final : public Provider {
public:
    const char* Name() const override { return "pcap"; }
    const char* Version() const override { return "1"; }
    std::vector<std::string> Formats() const override { return {"pcap", "pcapng"}; }
    std::vector<Capability> Capabilities() const override;
    int Detect(const ByteSource& source) const override;
    json Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const override;
};

} // namespace cortex::artifacts
