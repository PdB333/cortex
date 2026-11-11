#pragma once
#include "artifact.h"

namespace cortex::artifacts {
// Structural certificate/CMS inventory, not a trust or signature verifier.
// DER definite lengths and PEM certificate/CMS blocks; no private keys,
// certificate-store registration, network lookup or decryption.
class CertificateProvider final : public Provider {
public:
    const char* Name() const override { return "certificate"; }
    const char* Version() const override { return "1"; }
    std::vector<std::string> Formats() const override { return {"x509", "pem-certificates", "pkcs7", "pkcs7-catalog"}; }
    std::vector<Capability> Capabilities() const override;
    int Detect(const ByteSource& source) const override;
    json Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const override;
    std::vector<Child> Children(const ByteSource& source, const InspectOptions& options, std::string& error) const override;
    ByteSourcePtr OpenChild(ByteSourcePtr source, const Child& child, std::string& error) const override;
};
} // namespace cortex::artifacts
