#include "capture_provider.h"
#include "bounded_reader.h"

#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <tuple>

namespace cortex::artifacts {
namespace {
using bounded::Error;
using bounded::View;

std::string Ipv4(const View& v, uint64_t at) {
    return std::to_string(v.U8(at)) + "." + std::to_string(v.U8(at + 1)) + "." + std::to_string(v.U8(at + 2)) + "." + std::to_string(v.U8(at + 3));
}

std::string Ipv6(const View& v, uint64_t at) {
    static const char* d = "0123456789abcdef";
    std::string out;
    for (int i = 0; i < 8; ++i) {
        if (i) out += ':';
        const uint16_t w = v.U16(at + i * 2, false);
        bool lead = true;
        for (int s = 12; s >= 0; s -= 4) {
            const unsigned n = (w >> s) & 15;
            if (lead && n == 0 && s) continue;
            lead = false; out += d[n];
        }
    }
    return out;
}

struct Summary {
    uint64_t packets = 0, bytes = 0;
    std::map<std::string, uint64_t> protocols;
    struct Flow { uint64_t packets = 0, bytes = 0; };
    std::map<std::tuple<std::string, std::string, uint16_t, std::string, uint16_t>, Flow> flows;
    std::set<std::string> sni, http, dns;
    double first = 0, last = 0;
    bool any = false;
    std::set<uint32_t> linkTypes;

    void Count(const char* p) { ++protocols[p]; }

    // TLS ClientHello server_name (RFC 6066), when the record starts here.
    void Tls(const View& p) {
        if (p.Size() < 9 || p.U8(0) != 0x16 || p.U8(1) != 3 || p.U8(5) != 1) return;
        uint64_t at = 9 + 2 + 32;                         // record + handshake headers, version, random
        at += 1 + p.U8(at);                               // session id
        at += 2 + p.U16(at, false);                       // cipher suites
        at += 1 + p.U8(at);                               // compression methods
        const uint64_t end = at + 2 + p.U16(at, false);
        at += 2;
        while (at + 4 <= end) {
            const uint16_t type = p.U16(at, false), len = p.U16(at + 2, false);
            if (type == 0) {
                const uint64_t list = at + 4;
                if (p.U8(list + 2) == 0) {
                    const uint16_t n = p.U16(list + 3, false);
                    if (sni.size() < 1024) sni.insert(p.Text(list + 5, n));
                }
                return;
            }
            at += 4 + uint64_t(len);
        }
    }

    void Http(const View& p) {
        static const char* methods[] = {"GET ", "POST ", "PUT ", "HEAD ", "DELETE ", "OPTIONS ", "PATCH ", "CONNECT "};
        const size_t n = std::min<size_t>(p.Size(), 4096);
        const std::string text = p.Text(0, n);
        bool request = false;
        for (const char* m : methods) request = request || text.rfind(m, 0) == 0;
        if (!request) return;
        const size_t eol = text.find("\r\n");
        std::string line = bounded::Clip(text.substr(0, eol == std::string::npos ? text.size() : eol), 512);
        const size_t host = text.find("\r\nHost: ");
        if (host != std::string::npos) {
            const size_t e = text.find("\r\n", host + 8);
            line += " [host " + text.substr(host + 8, e == std::string::npos ? 0 : e - host - 8) + "]";
        }
        if (http.size() < 1024) http.insert(line);
    }

    void Dns(const View& p) {
        if (p.Size() < 12 || (p.U8(2) & 0x80) || p.U16(4, false) == 0) return;  // queries with a question
        uint64_t at = 12;
        std::string name;
        for (int labels = 0; labels < 128; ++labels) {
            const uint8_t len = p.U8(at);
            if (!len) break;
            if (len >= 0xC0) return;  // a query's first name is never compressed
            if (!name.empty()) name += '.';
            name += p.Text(at + 1, len);
            at += 1 + uint64_t(len);
            if (name.size() > 255) return;
        }
        if (!name.empty() && dns.size() < 1024) dns.insert(name);
    }

