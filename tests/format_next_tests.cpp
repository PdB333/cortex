#include "artifacts/artifact.h"
#include "artifacts/archive_provider.h"
#include "artifacts/certificate_provider.h"
#include "artifacts/typelib_provider.h"
#include "artifacts/inflate.h"
#include <algorithm>
#include <functional>
#include <iostream>
#include <random>
#include <string>
#include <vector>

using namespace cortex::artifacts;
namespace {
struct Tests {
    std::string fixtures;
    int checks = 0, failures = 0;
    void Check(bool ok, const std::string& label) {
        ++checks;
        if (!ok) { ++failures; std::cerr << "FAIL " << label << '\n'; }
    }
    ByteSourcePtr File(const std::string& path) {
        auto source = OpenFile(fixtures + "/" + path);
        if (!source) throw std::runtime_error("missing fixture: " + path);
        return source;
    }
    json Inspect(const std::string& path) { return DefaultRegistry().Inspect(*File(path)); }
    // Refused by every dedicated provider: failed, or only described by the reader of last resort.
    static bool Refused(const json& r) { return !r.value("ok", false) || r.value("provider", json::object()).value("name", "") == "generic"; }
    std::vector<uint8_t> Bytes(ByteSourcePtr source) {
        return source->ReadSome(0, static_cast<size_t>(source->Size()));
    }
    ByteSourcePtr ChildOf(ByteSourcePtr source, const std::string& name) {
        std::string error; auto inner = DefaultRegistry().OpenChild(source, name, error);
        if (!inner) throw std::runtime_error("missing child " + name + ": " + error);
        return inner;
    }
    void Run(const char* name, const std::function<void()>& action) {
        try { action(); }
        catch (const std::exception& e) { ++failures; std::cerr << name << ": " << e.what() << '\n'; }
    }
};

void Archives(Tests& t) {
    const auto expected = t.Bytes(t.File("archive-next/expected.bin"));
    for (const char* name : {"ustar.tar", "v7.tar", "pax.tar", "gnu.tar"}) {
        auto file = t.File(std::string("archive-next/") + name);
        const auto result = DefaultRegistry().Inspect(*file);
        t.Check(result.value("ok", false) && result.value("format", "") == "tar", std::string("TAR detect ") + name);
        const auto list = DefaultRegistry().ListChildren(*file);
        t.Check(list.at("children").size() > 0, "TAR lists files");
        auto child = t.ChildOf(file, list.at("children").at(0).at("name"));
        t.Check(t.Bytes(child) == expected, std::string("TAR bytes ") + name);
        InspectOptions zero; zero.maxEntries = 0;
        t.Check(DefaultRegistry().Inspect(*file, zero).at("data").at("files").empty(), "TAR zero cap");
    }
    for (const char* name : {"unsafe.tar", "duplicate.tar"}) {
        auto file = t.File(std::string("archive-next/") + name);
        const auto list = DefaultRegistry().ListChildren(*file);
        for (const auto& child : list.at("children")) {
            t.Check(child.at("readable") == false, "TAR unsafe/duplicate/link is not readable");
            std::string error;
            t.Check(!DefaultRegistry().OpenChild(file, child.at("name"), error), "TAR unsafe open refused");
        }
    }
    t.Check(Tests::Refused(t.Inspect("archive-next/bad-checksum.tar")), "TAR checksum rejected");
    for (const char* name : {"plain.gz", "optional-header.gz"}) {
        auto file = t.File(std::string("archive-next/") + name);
        const auto report = DefaultRegistry().Inspect(*file);
        t.Check(report.value("format", "") == "gzip" && report.at("data").at("checksums_verified") == true, "gzip CRC/ISIZE checked");
        t.Check(t.Bytes(t.ChildOf(file, "payload")) == expected, "gzip exact bytes");
    }
    auto concat = t.File("archive-next/concatenated.gz");
    const auto both = t.Bytes(t.ChildOf(concat, "payload"));
    t.Check(std::string(both.begin(), both.end()) == "firstsecond", "gzip concatenation");
    t.Check(DefaultRegistry().Inspect(*concat).at("data").at("member_count") == 2, "gzip member count");
    t.Check(!t.Inspect("archive-next/bad-crc.gz").value("ok", false), "gzip corrupt trailer rejected");
    t.Check(!t.Inspect("archive-next/bad-header.gz").value("ok", false), "gzip corrupt optional header rejected");
    auto npm = t.ChildOf(t.File("archive-next/package.tgz"), "payload");
    const auto package = DefaultRegistry().Inspect(*npm);
    t.Check(package.value("format", "") == "npm-tar", "tgz -> npm tar");
    t.Check(package.at("data").at("npm").at("name") == "@cortex/owned-fixture", "npm name");
    t.Check(package.at("data").at("npm").at("scripts_executed") == false, "npm scripts are data only");
    t.Check(t.ChildOf(npm, "package/package.json")->Size() > 0, "npm manifest readable");
    for (const auto& pair : std::vector<std::pair<std::string, std::string>>{
        {"simple.a", "alpha.bin"}, {"bsd.a", "long-name-that-does-not-fit.bin"}, {"gnu.a", "very-long-gnu-filename.bin"}}) {
        auto ar = t.File("archive-next/" + pair.first);
        t.Check(DefaultRegistry().Inspect(*ar).value("format", "") == "ar", "ar detected");
        t.Check(t.Bytes(t.ChildOf(ar, pair.second)) == expected, "ar exact bytes: " + pair.first);
    }
    t.Check(!t.Inspect("archive-next/thin.a").value("ok", false), "thin ar refuses external reads");
    std::string error;
    t.Check(!DefaultRegistry().OpenChild(t.File("archive-next/unsafe.a"), "../escape", error), "ar traversal refused");
    auto deb = t.File("archive-next/sample.deb");
    t.Check(DefaultRegistry().Inspect(*deb).value("format", "") == "deb", "Debian container detected");
    auto inner = t.ChildOf(t.ChildOf(t.ChildOf(deb, "data.tar.gz"), "payload"), "usr/share/cortex/hello.txt");
    t.Check(t.Bytes(inner) == expected, "deb -> gzip -> tar -> file");
    // Decoder exposes the first stream boundary; trailing bytes belong to caller.
    const uint8_t input[] = {0x03, 0x00, 0xa5, 0x5a};
    std::vector<uint8_t> output; size_t consumed = 99;
    t.Check(InflatePrefix(input, sizeof(input), output, 10, consumed, error) && output.empty() && consumed == 2,
            "DEFLATE prefix boundary");
    consumed = 99;
    t.Check(!InflatePrefix(input, 1, output, 10, consumed, error) && output.empty() && consumed == 0, "DEFLATE failure resets");
}

void Certificates(Tests& t) {
    const auto der = t.File("certificates/certificate.der");
    const auto cert = DefaultRegistry().Inspect(*der);
    t.Check(cert.value("ok", false) && cert.value("format", "") == "x509", "DER certificate recognized");
    const auto& data = cert.at("data");
    t.Check(data.at("serial") == "2a" && data.at("version") == 3, "certificate version/serial from OpenSSL");
    t.Check(data.at("sha256") == Sha256Of(*der), "certificate DER fingerprint");
    t.Check(data.at("public_key_algorithm_oid") == "1.2.840.113549.1.1.1", "RSA SPKI OID");
    t.Check(data.at("signature_verified") == false && data.at("chain_verified") == false, "no invented signature/trust verdict");
    bool cn = false;
    for (const auto& n : data.at("subject")) cn = cn || (n.at("oid") == "2.5.4.3" && n.at("value") == "Cortex Test Certificate");
    t.Check(cn, "X509 subject CN");
    auto pem = t.File("certificates/bundle.pem");
    auto bundle = DefaultRegistry().Inspect(*pem);
    t.Check(bundle.value("format", "") == "pem-certificates" && bundle.at("data").at("certificate_count") == 2, "PEM bundle count");
    t.Check(t.Bytes(t.ChildOf(pem, "certificate-1.der")) == t.Bytes(der), "PEM decoded child matches DER");
    const auto payload = t.Bytes(t.File("certificates/content.bin"));
    for (const char* name : {"signed.p7m", "signed.pem", "catalog.cat"}) {
        auto source = t.File(std::string("certificates/") + name);
        const auto report = DefaultRegistry().Inspect(*source);
        const std::string kind = std::string(name) == "catalog.cat" ? "pkcs7-catalog" : "pkcs7";
        t.Check(report.value("format", "") == kind, "CMS type: " + std::string(name));
        const auto& cms = report.at("data");
        t.Check(cms.at("certificate_count") == 1 && cms.at("signer_count") == 1, "CMS certificates/signers");
        t.Check(cms.at("signature_verified") == false && cms.at("chain_verified") == false, "CMS trust remains unverified");
        t.Check(t.Bytes(t.ChildOf(source, "content.der")) == payload, "CMS exact encapsulated octets");
        t.Check(t.Bytes(t.ChildOf(source, "certificate-0.der")) == t.Bytes(der), "CMS certificate child");
    }
    const auto p7b = t.Inspect("certificates/certificates.p7b");
    t.Check(p7b.value("format", "") == "pkcs7" && p7b.at("data").at("certificate_count") == 1 && p7b.at("data").at("signer_count") == 0,
            "PKCS7 certificate-only bundle");
    for (const char* name : {"bad-base64.pem", "indefinite.der"})
        t.Check(Tests::Refused(t.Inspect(std::string("certificates/") + name)), "invalid certificate encoding refused");
    InspectOptions zero; zero.maxEntries = 0;
    const auto bounded = DefaultRegistry().Inspect(*pem, zero);
    t.Check(bounded.at("data").at("certificates").empty() && bounded.at("data").at("truncated") == true, "certificate listing bound");
    auto altered = t.Bytes(der); altered.back() ^= 0x40;
    const auto unverified = DefaultRegistry().Inspect(*FromBytes(altered, "altered.der"));
    t.Check(unverified.value("format", "") == "x509" && unverified.at("data").at("signature_verified") == false,
            "altered signature is not declared authentic");
}

void TypeLibraries(Tests& t) {
    for (const auto& item : std::vector<std::pair<std::string, unsigned>>{{"microsoft-win32.tlb", 1}, {"microsoft-win64.tlb", 3}}) {
        auto source = t.File("typelib/" + item.first);
        const auto report = DefaultRegistry().Inspect(*source);
        t.Check(report.value("format", "") == "tlb-msft", "MSFT type library detected");
        const auto& data = report.at("data");
        t.Check(data.at("library").at("name") == "CortexFixture" && data.at("library").at("version") == "1.2", "TLB identity");
        t.Check(data.at("library").at("guid") == "12345678-1122-3344-5566-778899aabbcc", "TLB LIBID");
        t.Check(data.at("library").at("syskind") == item.second, "TLB bitness");
        t.Check(data.at("type_count") == 2 && data.at("com_activation") == false, "TLB type count/no activation");
        const auto& iface = data.at("types").at(0);
        t.Check(iface.at("name") == "ICortex" && iface.at("kind") == "interface", "TLB interface");
        const auto& method = iface.at("functions").at(0);
        t.Check(method.at("name") == "Ping" && method.at("memid") == 7, "TLB method id/name");
        t.Check(method.at("type").at("vartype") == 25 && method.at("calling_convention") == 4, "TLB HRESULT/stdcall");
        t.Check(method.at("parameters").at(0).at("name") == "value" && method.at("parameters").at(0).at("type").at("vartype") == 3,
                "TLB parameter name/type");
        t.Check(data.at("types").at(1).at("variables").at(0).at("name") == "Answer", "TLB enum member");
        InspectOptions zero; zero.maxEntries = 0;
        t.Check(DefaultRegistry().Inspect(*source, zero).at("data").at("types").empty(), "TLB zero bound");
        auto bytes = t.Bytes(source); bytes[32] = 255; bytes[33] = 255;
        t.Check(!DefaultRegistry().Inspect(*FromBytes(bytes, "oversized.tlb")).value("ok", false), "TLB oversized table rejected");
    }
    t.Check(!DefaultRegistry().Inspect(*FromBytes({'S','L','T','G',0,0,0,0}, "legacy.tlb")).value("ok", false), "SLTG explicitly not supported");
}

void Packages(Tests& t) {
    for (const auto& entry : std::vector<std::pair<std::string, std::string>>{
        {"sample.xpi", "xpi"}, {"webextension.zip", "webextension"}, {"sample.vsix", "vsix"},
        {"sample.war", "war"}, {"sample.ear", "ear"}, {"sample.pyz", "python-zipapp"}, {"sample.whl", "wheel"}}) {
        const auto report = t.Inspect("package-next/" + entry.first);
        t.Check(report.value("ok", false) && report.value("format", "") == entry.second, "ZIP package recognition: " + entry.first);
        t.Check(report.at("data").contains("package_metadata"), "package metadata reported");
    }
    const auto extension = t.Inspect("package-next/sample.xpi").at("data").at("package_metadata");
    t.Check(extension.at("name") == "Cortex Extension Fixture" && extension.at("version") == "1.2.3", "WebExtension identity");
    t.Check(extension.at("permissions") == json::array({"storage", "tabs"}), "WebExtension permissions");
    t.Check(extension.at("background").at("service_worker") == "worker.js", "WebExtension background entry point as data");
    t.Check(extension.at("gecko").at("id") == "fixture@cortex.invalid", "Firefox metadata");
    t.Check(extension.at("signature_verified") == false && extension.at("scripts_executed") == false, "extension is not installed or authenticated");
    const auto vsix = t.Inspect("package-next/sample.vsix").at("data").at("package_metadata");
    t.Check(vsix.at("identity").at("publisher") == "Cortex" && vsix.at("identity").at("version") == "2.3.4", "VSIX identity");
    t.Check(vsix.at("vscode").at("main") == "./out/extension.js" && vsix.at("vscode").at("vscode_engine") == "^1.90.0", "VS Code entry point/engine");
    t.Check(vsix.at("installed") == false, "VSIX never installed");
    const auto wheel = t.Inspect("package-next/sample.whl").at("data").at("package_metadata");
    t.Check(wheel.at("metadata").at("Name") == "cortex-fixture" && wheel.at("requires_dist").size() == 2, "wheel metadata and dependencies");
    for (const char* file : {"invalid-extension.zip", "invalid.vsix"}) {
        const auto report = t.Inspect(std::string("package-next/") + file);
        t.Check(report.value("ok", false) && report.at("data").at("package_metadata").contains("error"), "bad manifest retained as explicit metadata error");
    }
    for (const auto& entry : std::vector<std::pair<std::string, std::string>>{{"sample2.crx", "crx2"}, {"sample3.crx", "crx3"}, {"container.jmod", "jmod"}, {"jdk.jmod", "jmod"}}) {
        auto file = t.File("package-next/" + entry.first);
        const auto report = DefaultRegistry().Inspect(*file);
        t.Check(report.value("ok", false) && report.value("format", "") == entry.second, "ZIP wrapper: " + entry.first);
        auto archive = t.ChildOf(file, "archive.zip");
        t.Check(DefaultRegistry().ListChildren(*archive).value("ok", false), "wrapped ZIP children");
        if (entry.second == "jmod") {
            auto descriptor = t.ChildOf(archive, "classes/module-info.class");
            const auto magic = descriptor->ReadSome(0, 4);
            t.Check(magic == std::vector<uint8_t>({0xca, 0xfe, 0xba, 0xbe}), "JMOD class bytes available without executing");
        } else {
            t.Check(report.at("data").at("signature_verified") == false, "CRX placeholder proof not verified");
            t.Check(DefaultRegistry().Inspect(*archive).at("data").at("package_metadata").at("name") == "Cortex Extension Fixture", "CRX -> manifest");
        }
        if (entry.second == "crx3") {
            t.Check(report.at("data").at("proof_count") == 1, "CRX3 protobuf proof count");
            t.Check(report.at("data").at("claimed_extension_id") == "aaabacadaeafagahaiajakalamanaoap", "CRX3 claimed id decoded");
        }
    }
    for (const char* file : {"oversized.crx", "duplicate-header.crx"})
        t.Check(!t.Inspect(std::string("package-next/") + file).value("ok", false), "malformed CRX rejected");
    auto releaseFile = t.File("package-next/RELEASES");
    const auto release = DefaultRegistry().Inspect(*releaseFile);
    t.Check(release.value("format", "") == "squirrel-releases" && release.at("data").at("entry_count") == 2, "Squirrel RELEASES entries");
    t.Check(release.at("data").at("packages").at(1).at("delta") == true, "Squirrel delta metadata");
    t.Check(release.at("data").at("network_requests") == false && release.at("data").at("installation") == false, "Squirrel references not downloaded");
    auto invalid = FromBytes(t.Bytes(t.File("package-next/invalid-RELEASES.txt")), "RELEASES");
    t.Check(Tests::Refused(DefaultRegistry().Inspect(*invalid)), "Squirrel traversal name rejected");
    InspectOptions zero; zero.maxEntries = 0;
    const auto zeroRelease = DefaultRegistry().Inspect(*releaseFile, zero);
    t.Check(zeroRelease.at("data").at("packages").empty() && zeroRelease.at("data").at("truncated") == true, "Squirrel output bounds");
    const auto zeroExtension = DefaultRegistry().Inspect(*t.File("package-next/sample.xpi"), zero);
    t.Check(zeroExtension.at("data").at("package_metadata").at("permissions").empty(), "extension output bounds");
}

void Corruptions(Tests& t) {
    for (const char* relative : {"archive-next/ustar.tar", "archive-next/plain.gz", "archive-next/gnu.a",
                                "archive-next/sample.deb", "certificates/certificate.der", "certificates/signed.p7m",
                                "certificates/bundle.pem", "typelib/microsoft-win32.tlb"}) {
        const auto original = t.Bytes(t.File(relative));
        for (size_t n = 0; n < original.size(); n += 61) {
            const auto prefix = FromBytes(std::vector<uint8_t>(original.begin(), original.begin() + n), relative);
            (void)DefaultRegistry().Inspect(*prefix).dump();
        }
        std::mt19937 rng(20261107);
        for (unsigned n = 0; n < 128; ++n) {
            auto bytes = original;
            for (unsigned k = 0; k < 3; ++k) bytes[rng() % bytes.size()] = static_cast<uint8_t>(rng());
            const auto damaged = FromBytes(std::move(bytes), relative);
            (void)DefaultRegistry().Inspect(*damaged).dump();
        }
        t.Check(true, std::string("bounded corruption sweep ") + relative);
    }
}
} // namespace
int main(int argc, char** argv) {
    Tests tests{argc > 1 ? argv[1] : "tests/fixtures"};
    tests.Run("archives", [&] { Archives(tests); });
    tests.Run("certificates", [&] { Certificates(tests); });
    tests.Run("typelibs", [&] { TypeLibraries(tests); });
    tests.Run("packages", [&] { Packages(tests); });
    tests.Run("corruptions", [&] { Corruptions(tests); });
    std::cout << "format-next checks=" << tests.checks << " failures=" << tests.failures << '\n';
    return tests.failures ? 1 : 0;
}
