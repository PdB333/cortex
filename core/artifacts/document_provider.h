#pragma once

// Documents that can carry active content, read offline: PDF (objects,
// pages, document info, JavaScript, launch/URI/submit actions, embedded
// files, forms, encryption; FlateDecode object streams are expanded with
// the built-in inflater) and Windows shell links (.lnk: header, target
// path, arguments, working directory, icon, volume, extra-data blocks).
// Scripts are listed, never run; encrypted PDF strings are not decrypted.

#include "artifact.h"

namespace cortex::artifacts {

class PdfProvider final : public Provider {
public:
    const char* Name() const override { return "pdf"; }
    const char* Version() const override { return "1"; }
    std::vector<std::string> Formats() const override { return {"pdf"}; }
    std::vector<Capability> Capabilities() const override;
    int Detect(const ByteSource& source) const override;
    json Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const override;
};

class LnkProvider final : public Provider {
public:
    const char* Name() const override { return "shell-link"; }
    const char* Version() const override { return "1"; }
    std::vector<std::string> Formats() const override { return {"lnk"}; }
    std::vector<Capability> Capabilities() const override;
    int Detect(const ByteSource& source) const override;
    json Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const override;
};

} // namespace cortex::artifacts
