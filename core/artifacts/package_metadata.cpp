#include "package_metadata.h"
#include "bounded_reader.h"
#include "xml_reader.h"
#include "zip_provider.h"
#include "sha256.h"
#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>

namespace cortex::artifacts {
namespace {
using bounded::Error;
using bounded::View;
std::string Lower(std::string text) {
    for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}
bool Ends(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}
std::string Leaf(const std::string& name) {
    const auto pos = name.find_last_of("/\\!"); return pos == std::string::npos ? name : name.substr(pos + 1);
}
json ReadJson(const std::vector<uint8_t>& bytes) {
    size_t events = 0;
    const auto guard = [&](int depth, json::parse_event_t, json&) {
        if (depth > 64 || ++events > 65536) throw Error("package_json_depth_or_node_limit");
        return true;
    };
    json result = json::parse(bytes.begin(), bytes.end(), guard, false, true);
    if (!result.is_object()) throw Error("package_manifest_not_json_object");
    return result;
}
std::string String(const json& value, const char* key) {
    const auto it = value.find(key);
    if (it == value.end() || !it->is_string()) return "";
    const auto& text = it->get_ref<const std::string&>();
    if (text.size() > 4096) throw Error("package_string_limit");
    return text;
}
json Strings(const json& value, const char* key, size_t limit) {
    json result = json::array(); const auto it = value.find(key);
    if (it == value.end() || !it->is_array()) return result;
    for (const auto& item : *it) {
        if (result.size() >= limit) break;
        if (!item.is_string()) continue;
        const auto& text = item.get_ref<const std::string&>();
        if (text.size() > 4096) throw Error("package_string_limit");
        result.push_back(text);
    }
    return result;
}
class Package {
public:
    Package(const ByteSource& source, const std::vector<Child>& children, size_t maximum)
        : source_(source), limit_(std::min<size_t>(maximum, 4096)) {
        for (const auto& child : children) if (!files_.emplace(child.name, &child).second) duplicate_.insert(child.name);
    }
    bool Has(const std::string& name) const { return files_.count(name) != 0; }
    std::vector<uint8_t> Read(const std::string& name) const {
        const auto it = files_.find(name);
        if (it == files_.end()) throw Error("package_manifest_missing");
        if (duplicate_.count(name)) throw Error("package_manifest_duplicated");
        if (!it->second->readable || it->second->size > (1u << 20)) throw Error("package_manifest_unreadable_or_limit");
        const auto source = ByteSourcePtr(&source_, [](const ByteSource*) {});
        std::string error;
        auto inner = ZipProvider().OpenChild(source, *it->second, error);
        if (!inner) throw Error(error.empty() ? "package_manifest_read_failed" : error);
        return bounded::Read(*inner, 0, inner->Size(), 1u << 20);
    }
    json WebExtension() const {
        const auto manifest = ReadJson(Read("manifest.json"));
        if (!manifest.contains("manifest_version") || !manifest["manifest_version"].is_number_integer() ||
            (manifest["manifest_version"] != 2 && manifest["manifest_version"] != 3) ||
            String(manifest, "name").empty() || String(manifest, "version").empty()) throw Error("webextension_required_fields_missing");
        const bool xpi = Ends(Lower(Leaf(source_.Name())), ".xpi");
        json result = {{"format", xpi ? "xpi" : "webextension"}, {"name", String(manifest, "name")},
                       {"version", String(manifest, "version")}, {"manifest_version", manifest["manifest_version"]},
                       {"scripts_executed", false}, {"signature_verified", false}};
        for (const char* field : {"description", "default_locale", "homepage_url", "options_page", "devtools_page"})
            if (!String(manifest, field).empty()) result[field] = String(manifest, field);
        bool truncated = false;
        for (const char* field : {"permissions", "host_permissions", "optional_permissions", "optional_host_permissions"}) {
            result[field] = Strings(manifest, field, limit_);
            if (manifest.contains(field) && manifest[field].is_array()) truncated |= manifest[field].size() > limit_;
        }
        if (manifest.contains("background") && manifest["background"].is_object()) {
            const auto& background = manifest["background"];
            result["background"] = {{"scripts", Strings(background, "scripts", limit_)}, {"service_worker", String(background, "service_worker")},
                                     {"page", String(background, "page")}, {"type", String(background, "type")}};
            if (background.contains("scripts") && background["scripts"].is_array()) truncated |= background["scripts"].size() > limit_;
        }
        json content = json::array();
        if (manifest.contains("content_scripts") && manifest["content_scripts"].is_array()) {
            truncated |= manifest["content_scripts"].size() > limit_;
            for (const auto& script : manifest["content_scripts"]) {
                if (content.size() >= limit_) break;
                if (!script.is_object()) continue;
                content.push_back({{"matches", Strings(script, "matches", limit_)}, {"js", Strings(script, "js", limit_)},
                                   {"css", Strings(script, "css", limit_)}, {"run_at", String(script, "run_at")}});
            }
        }
        result["content_scripts"] = std::move(content);
        for (const char* key : {"browser_specific_settings", "applications"}) {
            if (!manifest.contains(key) || !manifest[key].is_object()) continue;
            const auto& settings = manifest[key];
            if (settings.contains("gecko") && settings["gecko"].is_object()) {
                result["gecko"] = {{"id", String(settings["gecko"], "id")}, {"strict_min_version", String(settings["gecko"], "strict_min_version")}};
            }
        }
        result["truncated"] = truncated;
        return result;
    }
    json Vsix() const {
        const xml::Document doc(Read("extension.vsixmanifest"));
        const auto& root = doc.nodes.at(0);
        constexpr const char* ns = "http://schemas.microsoft.com/developer/vsx-schema/2011";
        if (root.local != "PackageManifest" || root.ns != ns) throw Error("vsix_schema_unsupported");
        json result = {{"format", "vsix"}, {"schema_version", root.Attr("Version")}, {"installed", false}, {"signature_verified", false}};
        json assets = json::array(), dependencies = json::array(); size_t assetCount = 0, dependencyCount = 0;
        for (const auto& node : doc.nodes) {
            if (node.ns != ns) continue;
            if (node.local == "Identity") {
                if (result.contains("identity")) throw Error("vsix_duplicate_identity");
                result["identity"] = {{"id", node.Attr("Id")}, {"version", node.Attr("Version")},
                                      {"publisher", node.Attr("Publisher")}, {"language", node.Attr("Language")}};
            } else if (node.local == "DisplayName" || node.local == "Description") result[node.local] = node.text;
            else if (node.local == "Asset") {
                ++assetCount;
                if (assets.size() < limit_) assets.push_back({{"type", node.Attr("Type")}, {"path", node.Attr("Path")}});
            } else if (node.local == "Dependency") {
                ++dependencyCount;
                if (dependencies.size() < limit_) dependencies.push_back({{"id", node.Attr("Id")}, {"version", node.Attr("Version")}});
            }
        }
        if (!result.contains("identity") || result["identity"]["id"] == "") throw Error("vsix_identity_missing");
        result["assets"] = assets; result["dependencies"] = dependencies;
        result["truncated"] = assetCount > limit_ || dependencyCount > limit_;
        if (Has("extension/package.json")) {
            const auto package = ReadJson(Read("extension/package.json"));
            json vscode = json::object();
            for (const char* key : {"name", "version", "publisher", "main", "browser", "displayName", "description"})
                if (!String(package, key).empty()) vscode[key] = String(package, key);
            if (package.contains("engines") && package["engines"].is_object()) vscode["vscode_engine"] = String(package["engines"], "vscode");
            vscode["activation_events"] = Strings(package, "activationEvents", limit_);
            vscode["scripts_executed"] = false; result["vscode"] = std::move(vscode);
        }
        return result;
    }
    json JavaPackage(const std::string& path, const std::string& rootName, const std::string& kind) const {
        const xml::Document doc(Read(path));
        if (doc.nodes.at(0).local != rootName) throw Error("java_package_descriptor_wrong_root");
        const auto ns = doc.nodes.at(0).ns;
        if (ns != "https://jakarta.ee/xml/ns/jakartaee" && ns != "http://xmlns.jcp.org/xml/ns/javaee" &&
            ns != "http://java.sun.com/xml/ns/javaee" && !ns.empty()) throw Error("java_package_namespace_unsupported");
        json values = json::array(); size_t found = 0;
        for (const auto& n : doc.nodes) {
            if (n.ns != ns) continue;
            if (n.local == "display-name" || n.local == "servlet-name" || n.local == "servlet-class" ||
                n.local == "url-pattern" || n.local == "web-uri" || n.local == "context-root" || n.local == "ejb" || n.local == "java") {
                ++found; if (values.size() < limit_) values.push_back({{"kind", n.local}, {"value", n.text}});
            }
        }
        return {{"format", kind}, {"descriptor", path}, {"namespace", ns}, {"values", values},
                {"truncated", found > limit_}, {"classes_executed", false}};
    }
    std::string WheelMetadata() const {
        for (const auto& [name, child] : files_) if (Ends(name, ".dist-info/METADATA")) return name;
        return "";
    }
    json Wheel(const std::string& path) const {
        const auto bytes = Read(path);
        const std::string text = bounded::Utf8(bytes.data(), bytes.size());
        std::istringstream input(text); std::string line, previousKey;
        json fields = json::object(), requirements = json::array(); size_t count = 0;
        bool retainedRequirement = false;
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) break;
            if (line.size() > 8192) throw Error("wheel_metadata_line_limit");
            if (line.front() == ' ' || line.front() == '\t') {
                // Folded continuation is preserved explicitly, not silently
                // applied to the wrong header. Full email parsing is out of scope.
                if (previousKey == "Requires-Dist" && retainedRequirement) requirements.back().get_ref<std::string&>() += " " + line;
                continue;
            }
            const auto colon = line.find(':'); if (colon == std::string::npos) continue;
            previousKey = line.substr(0, colon); size_t start = colon + 1;
            while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) ++start;
            retainedRequirement = previousKey == "Requires-Dist" && requirements.size() < limit_;
            if (previousKey == "Requires-Dist") { ++count; if (retainedRequirement) requirements.push_back(line.substr(start)); }
            else if (previousKey == "Name" || previousKey == "Version" || previousKey == "Requires-Python" || previousKey == "Summary" || previousKey == "Metadata-Version") fields[previousKey] = line.substr(start);
        }
        return {{"format", "wheel"}, {"metadata", fields}, {"requires_dist", requirements}, {"truncated", count > limit_}, {"code_executed", false}};
    }