    void Transport(uint8_t proto, const View& seg, const std::string& family, const std::string& src, const std::string& dst, uint64_t length) {
        if (proto == 6 && seg.Size() >= 20) {
            Count("tcp");
            const uint16_t sp = seg.U16(0, false), dp = seg.U16(2, false);
            const uint64_t off = uint64_t(seg.U8(12) >> 4) * 4;
            Flow& f = flows[{"tcp/" + family, src, sp, dst, dp}]; ++f.packets; f.bytes += length;
            if (off >= 20 && off <= seg.Size()) {
                const View payload = seg.Slice(off, seg.Size() - off);
                if (payload.Size()) { try { Tls(payload); } catch (const Error&) {} try { Http(payload); } catch (const Error&) {} }
            }
        } else if (proto == 17 && seg.Size() >= 8) {
            Count("udp");
            const uint16_t sp = seg.U16(0, false), dp = seg.U16(2, false);
            Flow& f = flows[{"udp/" + family, src, sp, dst, dp}]; ++f.packets; f.bytes += length;
            if (sp == 53 || dp == 53 || sp == 5353 || dp == 5353) { try { Dns(seg.Slice(8, seg.Size() - 8)); } catch (const Error&) {} }
        } else if (proto == 1 || proto == 58) {
            Count(proto == 1 ? "icmp" : "icmpv6");
        } else {
            Count("other_ip");
        }
    }

    void Ip(const View& p, uint64_t length) {
        if (!p.Size()) return;
        const unsigned version = p.U8(0) >> 4;
        if (version == 4 && p.Size() >= 20) {
            Count("ipv4");
            const uint64_t ihl = uint64_t(p.U8(0) & 15) * 4;
            const uint64_t total = std::min<uint64_t>(p.U16(2, false), p.Size());
            if (ihl < 20 || ihl > total) return;
            if (p.U16(6, false) & 0x1FFF) { Count("ipv4_fragment"); return; }
            Transport(p.U8(9), p.Slice(ihl, total - ihl), "ipv4", Ipv4(p, 12), Ipv4(p, 16), length);
        } else if (version == 6 && p.Size() >= 40) {
            Count("ipv6");
            uint8_t next = p.U8(6);
            uint64_t at = 40;
            for (int i = 0; i < 8 && (next == 0 || next == 43 || next == 60); ++i) {  // hop-by-hop, routing, destination options
                next = p.U8(at);
                at += (uint64_t(p.U8(at + 1)) + 1) * 8;
            }
            if (at > p.Size()) return;
            Transport(next, p.Slice(at, p.Size() - at), "ipv6", Ipv6(p, 8), Ipv6(p, 24), length);
        }
    }

    void Packet(uint32_t link, const View& data, uint64_t original, double time) {
        ++packets; bytes += original;
        if (!any || time < first) first = time;
        if (!any || time > last) last = time;
        any = true;
        linkTypes.insert(link);
        try {
            switch (link) {
                case 1: {  // Ethernet, with up to two VLAN tags
                    uint64_t at = 12;
                    uint16_t type = data.U16(at, false);
                    for (int i = 0; i < 2 && (type == 0x8100 || type == 0x88A8); ++i) { at += 4; type = data.U16(at, false); }
                    at += 2;
                    const View payload = data.Slice(at, data.Size() - at);
                    if (type == 0x0800 || type == 0x86DD) Ip(payload, original);
                    else if (type == 0x0806) Count("arp");
                    else Count("other_ethernet");
                    break;
                }
                case 0: { const uint32_t family = data.U32(0); if (family == 2 || family == 24 || family == 28 || family == 30) Ip(data.Slice(4, data.Size() - 4), original); break; }
                case 101: case 228: case 229: Ip(data, original); break;
                case 113: { const uint16_t type = data.U16(14, false); if (type == 0x0800 || type == 0x86DD) Ip(data.Slice(16, data.Size() - 16), original); break; }
                case 276: { const uint16_t type = data.U16(0, false); if (type == 0x0800 || type == 0x86DD) Ip(data.Slice(20, data.Size() - 20), original); break; }
                default: Count("unknown_link"); break;
            }
        } catch (const Error&) { Count("truncated_packet"); }
    }

