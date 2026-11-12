#include "certificate_provider.h"
#include "bounded_reader.h"
#include "sha256.h"
#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace cortex::artifacts {
namespace {
using bounded::Error;
using bounded::View;
constexpr size_t kLimit = 8u << 20;
struct Node { uint8_t tag; size_t start, value, end; };
class Der {
public:
    explicit Der(const std::vector<uint8_t>& bytes) : v(bytes) {}
    Node At(size_t at, size_t end) {
        if (++nodes > 32768 || end > v.Size() || at >= end || end - at < 2) throw Error("der_node_budget_or_truncated");
        const size_t start = at; const uint8_t tag = v.U8(at++);
        if ((tag & 31) == 31 || tag == 0) throw Error("der_high_tag_or_eoc_unsupported");
        uint64_t n = v.U8(at++);
        if (n & 128) {
            const size_t count = static_cast<size_t>(n & 127);
            if (!count) throw Error("der_indefinite_length_unsupported");
            if (count > 4 || count > end - at || !v.U8(at)) throw Error("der_invalid_length");
            n = 0;
            for (size_t i = 0; i < count; ++i) n = (n << 8) | v.U8(at++);
            if (n < 128) throw Error("der_nonminimal_length");
        }
        if (n > end - at) throw Error("der_value_outside_parent");
        return {tag, start, at, at + static_cast<size_t>(n)};
    }
    std::vector<Node> Children(const Node& parent, size_t cap = 4096) {
        if (!(parent.tag & 32)) throw Error("der_primitive_has_no_children");
        std::vector<Node> out;
        for (size_t at = parent.value; at < parent.end;) {
            if (out.size() >= cap) throw Error("der_child_limit");
            const auto node = At(at, parent.end); out.push_back(node); at = node.end;
        }
        return out;
    }
    Node Root() { const auto n = At(0, v.Size()); if (n.end != v.Size()) throw Error("der_trailing_bytes"); return n; }
    View v;
private:
    size_t nodes = 0;
};
void Tag(const Node& node, uint8_t tag) { if (node.tag != tag) throw Error("der_unexpected_tag"); }
std::string Hex(const View& v) {
    static const char* digits = "0123456789abcdef"; std::string out;
    if (v.Size() > (1u << 20)) throw Error("der_hex_budget");
    for (size_t i = 0; i < v.Size(); ++i) { const auto b = v.U8(i); out += digits[b >> 4]; out += digits[b & 15]; }
    return out;
}
std::string Oid(Der& d, const Node& n) {
    Tag(n, 6); if (n.value == n.end || n.end - n.value > 128) throw Error("der_oid_empty_or_limit");
    uint64_t part = 0; bool first = true, fresh = true; std::string out;
    for (size_t at = n.value; at < n.end; ++at) {
        const uint8_t b = d.v.U8(at);
        if ((fresh && b == 0x80) || part > (UINT64_MAX >> 7)) throw Error("der_oid_overflow_or_nonminimal");
        part = (part << 7) | (b & 127); fresh = false;
        if (b & 128) continue;
        if (first) {
            const unsigned head = part < 40 ? 0 : part < 80 ? 1 : 2;
            out = std::to_string(head) + "." + std::to_string(part - head * 40); first = false;
        } else out += "." + std::to_string(part);
        part = 0; fresh = true;
    }
    if (!fresh) throw Error("der_oid_truncated");
    return out;
}
uint64_t Integer(Der& d, const Node& n) {
    Tag(n, 2); const size_t count = n.end - n.value;
    if (!count || count > 9 || (d.v.U8(n.value) & 128)) throw Error("der_integer_not_small_unsigned");
    if (count > 1 && d.v.U8(n.value) == 0 && !(d.v.U8(n.value + 1) & 128)) throw Error("der_nonminimal_integer");
    uint64_t result = 0;
    for (size_t i = n.value; i < n.end; ++i) {
        if (result > (UINT64_MAX >> 8)) throw Error("der_integer_overflow");
        result = (result << 8) | d.v.U8(i);
    }
    return result;
}
std::string Serial(Der& d, const Node& n) {
    Tag(n, 2);
    if (n.end == n.value || n.end - n.value > 64 || (d.v.U8(n.value) & 128)) throw Error("x509_invalid_serial");
    return Hex(d.v.Slice(n.value, n.end - n.value));
}
std::string Hash(const View& v) { Sha256 sha; sha.Update(v.Data(0, v.Size()), v.Size()); return Sha256::Hex(sha.Final()); }
std::string Algorithm(Der& d, const Node& n) {
    Tag(n, 0x30); const auto fields = d.Children(n, 3);
    if (fields.empty() || fields.size() > 2) throw Error("der_algorithm_invalid");
    return Oid(d, fields[0]);
}
std::string Text(Der& d, const Node& n) {
    if (n.end - n.value > 4096) throw Error("der_text_limit");
    const auto v = d.v.Slice(n.value, n.end - n.value);
    if (n.tag == 0x1e) return bounded::Utf16(v, false);
    if (n.tag != 0x0c && n.tag != 0x13 && n.tag != 0x14 && n.tag != 0x16 && n.tag != 0x17 && n.tag != 0x18)
        return Hex(v);
    return bounded::Utf8(v.Data(0, v.Size()), v.Size());
}
json Name(Der& d, const Node& node) {
    Tag(node, 0x30); json out = json::array();
    for (const auto& rdn : d.Children(node, 128)) {
        Tag(rdn, 0x31);
        for (const auto& attr : d.Children(rdn, 32)) {
            Tag(attr, 0x30); const auto fields = d.Children(attr, 3);
            if (fields.size() != 2) throw Error("x509_name_attribute_invalid");
            out.push_back({{"oid", Oid(d, fields[0])}, {"value", Text(d, fields[1])}});
        }
    }
    return out;
}
std::string Time(Der& d, const Node& n) {
    if (n.tag != 0x17 && n.tag != 0x18) throw Error("x509_validity_not_time");
    std::string raw = Text(d, n);
    const size_t count = n.tag == 0x17 ? 13 : 15;
    if (raw.size() != count || raw.back() != 'Z') throw Error("x509_unsupported_time_encoding");
    for (size_t i = 0; i + 1 < count; ++i) if (raw[i] < '0' || raw[i] > '9') throw Error("x509_invalid_time");
    if (n.tag == 0x17) raw = (raw.compare(0, 2, "50") < 0 ? "20" : "19") + raw;
    return raw.substr(0,4)+"-"+raw.substr(4,2)+"-"+raw.substr(6,2)+"T"+raw.substr(8,2)+":"+raw.substr(10,2)+":"+raw.substr(12,2)+"Z";
}
void BitString(Der& d, const Node& n) {
    Tag(n, 3);
    if (n.value == n.end || d.v.U8(n.value) > 7) throw Error("der_invalid_bit_string");
    const unsigned unused = d.v.U8(n.value);
    if ((n.end - n.value == 1 && unused) || (unused && (d.v.U8(n.end - 1) & ((1u << unused) - 1))))
        throw Error("der_nonzero_unused_bits");
}
json Certificate(Der& d, const Node& cert, size_t limit) {
    Tag(cert, 0x30); const auto parts = d.Children(cert, 4);
    if (parts.size() != 3) throw Error("x509_certificate_structure");
    Tag(parts[0], 0x30); BitString(d, parts[2]);
    const auto tbs = d.Children(parts[0], 16); size_t at = 0; uint64_t version = 0;
    if (!tbs.empty() && tbs[0].tag == 0xa0) {
        const auto explicitVersion = d.Children(tbs[0], 2);
        if (explicitVersion.size() != 1) throw Error("x509_version_invalid");
        version = Integer(d, explicitVersion[0]); if (version > 2) throw Error("x509_version_unsupported"); ++at;
    }
    if (tbs.size() < at + 6) throw Error("x509_tbs_incomplete");
    const auto serial = Serial(d, tbs[at]); const auto innerAlgorithm = Algorithm(d, tbs[at + 1]);
    const auto issuer = Name(d, tbs[at + 2]), subject = Name(d, tbs[at + 4]);
    Tag(tbs[at + 3], 0x30); const auto validity = d.Children(tbs[at + 3], 3);
    if (validity.size() != 2) throw Error("x509_validity_invalid");
    Tag(tbs[at + 5], 0x30); const auto spki = d.Children(tbs[at + 5], 3);
    if (spki.size() != 2) throw Error("x509_spki_invalid");
    BitString(d, spki[1]);
    const auto algorithm = Algorithm(d, parts[1]);
    json result = {{"version", version + 1}, {"serial", serial}, {"issuer", issuer}, {"subject", subject},
                   {"not_before", Time(d, validity[0])}, {"not_after", Time(d, validity[1])},
                   {"signature_algorithm_oid", algorithm}, {"tbs_signature_algorithm_oid", innerAlgorithm},
                   {"algorithm_oids_match", algorithm == innerAlgorithm},
                   {"public_key_algorithm_oid", Algorithm(d, spki[0])},
                   {"subject_public_key_info_sha256", Hash(d.v.Slice(tbs[at + 5].start, tbs[at + 5].end - tbs[at + 5].start))},
                   {"sha256", Hash(d.v.Slice(cert.start, cert.end - cert.start))},
                   {"signature_verified", false}, {"chain_verified", false}};
    // Matching names is not a signature test and must never be named self_signed.
    result["self_issued_name_match"] = issuer == subject;
    json extensions = json::array(); size_t extensionCount = 0; bool seenExtensions = false;
    for (size_t index = at + 6; index < tbs.size(); ++index) {
        if (tbs[index].tag == 0x81 || tbs[index].tag == 0x82) continue;
        if (tbs[index].tag != 0xa3 || seenExtensions) throw Error("x509_unexpected_tbs_field");
        seenExtensions = true;
        const auto wrapper = d.Children(tbs[index], 2);
        if (wrapper.size() != 1) throw Error("x509_extensions_wrapper");
        Tag(wrapper[0], 0x30); std::set<std::string> oids;
        for (const auto& ext : d.Children(wrapper[0], 256)) {
            Tag(ext, 0x30); const auto fields = d.Children(ext, 4);
            if (fields.size() < 2 || fields.size() > 3) throw Error("x509_extension_fields");
            const auto oid = Oid(d, fields[0]); if (!oids.insert(oid).second) throw Error("x509_duplicate_extension");
            bool critical = false; size_t value = 1;
            if (fields[value].tag == 1) {
                if (fields[value].end - fields[value].value != 1 ||
                    (d.v.U8(fields[value].value) != 0 && d.v.U8(fields[value].value) != 255)) throw Error("der_boolean_invalid");
                critical = d.v.U8(fields[value].value) != 0; ++value;
            }
            if (value + 1 != fields.size()) throw Error("x509_extension_value_missing");
            Tag(fields[value], 4); ++extensionCount;
            if (extensions.size() < limit) extensions.push_back({{"oid", oid}, {"critical", critical},
                {"encoded_bytes", fields[value].end - fields[value].value}});
        }
    }
    result["extensions"] = std::move(extensions); result["extension_count"] = extensionCount;
    result["extensions_truncated"] = extensionCount > limit;
    return result;
}

std::vector<uint8_t> Base64(const std::string& text) {
    std::string clean;
    for (unsigned char c : text) { if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue; clean += static_cast<char>(c); }
    if (clean.empty() || clean.size() % 4 || clean.size() > (kLimit * 4 / 3 + 8)) throw Error("pem_base64_length");
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A'; if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52; return c == '+' ? 62 : c == '/' ? 63 : -1;
    };
    std::vector<uint8_t> out;
    for (size_t at = 0; at < clean.size(); at += 4) {
        const int a = value(clean[at]), b = value(clean[at + 1]);
        const int c = clean[at + 2] == '=' ? 0 : value(clean[at + 2]);
        const int e = clean[at + 3] == '=' ? 0 : value(clean[at + 3]);
        const bool pad2 = clean[at + 2] == '=', pad1 = clean[at + 3] == '=';
        if (a < 0 || b < 0 || c < 0 || e < 0 || (pad2 && !pad1) ||
            ((pad1 || pad2) && at + 4 != clean.size()) || (pad2 && (b & 15)) || (pad1 && !pad2 && (c & 3)))
            throw Error("pem_base64_invalid");
        out.push_back(static_cast<uint8_t>((a << 2) | (b >> 4)));
        if (!pad2) out.push_back(static_cast<uint8_t>((b << 4) | (c >> 2)));
        if (!pad1) out.push_back(static_cast<uint8_t>((c << 6) | e));
    }
    return out;
}
struct Parsed { json report; std::map<std::string, std::vector<uint8_t>> children; };
void AddChild(Parsed& out, Der& d, const Node& node, const std::string& name, bool valueOnly = false) {
    const size_t from = valueOnly ? node.value : node.start;
    const auto view = d.v.Slice(from, node.end - from);
    out.children[name] = std::vector<uint8_t>(view.Data(0, view.Size()), view.Data(0, view.Size()) + view.Size());
}
Parsed ParseDer(const std::vector<uint8_t>& bytes, size_t limit) {
    Der d(bytes); const auto root = d.Root(); Tag(root, 0x30);
    const auto fields = d.Children(root, 4); Parsed out;
    if (fields.size() == 3 && fields[0].tag == 0x30 && fields[1].tag == 0x30 && fields[2].tag == 3) {
        out.report = Certificate(d, root, limit); out.report["format"] = "x509"; return out;
    }
    if (fields.size() != 2 || Oid(d, fields[0]) != "1.2.840.113549.1.7.2") throw Error("certificate_or_signed_data_expected");
    Tag(fields[1], 0xa0); const auto wrapped = d.Children(fields[1], 2);
    if (wrapped.size() != 1) throw Error("cms_signed_data_wrapper");
    Tag(wrapped[0], 0x30); const auto sd = d.Children(wrapped[0], 8);
    if (sd.size() < 4) throw Error("cms_signed_data_incomplete");
    const uint64_t version = Integer(d, sd[0]); Tag(sd[1], 0x31); Tag(sd[2], 0x30);
    json digests = json::array();
    for (const auto& alg : d.Children(sd[1], 128)) { const auto oid = Algorithm(d, alg); if (digests.size() < limit) digests.push_back(oid); }
    const auto content = d.Children(sd[2], 3);
    if (content.empty() || content.size() > 2) throw Error("cms_encapsulated_content_invalid");
    const std::string contentType = Oid(d, content[0]);
    if (content.size() == 2) {
        Tag(content[1], 0xa0); const auto value = d.Children(content[1], 2);
        if (value.size() != 1) throw Error("cms_content_wrapper_invalid");
        // CMS wraps octets. Legacy PKCS#7 may carry a structured ASN.1 value.
        AddChild(out, d, value[0], "content.der", value[0].tag == 4);
    }
    json certs = json::array(), signers = json::array(); size_t certCount = 0, signerCount = 0;
    size_t at = 3;
    if (at < sd.size() && sd[at].tag == 0xa0) {
        for (const auto& cert : d.Children(sd[at++], 128)) {
            if (cert.tag != 0x30) throw Error("cms_certificate_choice_unsupported");
            const auto row = Certificate(d, cert, limit);
            AddChild(out, d, cert, "certificate-" + std::to_string(certCount) + ".der"); ++certCount;
            if (certs.size() < limit) certs.push_back(row);
        }
    }
    bool crls = false;
    if (at < sd.size() && sd[at].tag == 0xa1) { crls = true; ++at; }
    if (at + 1 != sd.size()) throw Error("cms_signers_missing_or_extra_fields");
    Tag(sd[at], 0x31);
    for (const auto& signer : d.Children(sd[at], 128)) {
        Tag(signer, 0x30); const auto values = d.Children(signer, 8);
        if (values.size() < 5) throw Error("cms_signer_incomplete");
        json row = {{"version", Integer(d, values[0])}, {"digest_algorithm_oid", Algorithm(d, values[2])}, {"verified", false}};
        if (values[1].tag == 0x30) {
            const auto sid = d.Children(values[1], 3); if (sid.size() != 2) throw Error("cms_signer_identifier_invalid");
            row["issuer"] = Name(d, sid[0]); row["serial"] = Serial(d, sid[1]);
        } else if (values[1].tag == 0x80) row["subject_key_identifier"] = Hex(d.v.Slice(values[1].value, values[1].end - values[1].value));
        else throw Error("cms_signer_identifier_unsupported");
        size_t index = 3;
        if (values[index].tag == 0xa0) { row["signed_attributes_present"] = true; ++index; }
        if (index + 2 > values.size()) throw Error("cms_signer_signature_missing");
        row["signature_algorithm_oid"] = Algorithm(d, values[index++]); Tag(values[index], 4);
        row["signature_bytes"] = values[index].end - values[index].value; ++index;
        if (index < values.size()) { if (index + 1 != values.size() || values[index].tag != 0xa1) throw Error("cms_extra_signer_fields"); row["unsigned_attributes_present"] = true; }
        ++signerCount; if (signers.size() < limit) signers.push_back(std::move(row));
    }
    out.report = {{"format", contentType == "1.3.6.1.4.1.311.10.1" ? "pkcs7-catalog" : "pkcs7"},
                  {"version", version}, {"content_type_oid", contentType}, {"digest_algorithms", digests},
                  {"certificate_count", certCount}, {"certificates", certs}, {"signer_count", signerCount}, {"signers", signers},
                  {"truncated", certCount > limit || signerCount > limit}, {"revocation_lists_present", crls},
                  {"signature_verified", false}, {"chain_verified", false}, {"network_requests", false}};
    if (contentType == "1.3.6.1.4.1.311.10.1") out.report["catalog_entries"] = "not_decoded; raw content.der is available";
    return out;
}
Parsed Parse(const ByteSource& source, size_t limit) {
    const auto bytes = bounded::Read(source, 0, source.Size(), kLimit);
    if (bytes.empty()) throw Error("certificate_empty_input");
    if (bytes.front() == 0x30) return ParseDer(bytes, limit);
    const std::string text(bytes.begin(), bytes.end());
    size_t at = 0, count = 0; Parsed out; json certs = json::array();
    while (true) {
        const size_t begin = text.find("-----BEGIN ", at);
        if (begin == std::string::npos) break;
        if (count >= 128) throw Error("pem_block_limit");
        const size_t labelEnd = text.find("-----", begin + 11);
        if (labelEnd == std::string::npos || labelEnd - begin > 80) throw Error("pem_bad_begin");
        const std::string label = text.substr(begin + 11, labelEnd - begin - 11);
        if (label != "CERTIFICATE" && label != "PKCS7" && label != "CMS") throw Error("pem_label_unsupported");
        const std::string closing = "-----END " + label + "-----";
        const size_t end = text.find(closing, labelEnd + 5);
        if (end == std::string::npos) throw Error("pem_missing_end");
        const auto der = Base64(text.substr(labelEnd + 5, end - labelEnd - 5));
        auto item = ParseDer(der, limit);
        if (label != "CERTIFICATE") {
            if (count || text.find("-----BEGIN ", end + closing.size()) != std::string::npos || item.report["format"] == "x509")
                throw Error("pem_mixed_cms_bundle_unsupported");
            item.report["encoding"] = "pem"; return item;
        }
        if (item.report["format"] != "x509") throw Error("pem_certificate_label_mismatch");
        out.children["certificate-" + std::to_string(count) + ".der"] = der; ++count;
        if (certs.size() < limit) certs.push_back(std::move(item.report));
        at = end + closing.size();
    }
    if (!count) throw Error("pem_no_certificate_blocks");
    out.report = {{"format", "pem-certificates"}, {"encoding", "pem"}, {"certificate_count", count},
                  {"certificates", certs}, {"truncated", count > limit}, {"signature_verified", false}, {"chain_verified", false}};
    return out;
}
} // namespace