private:
    const ByteSource& source_; size_t limit_;
    std::map<std::string, const Child*> files_; std::set<std::string> duplicate_;
};

struct ProtoField { uint32_t number; uint8_t wire; View data; uint64_t integer = 0; };
uint64_t Varint(const View& view, size_t& at) {
    uint64_t result = 0;
    for (unsigned i = 0; i < 10; ++i) {
        const uint8_t byte = view.U8(at++);
        if (i == 9 && byte > 1) throw Error("protobuf_varint_overflow");
        result |= uint64_t(byte & 127) << (i * 7);
        if (!(byte & 128)) return result;
    }
    throw Error("protobuf_varint_truncated");
}
std::vector<ProtoField> Fields(const View& view) {
    std::vector<ProtoField> result; size_t at = 0;
    while (at < view.Size()) {
        if (result.size() >= 4096) throw Error("crx_proto_field_limit");
        const uint64_t tag = Varint(view, at); const uint64_t number = tag >> 3;
        if (!number || number > 0x1fffffff) throw Error("protobuf_field_number_invalid");
        ProtoField f{static_cast<uint32_t>(number), static_cast<uint8_t>(tag & 7), View(), 0};
        uint64_t n = 0;
        if (f.wire == 0) f.integer = Varint(view, at);
        else if (f.wire == 1 || f.wire == 5 || f.wire == 2) {
            n = f.wire == 1 ? 8 : f.wire == 5 ? 4 : Varint(view, at);
            f.data = view.Slice(at, n); at += static_cast<size_t>(n);
        } else throw Error("protobuf_groups_or_wire_type_unsupported");
        result.push_back(f);
    }
    return result;
}
std::string Hash(const View& v) { Sha256 sha; sha.Update(v.Data(0, v.Size()), v.Size()); return Sha256::Hex(sha.Final()); }
struct Wrapper { uint64_t offset; json data; };
Wrapper Wrap(const ByteSource& source, size_t limit) {
    const auto head = bounded::Read(source, 0, std::min<uint64_t>(source.Size(), 16), 16); const View v(head);
    if (v.Has(0, 4) && v.U8(0) == 'J' && v.U8(1) == 'M') {
        if (v.U8(2) != 1 || v.U8(3) != 0) throw Error("jmod_version_unsupported");
        return {4, {{"format", "jmod"}, {"version", "1.0"}, {"classes_executed", false}}};
    }
    if (v.U32(0) != 0x34327243) throw Error("crx_signature_invalid");
    const uint32_t version = v.U32(4); uint64_t offset = 0;
    json result = {{"format", "crx" + std::to_string(version)}, {"version", version}, {"signature_verified", false}, {"extension_installed", false}};
    if (version == 2) {
        const uint32_t key = v.U32(8), signature = v.U32(12);
        if (uint64_t(key) + signature > (1u << 20)) throw Error("crx2_header_limit");
        offset = 16ull + key + signature;
        const auto bytes = bounded::Read(source, 16, uint64_t(key) + signature, 1u << 20);
        result["public_key_bytes"] = key; result["signature_bytes"] = signature;
        result["public_key_sha256"] = Hash(View(bytes).Slice(0, key));
    } else if (version == 3) {
        const uint32_t n = v.U32(8); if (n > (1u << 20)) throw Error("crx3_header_limit");
        offset = 12ull + n;
        const auto bytes = bounded::Read(source, 12, n, 1u << 20);
        json proofs = json::array(); size_t count = 0; bool signedHeader = false;
        for (const auto& f : Fields(View(bytes))) {
            if ((f.number == 2 || f.number == 3 || f.number == 4 || f.number == 10000) && f.wire != 2) throw Error("crx3_known_field_wire_mismatch");
            if (f.number == 2 || f.number == 3) {
                json proof = {{"algorithm", f.number == 2 ? "rsa" : "ecdsa"}, {"verified", false}};
                std::set<uint32_t> seen;
                for (const auto& p : Fields(f.data)) {
                    if (p.number != 1 && p.number != 2) continue;
                    if (p.wire != 2 || !seen.insert(p.number).second) throw Error("crx3_duplicate_or_invalid_proof_field");
                    if (p.number == 1) { proof["public_key_bytes"] = p.data.Size(); proof["public_key_sha256"] = Hash(p.data); }
                    else proof["signature_bytes"] = p.data.Size();
                }
                ++count; if (proofs.size() < limit) proofs.push_back(std::move(proof));
            } else if (f.number == 10000) {
                if (signedHeader) throw Error("crx3_duplicate_signed_header"); signedHeader = true;
                bool id = false;
                for (const auto& p : Fields(f.data)) if (p.number == 1) {
                    if (id || p.wire != 2 || p.data.Size() != 16) throw Error("crx3_id_invalid"); id = true;
                    std::string extension;
                    for (size_t i = 0; i < 16; ++i) { extension += char('a' + (p.data.U8(i) >> 4)); extension += char('a' + (p.data.U8(i) & 15)); }
                    result["claimed_extension_id"] = extension;
                }
            } else if (f.number == 4) result["verified_contents_bytes_unchecked"] = f.data.Size();
        }
        result["proof_count"] = count; result["proofs"] = proofs; result["truncated"] = count > limit;
        result["signed_header_present"] = signedHeader;
    } else throw Error("crx_version_unsupported");
    if (offset >= source.Size()) throw Error("wrapped_zip_payload_missing");
    return {offset, std::move(result)};
}
ByteSourcePtr ZipChild(ByteSourcePtr source, const Wrapper& wrapper) {
    if (wrapper.offset >= source->Size()) throw Error("wrapped_zip_payload_missing");
    auto inner = Slice(source, wrapper.offset, source->Size() - wrapper.offset, source->Name() + "!archive.zip");
    if (!inner || !ZipProvider().Detect(*inner)) throw Error("wrapped_zip_invalid_archive");
    return inner;
}