    json Json(const InspectOptions& options) const {
        json flowList = json::array();
        for (const auto& [key, f] : flows) {
            if (flowList.size() >= options.maxEntries) break;
            flowList.push_back({{"protocol", std::get<0>(key)}, {"source", std::get<1>(key)}, {"source_port", std::get<2>(key)},
                                {"destination", std::get<3>(key)}, {"destination_port", std::get<4>(key)},
                                {"packets", f.packets}, {"bytes", f.bytes}});
        }
        return {{"packets", packets}, {"bytes", bytes}, {"link_types", linkTypes}, {"protocols", protocols},
                {"flow_count", flows.size()}, {"flows", flowList}, {"tls_server_names", sni},
                {"http_requests", http}, {"dns_queries", dns},
                {"first_timestamp", any ? json(first) : json(nullptr)}, {"last_timestamp", any ? json(last) : json(nullptr)}};
    }
};

json Pcap(const View& v, const InspectOptions& options) {
    const uint32_t magic = v.U32(0);
    bool little = true, nano = false;
    if (magic == 0xA1B2C3D4) {} else if (magic == 0xA1B23C4D) nano = true;
    else if (magic == 0xD4C3B2A1) little = false; else if (magic == 0x4D3CB2A1) { little = false; nano = true; }
    else throw Error("pcap_magic");
    const uint32_t link = v.U32(20, little) & 0x0FFFFFFF, snaplen = v.U32(16, little);
    Summary s;
    uint64_t at = 24;
    bool truncatedFile = false;
    while (at < v.Size()) {
        if (!v.Has(at, 16)) { truncatedFile = true; break; }
        const uint32_t sec = v.U32(at, little), frac = v.U32(at + 4, little), incl = v.U32(at + 8, little), orig = v.U32(at + 12, little);
        if (incl > (256u << 20)) throw Error("pcap_record_length_invalid");
        if (!v.Has(at + 16, incl)) { truncatedFile = true; break; }
        s.Packet(link, v.Slice(at + 16, incl), orig, sec + frac / (nano ? 1e9 : 1e6));
        at += 16 + uint64_t(incl);
        if (s.packets > 50000000) throw Error("pcap_packet_limit");
    }
    json result = s.Json(options);
    result["format"] = "pcap";
    result["version"] = std::to_string(v.U16(4, little)) + "." + std::to_string(v.U16(6, little));
    result["timestamp_resolution"] = nano ? "nanosecond" : "microsecond";
    result["big_endian"] = !little;
    result["snaplen"] = snaplen;
    if (truncatedFile) result["warnings"] = json::array({"capture_ends_inside_a_record"});
    return result;
}

std::string PcapngString(const View& v, uint64_t at, uint16_t len) { return v.Text(at, len); }

json Pcapng(const View& v, const InspectOptions& options) {
    Summary s;
    json sections = json::array(), interfaces = json::array();
    struct Interface { uint32_t link; double resolution; };
    std::vector<Interface> ifaces;
    bool little = true, truncatedFile = false;
    uint64_t at = 0, secrets = 0, otherBlocks = 0;
    while (at < v.Size()) {
        if (!v.Has(at, 12)) { truncatedFile = true; break; }
        uint32_t type = v.U32(at, little);
        if (type == 0x0A0D0D0A) {
            const uint32_t bom = v.U32(at + 8);
            if (bom == 0x1A2B3C4D) little = true; else if (bom == 0x4D3C2B1A) little = false; else throw Error("pcapng_byte_order_magic");
            ifaces.clear();
        }
        const uint32_t length = v.U32(at + 4, little);
        if (length < 12 || length % 4 || !v.Has(at, length)) { truncatedFile = true; break; }
        if (v.U32(at + length - 4, little) != length) throw Error("pcapng_trailing_length_mismatch");
        const View body = v.Slice(at + 8, length - 12);
        // Options start after the fixed fields: (code, length, value padded to 4).
        auto options_ = [&](uint64_t from, const std::function<void(uint16_t, uint64_t, uint16_t)>& visit) {
            uint64_t o = from;
            while (o + 4 <= body.Size()) {
                const uint16_t code = body.U16(o, little), len = body.U16(o + 2, little);
                if (code == 0) break;
                if (!body.Has(o + 4, len)) break;
                visit(code, o + 4, len);
                o += 4 + ((uint64_t(len) + 3) & ~uint64_t(3));
            }
        };
        switch (type) {
            case 0x0A0D0D0A: {
                json shb = {{"version", std::to_string(body.U16(4, little)) + "." + std::to_string(body.U16(6, little))}, {"big_endian", !little}};
                options_(16, [&](uint16_t code, uint64_t o, uint16_t len) {
                    if (code == 2) shb["hardware"] = PcapngString(body, o, len);
                    else if (code == 3) shb["os"] = PcapngString(body, o, len);
                    else if (code == 4) shb["application"] = PcapngString(body, o, len);
                    else if (code == 1) shb["comment"] = PcapngString(body, o, len);
                });
                if (sections.size() < 64) sections.push_back(std::move(shb));
                break;
            }
            case 0x00000001: {
                Interface i{body.U16(0, little), 1e-6};
                json row = {{"link_type", i.link}, {"snaplen", body.U32(4, little)}};
                options_(8, [&](uint16_t code, uint64_t o, uint16_t len) {
                    if (code == 2) row["name"] = PcapngString(body, o, len);
                    else if (code == 3) row["description"] = PcapngString(body, o, len);
                    else if (code == 9 && len >= 1) {
                        const uint8_t r = body.U8(o);
                        i.resolution = (r & 0x80) ? std::pow(2.0, -double(r & 0x7F)) : std::pow(10.0, -double(r));
                        row["timestamp_resolution"] = i.resolution;
                    }
                });
                if (ifaces.size() > 4096) throw Error("pcapng_interface_limit");
                ifaces.push_back(i);
                if (interfaces.size() < 256) interfaces.push_back(std::move(row));
                break;
            }
            case 0x00000006: case 0x00000002: {
                const uint32_t id = type == 6 ? body.U32(0, little) : body.U16(0, little);
                if (id >= ifaces.size()) throw Error("pcapng_interface_id_invalid");
                const uint64_t ts = (uint64_t(body.U32(4, little)) << 32) | body.U32(8, little);
                const uint32_t captured = body.U32(12, little), original = body.U32(16, little);
                s.Packet(ifaces[id].link, body.Slice(20, captured), original, double(ts) * ifaces[id].resolution);
                break;
            }
            case 0x00000003: {
                if (ifaces.empty()) throw Error("pcapng_simple_packet_without_interface");
                const uint32_t original = body.U32(0, little);
                const uint64_t captured = std::min<uint64_t>(original, body.Size() - 4);
                s.Packet(ifaces[0].link, body.Slice(4, captured), original, 0);
                break;
            }
            case 0x0000000A: ++secrets; break;
            default: ++otherBlocks; break;
        }
        at += length;
        if (s.packets > 50000000) throw Error("pcapng_packet_limit");
    }
    if (sections.empty()) throw Error("pcapng_no_complete_section_header");
    json result = s.Json(options);
    result["format"] = "pcapng";
    result["sections"] = sections;
    result["interfaces"] = interfaces;
    result["decryption_secrets_blocks"] = secrets;
    if (secrets) result["note"] = "the capture embeds TLS key material; Cortex lists it and does not decrypt";
    result["other_blocks"] = otherBlocks;
    if (truncatedFile) result["warnings"] = json::array({"capture_ends_inside_a_block"});
    return result;
}

} // namespace

std::vector<Capability> PcapProvider::Capabilities() const {
    return {{"static", CapabilityStatus::Experimental, "cortex_format_more_tests; dpkt-written captures with a ClientHello from Python's ssl",
             "pcap and pcapng; Ethernet/raw/SLL/loopback links; IPv4/IPv6; TCP/UDP flows; TLS SNI, HTTP request lines, DNS questions"},
            {"decompile", CapabilityStatus::Unavailable, "", "not applicable"},
            {"live", CapabilityStatus::Unavailable, "", "packets are never replayed or sent; TLS is not decrypted"}};
}

int PcapProvider::Detect(const ByteSource& source) const {
    uint32_t m = 0;
    if (!ReadU32(source, 0, m)) return 0;
    if (m == 0xA1B2C3D4 || m == 0xA1B23C4D || m == 0xD4C3B2A1 || m == 0x4D3CB2A1) return 70;
    if (m == 0x0A0D0D0A) return 70;
    return 0;
}

json PcapProvider::Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const {
    error.clear();
    try {
        const auto bytes = bounded::Read(source, 0, source.Size(), 2048u << 20);
        const View v(bytes);
        return v.U32(0) == 0x0A0D0D0A ? Pcapng(v, options) : Pcap(v, options);
    } catch (const Error& e) { error = e.what(); return nullptr; }
}

} // namespace cortex::artifacts