std::vector<Capability> CertificateProvider::Capabilities() const {
    return {{"static", CapabilityStatus::Experimental, "cortex_format_next_tests; OpenSSL fixtures",
             "X.509 DER/PEM metadata, definite-length CMS SignedData and catalog content inventory; no signature/trust verification"},
            {"decompile", CapabilityStatus::Unavailable, "", "not applicable"},
            {"live", CapabilityStatus::Unavailable, "", "no activation, import, network or decryption"}};
}
int CertificateProvider::Detect(const ByteSource& source) const {
    if (!source.Size() || source.Size() > kLimit) return 0;
    const auto head = source.ReadSome(0, 64);
    if (head.empty()) return 0;
    const std::string text(head.begin(), head.end());
    if (head[0] != 0x30 && text.rfind("-----BEGIN CERTIFICATE-----", 0) != 0 &&
        text.rfind("-----BEGIN PKCS7-----", 0) != 0 && text.rfind("-----BEGIN CMS-----", 0) != 0) return 0;
    try { (void)Parse(source, 0); return 65; } catch (const Error&) { return 0; }
}
json CertificateProvider::Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const {
    error.clear(); try { return Parse(source, options.maxEntries).report; }
    catch (const Error& ex) { error = ex.what(); return nullptr; }
}
std::vector<Child> CertificateProvider::Children(const ByteSource& source, const InspectOptions& options, std::string& error) const {
    error.clear(); try {
        auto parsed = Parse(source, 0); std::vector<Child> children;
        for (const auto& [name, bytes] : parsed.children) {
            if (children.size() >= options.maxEntries) break;
            Child child; child.name = name; child.size = bytes.size(); children.push_back(std::move(child));
        }
        return children;
    } catch (const Error& ex) { error = ex.what(); return {}; }
}
ByteSourcePtr CertificateProvider::OpenChild(ByteSourcePtr source, const Child& child, std::string& error) const {
    error.clear(); try {
        if (!source || !child.readable) throw Error("certificate_child_unreadable");
        auto parsed = Parse(*source, 0); auto found = parsed.children.find(child.name);
        if (found == parsed.children.end()) throw Error("certificate_child_not_found");
        return FromBytes(std::move(found->second), source->Name() + "!" + child.name);
    } catch (const Error& ex) { error = ex.what(); return {}; }
}
} // namespace cortex::artifacts
