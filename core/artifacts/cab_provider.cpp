#include "cab_provider.h"
#include "bounded_reader.h"
#include "inflate.h"

#include <algorithm>
#include <cstring>
#include <map>

// Microsoft [MS-CAB], CFHEADER/CFFOLDER/CFFILE/CFDATA. Compression is not
// execution: only stored and RFC1951 MSZIP data are decoded, in memory.
namespace cortex::artifacts {
namespace {
using bounded::View;
using bounded::Error;
constexpr uint64_t kInputLimit = 64ull << 20;
constexpr uint64_t kExpandedLimit = 64ull << 20;
constexpr size_t kBlockLimit = 65536;

uint32_t Checksum(const View& bytes, uint32_t sum = 0) {
    uint64_t at = 0;
    while (bytes.Has(at, 4)) { sum ^= bytes.U32(at); at += 4; }
    // The remainder is folded from the last byte backwards (MS-CAB 3.1).
    uint32_t tail = 0;
    const size_t left = bytes.Size() - static_cast<size_t>(at);
    for (size_t i = 0; i < left; ++i) tail |= uint32_t(bytes.U8(at + i)) << (8 * (left - 1 - i));
    return sum ^ tail;
}

bool Unsafe(const std::string& name) {
    if (name.empty() || name[0] == '/' || name.find(':') != std::string::npos) return true;
    size_t start = 0;
    while (start <= name.size()) {
        const size_t end = name.find('/', start);
        const std::string part = name.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (part == ".." || part == "." || part.empty()) return true;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return false;
}
const char* Compression(uint16_t type) {
    switch (type & 15) { case 0: return "stored"; case 1: return "mszip"; case 2: return "quantum"; case 3: return "lzx"; default: return "unknown"; }
}
struct Block { uint32_t offset = 0, size = 0, expanded = 0; bool checked = false; };
struct Folder {
    uint32_t offset = 0;
    uint16_t count = 0, compression = 0;
    uint64_t expanded = 0, stored = 0;
    std::vector<Block> blocks;
    std::string error;
};
struct File {
    std::string name, error;
    uint32_t size = 0, folderOffset = 0;
    uint16_t folder = 0, date = 0, time = 0, attributes = 0;
};

class Cabinet {
public:
    explicit Cabinet(const ByteSource& source) : bytes_(bounded::Read(source, 0, source.Size(), kInputLimit)), data_(bytes_) {
        if (!data_.Has(0, 36) || std::memcmp(data_.Data(0,4), "MSCF", 4)) throw Error("not_cab");
        cabinetSize = data_.U32(8);
        if (cabinetSize < 36 || cabinetSize > bytes_.size()) throw Error("cab_size_invalid");
        data_ = data_.Slice(0, cabinetSize);
        if (data_.U8(24) != 3 || data_.U8(25) != 1) throw Error("cab_version_unsupported");
        const uint32_t fileOffset = data_.U32(16);
        const uint16_t folderCount = data_.U16(26), fileCount = data_.U16(28);
        flags = data_.U16(30); setId = data_.U16(32); cabinetIndex = data_.U16(34);
        if (flags & ~7u) throw Error("cab_flags_unsupported");
        uint64_t at = 36;
        uint8_t folderReserve = 0;
        if (flags & 4) {
            const uint16_t reserve = data_.U16(at);
            folderReserve = data_.U8(at + 2); dataReserve_ = data_.U8(at + 3);
            at += 4; data_.Data(at, reserve); at += reserve;
        }
        auto str = [&]() {
            size_t next = 0; const std::string value = data_.CString(at, &next);
            at = next; return value;
        };
        if (flags & 1) { previous = str(); previousDisk = str(); }
        if (flags & 2) { next = str(); nextDisk = str(); }
        if (!data_.Has(at, uint64_t(folderCount) * (8 + folderReserve))) throw Error("cab_folders_truncated");
        for (uint32_t i = 0; i < folderCount; ++i) {
            Folder folder;
            folder.offset = data_.U32(at); folder.count = data_.U16(at + 4); folder.compression = data_.U16(at + 6);
            if ((folder.compression & 15) > 1) folder.error = "unsupported_compression";
            if ((folder.compression & 15) <= 1 && (folder.compression & 0xFFF0)) folder.error = "compression_parameters_unsupported";
            folders.push_back(std::move(folder)); at += 8 + folderReserve;
        }
        if (fileOffset < at || fileOffset > cabinetSize) throw Error("cab_file_table_offset_invalid");
        at = fileOffset;
        std::map<std::string, unsigned> names;
        for (uint32_t i = 0; i < fileCount; ++i) {
            File file;
            file.size = data_.U32(at); file.folderOffset = data_.U32(at + 4); file.folder = data_.U16(at + 8);
            file.date = data_.U16(at + 10); file.time = data_.U16(at + 12); file.attributes = data_.U16(at + 14);
            at += 16; file.name = str();
            std::replace(file.name.begin(), file.name.end(), '\\', '/');
            if (Unsafe(file.name)) file.error = "unsafe_entry_name";
            if (file.folder >= 0xFFFD) file.error = "continued_file_not_supported";
            else if (file.folder >= folders.size()) file.error = "folder_index_invalid";
            if (file.size > kExpandedLimit) file.error = "entry_size_limit";
            ++names[file.name]; files.push_back(std::move(file));
        }
        const uint64_t metadataEnd = at;
        size_t blockCount = 0;
        uint64_t checksumBudget = 128ull << 20;
        for (auto& folder : folders) {
            try {
                if (folder.offset < metadataEnd) throw Error("cab_blocks_overlap_metadata");
                uint64_t pos = folder.offset;
                for (uint32_t i = 0; i < folder.count; ++i) {
                    if (++blockCount > kBlockLimit) throw Error("cab_block_count_limit");
                    const uint32_t checksum = data_.U32(pos);
                    const uint16_t size = data_.U16(pos + 4), expanded = data_.U16(pos + 6);
                    const uint64_t start = pos + 8 + dataReserve_;
                    const View payload = data_.Slice(start, size);
                    if (expanded == 0) throw Error("cab_split_block_not_supported");
                    if (expanded > 32768) throw Error("cab_block_output_invalid");
                    if (dataReserve_) throw Error("cab_data_reserve_not_supported");
                    if (size > checksumBudget) throw Error("cab_checksum_work_limit");
                    checksumBudget -= size;
                    if (checksum && Checksum(payload, Checksum(data_.Slice(pos + 4, 4))) != checksum)
                        throw Error("cab_checksum_mismatch");
                    if ((folder.compression & 15) == 0 && size != expanded) throw Error("cab_stored_size_mismatch");
                    if ((folder.compression & 15) == 1 && (size < 2 || payload.U16(0) != 0x4B43))
                        throw Error("cab_mszip_magic_invalid");
                    folder.expanded += expanded; folder.stored += size;
                    if (folder.expanded > kExpandedLimit) throw Error("cab_folder_expansion_limit");
                    folder.blocks.push_back({static_cast<uint32_t>(start), size, expanded, checksum != 0});
                    pos = start + size;
                }
            } catch (const Error& ex) { folder.error = ex.what(); }
        }
        for (auto& file : files) {
            if (names[file.name] != 1) file.error = "duplicate_entry_name";
            if (!file.error.empty()) continue;
            const auto& folder = folders[file.folder];
            if (!folder.error.empty()) file.error = folder.error;
            else if (!bounded::Range(folder.expanded, file.folderOffset, file.size)) file.error = "file_outside_folder";
        }
    }
    std::vector<uint8_t> ReadFile(size_t index) const {
        if (index >= files.size()) throw Error("cab_file_index_invalid");
        const auto& file = files[index];
        if (!file.error.empty()) throw Error(file.error);
        const auto& folder = folders[file.folder];
        std::vector<uint8_t> expanded;
        expanded.reserve(static_cast<size_t>(folder.expanded));
        for (const auto& block : folder.blocks) {
            const View payload = data_.Slice(block.offset, block.size);
            std::vector<uint8_t> output;
            if ((folder.compression & 15) == 0) {
                output.assign(payload.Data(0, block.size), payload.Data(0, block.size) + block.size);
            } else {
                std::string error;
                if (!InflateWithDictionary(payload.Data(2, block.size - 2), block.size - 2,
                                           expanded.data(), expanded.size(), output, block.expanded, error))
                    throw Error("cab_mszip:" + error);
                if (output.size() != block.expanded) throw Error("cab_block_size_mismatch");
            }
            if (output.size() > kExpandedLimit - expanded.size()) throw Error("cab_folder_expansion_limit");
            expanded.insert(expanded.end(), output.begin(), output.end());
        }
        if (!bounded::Range(expanded.size(), file.folderOffset, file.size)) throw Error("cab_file_range_invalid");
        return std::vector<uint8_t>(expanded.begin() + file.folderOffset, expanded.begin() + file.folderOffset + file.size);
    }
    Child ChildAt(size_t index) const {
        const auto& file = files[index]; Child child;
        child.name = file.name; child.offset = index; child.size = file.size;
        child.readable = file.error.empty(); child.note = file.error;
        if (file.folder < folders.size()) child.compression = std::string("cab/") + Compression(folders[file.folder].compression);
        return child;
    }
    uint32_t cabinetSize = 0;
    uint16_t flags = 0, setId = 0, cabinetIndex = 0;
    std::string previous, previousDisk, next, nextDisk;
    std::vector<File> files;
    std::vector<Folder> folders;
private:
    std::vector<uint8_t> bytes_;
    View data_;
    uint8_t dataReserve_ = 0;
};
} // namespace

std::vector<Capability> CabProvider::Capabilities() const {
    return {{"static", CapabilityStatus::Experimental, "tests/fixtures/cab; cortex_artifacts_tests",
             "CAB file/folder inventory, CFDATA checksum validation, stored/MSZIP including history between blocks; 64 MiB input/folder limits. Quantum/LZX/spanned files listed, not expanded."},
            {"decompile", CapabilityStatus::Unavailable, "", "nested entries use their own file providers"},
            {"live", CapabilityStatus::Unavailable, "", "archive is never installed or executed"}};
}
int CabProvider::Detect(const ByteSource& source) const {
    uint8_t magic[4]; return source.Read(0, magic, 4) && !std::memcmp(magic,"MSCF",4) ? 80 : 0;
}
json CabProvider::Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const {
    try {
        const Cabinet cab(source);
        const size_t limit = std::min<size_t>(options.maxEntries, 65536);
        json data = {{"format","cab"},{"version","1.3"},{"cabinet_bytes",cab.cabinetSize},
                     {"set_id",cab.setId},{"cabinet_index",cab.cabinetIndex},{"flags",cab.flags},
                     {"file_count",cab.files.size()},{"folder_count",cab.folders.size()},
                     {"files",json::array()},{"folders",json::array()},
                     {"truncated",cab.files.size()>limit || cab.folders.size()>limit}};
        if (cab.flags & 1) data["previous"] = {{"cabinet",cab.previous},{"disk",cab.previousDisk}};
        if (cab.flags & 2) data["next"] = {{"cabinet",cab.next},{"disk",cab.nextDisk}};
        for (size_t i=0;i<cab.folders.size() && i<limit;++i) {
            const auto& f=cab.folders[i]; size_t checked=0;
            for(const auto& b:f.blocks) checked+=b.checked;
            json row={{"index",i},{"compression",Compression(f.compression)},{"blocks",f.count},
                      {"stored_bytes",f.stored},{"expanded_bytes",f.expanded},{"checksummed_blocks",checked}};
            if(!f.error.empty()) row["error"]=f.error;
            data["folders"].push_back(std::move(row));
        }
        for(size_t i=0;i<cab.files.size() && i<limit;++i) {
            const auto& f=cab.files[i];
            json row={{"index",i},{"name",f.name},{"size",f.size},{"folder",f.folder},
                      {"folder_offset",f.folderOffset},{"attributes",f.attributes},{"dos_date",f.date},
                      {"dos_time",f.time},{"readable",f.error.empty()}};
            if(!f.error.empty()) row["error"]=f.error;
            data["files"].push_back(std::move(row));
        }
        return data;
    } catch(const Error& ex) { error=ex.what(); return nullptr; }
}
std::vector<Child> CabProvider::Children(const ByteSource& source,const InspectOptions& options,std::string& error) const {
    try {
        const Cabinet cab(source); std::vector<Child> children;
        const size_t limit=std::min<size_t>(options.maxEntries,65536);
        for(size_t i=0;i<cab.files.size() && i<limit;++i) children.push_back(cab.ChildAt(i));
        return children;
    } catch(const Error& ex) {error=ex.what();return {};}
}
ByteSourcePtr CabProvider::OpenChild(ByteSourcePtr container,const Child& child,std::string& error) const {
    try {
        if(!container) throw Error("no_container");
        const Cabinet cab(*container);
        if(child.offset>=cab.files.size() || cab.files[child.offset].name!=child.name) throw Error("cab_child_identity_invalid");
        return FromBytes(cab.ReadFile(static_cast<size_t>(child.offset)),container->Name()+"!"+child.name);
    } catch(const Error& ex) {error=ex.what();return {};}
}
} // namespace cortex::artifacts