json Releases(const ByteSource& source, size_t limit) {
    const auto bytes = bounded::Read(source, 0, source.Size(), 1u << 20);
    std::string text = bounded::Utf8(bytes.data(), bytes.size());
    if (text.size() >= 3 && text.compare(0, 3, "\xef\xbb\xbf") == 0) text.erase(0, 3);
    std::istringstream input(text); std::string line; json rows = json::array(); size_t count = 0;
    while (std::getline(input, line)) {
        if (line.size() > 8192) throw Error("squirrel_line_limit");
        const auto comment = line.find('#'); const std::string note = comment == std::string::npos ? "" : line.substr(comment + 1);
        if (comment != std::string::npos) line.resize(comment);
        std::istringstream fields(line); std::string hash, file, size, extra;
        if (!(fields >> hash)) continue;
        if (!(fields >> file >> size) || (fields >> extra) || hash.size() != 40 || hash.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
            throw Error("squirrel_release_line_invalid");
        if (size.empty() || size.size() > 19 || size.find_first_not_of("0123456789") != std::string::npos) throw Error("squirrel_release_size_invalid");
        uint64_t length = 0;
        for (char c : size) { if (length > (uint64_t(INT64_MAX) - (c - '0')) / 10) throw Error("squirrel_release_size_overflow"); length = length * 10 + c - '0'; }
        if (!length) throw Error("squirrel_release_size_zero");
        const bool url = file.rfind("https://", 0) == 0 || file.rfind("http://", 0) == 0;
        if (!url && (file.find_first_of("/\\:*?\"<>|") != std::string::npos || file == "." || file == "..")) throw Error("squirrel_unsafe_package_name");
        std::string leaf = Leaf(file.substr(0, file.find('?')));
        if (!Ends(Lower(leaf), ".nupkg")) throw Error("squirrel_package_extension_invalid");
        ++count; if (count > 16384) throw Error("squirrel_release_count_limit");
        if (rows.size() >= limit) continue;
        json row = {{"sha1_claimed", Lower(hash)}, {"reference", file}, {"filename", leaf}, {"size_claimed", length},
                    {"delta", Ends(Lower(leaf), "-delta.nupkg")}, {"external_url", url}, {"downloaded", false}, {"hash_verified", false}};
        if (!note.empty()) row["comment"] = note;
        rows.push_back(std::move(row));
    }
    if (!count) throw Error("squirrel_no_release_entries");
    return {{"format", "squirrel-releases"}, {"entry_count", count}, {"packages", rows}, {"truncated", count > limit},
            {"network_requests", false}, {"installation", false}, {"delta_patch_applied", false}};
}
} // namespace

