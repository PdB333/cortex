#pragma once

// The reader of last resort: any file that no dedicated provider claims is
// still described as far as its bytes allow, whatever its extension.
//
//   - signature identification (magic numbers at fixed offsets, ~150 formats
//     of images, audio/video, fonts, disk images, file systems, firmware,
//     Android, game engines, documents, databases, keys and containers);
//   - what the extension usually denotes, when no signature matches;
//   - text or binary: encoding (BOM, UTF-8, UTF-16), line endings, and for
//     text the likely language (JSON, XML/HTML, script by shebang, INI, CSV,
//     YAML, registry export, Markdown) with a bounded preview;
//   - entropy overall and per region, to spot compressed or encrypted parts;
//   - printable strings (ASCII and UTF-16LE), with URLs, IP addresses, e-mail
//     addresses, paths, registry keys and GUIDs pulled out;
//   - files embedded at an offset (an executable, archive, image, database...)
//     confirmed by the dedicated provider, listed as children that open with
//     that provider.
//
// Nothing is decoded beyond these bounded reads, run or decrypted.

#include "artifact.h"

namespace cortex::artifacts {

class GenericProvider final : public Provider {
public:
    const char* Name() const override { return "generic"; }
    const char* Version() const override { return "1"; }
    std::vector<std::string> Formats() const override { return {"unknown"}; }
    std::vector<Capability> Capabilities() const override;
    // Claims every file with the lowest score: dedicated providers win.
    int Detect(const ByteSource& source) const override;
    json Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const override;
    std::vector<Child> Children(const ByteSource& source, const InspectOptions& options, std::string& error) const override;
};

} // namespace cortex::artifacts