json InspectZipPackage(const ByteSource& source, const std::vector<Child>& children, const InspectOptions& options) {
    try {
        Package p(source, children, options.maxEntries);
        if (p.Has("extension.vsixmanifest")) return p.Vsix();
        if (p.Has("manifest.json")) return p.WebExtension();
        if (p.Has("WEB-INF/web.xml")) return p.JavaPackage("WEB-INF/web.xml", "web-app", "war");
        if (p.Has("META-INF/application.xml")) return p.JavaPackage("META-INF/application.xml", "application", "ear");
        if (const auto meta = p.WheelMetadata(); !meta.empty()) return p.Wheel(meta);
        if (p.Has("__main__.py") && Ends(Lower(source.Name()), ".pyz"))
            return {{"format", "python-zipapp"}, {"entry_point", "__main__.py"}, {"code_executed", false}};
        return json::object();
    } catch (const Error& e) { return {{"error", e.what()}, {"metadata_validated", false}}; }
}
std::vector<Capability> WrappedZipProvider::Capabilities() const {
    return {{"static", CapabilityStatus::Experimental, "cortex_format_next_tests", "CRX2/3 bounded header/proof inventory and JMOD1.0 container; ZIP manifest metadata, no cryptographic signature verification"},
            {"decompile", CapabilityStatus::Unavailable, "", "nested providers handle file analysis"},
            {"live", CapabilityStatus::Unavailable, "", "extensions/modules are never installed or executed"}};
}
int WrappedZipProvider::Detect(const ByteSource& source) const {
    uint8_t h[4]{}; if (!source.Read(0, h, 4)) return 0;
    return (h[0] == 'C' && h[1] == 'r' && h[2] == '2' && h[3] == '4') || (h[0] == 'J' && h[1] == 'M') ? 70 : 0;
}
json WrappedZipProvider::Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const {
    error.clear(); try {
        auto wrapper = Wrap(source, options.maxEntries);
        auto inner = ZipChild(ByteSourcePtr(&source, [](const ByteSource*) {}), wrapper);
        auto archive = ZipProvider().Inspect(*inner, options, error);
        if (archive.is_null()) return nullptr;
        wrapper.data["archive"] = archive; wrapper.data["archive_offset"] = wrapper.offset;
        wrapper.data["archive_child"] = "archive.zip";
        return wrapper.data;
    } catch (const Error& e) { error = e.what(); return nullptr; }
}
std::vector<Child> WrappedZipProvider::Children(const ByteSource& source, const InspectOptions& options, std::string& error) const {
    error.clear(); try {
        const auto wrapper = Wrap(source, 0);
        auto inner = ZipChild(ByteSourcePtr(&source, [](const ByteSource*) {}), wrapper);
        if (!options.maxEntries) return {};
        Child child; child.name = "archive.zip"; child.offset = wrapper.offset; child.size = inner->Size(); return {child};
    } catch (const Error& e) { error = e.what(); return {}; }
}
ByteSourcePtr WrappedZipProvider::OpenChild(ByteSourcePtr source, const Child& child, std::string& error) const {
    error.clear(); try {
        if (!source || !child.readable || child.name != "archive.zip") throw Error("wrapped_zip_unknown_child");
        return ZipChild(source, Wrap(*source, 0));
    } catch (const Error& e) { error = e.what(); return {}; }
}
std::vector<Capability> SquirrelReleasesProvider::Capabilities() const {
    return {{"static", CapabilityStatus::Experimental, "cortex_format_next_tests", "Squirrel RELEASES SHA1/filename/size inventory; references never fetched, hashes not independently verified"},
            {"decompile", CapabilityStatus::Unavailable, "", "manifest only"}, {"live", CapabilityStatus::Unavailable, "", "no update applied"}};
}
int SquirrelReleasesProvider::Detect(const ByteSource& source) const {
    if (Lower(Leaf(source.Name())) != "releases" || source.Size() > (1u << 20)) return 0;
    try { (void)Releases(source, 0); return 55; } catch (const Error&) { return 0; }
}
json SquirrelReleasesProvider::Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const {
    error.clear(); try { return Releases(source, std::min<size_t>(options.maxEntries, 4096)); }
    catch (const Error& e) { error = e.what(); return nullptr; }
}
} // namespace cortex::artifacts
