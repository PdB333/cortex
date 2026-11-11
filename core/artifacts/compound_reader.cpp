#include "compound_reader.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <set>

// Microsoft MS-CFB: header, allocation tables and directory entry layouts.
// https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-cfb/
namespace cortex::artifacts::compound {
namespace {
using bounded::Error;
using bounded::View;
constexpr uint32_t kFree = 0xFFFFFFFFu, kEnd = 0xFFFFFFFEu, kFat = 0xFFFFFFFDu, kDifat = 0xFFFFFFFCu;
constexpr uint64_t kLimit = 64ull << 20;
constexpr uint32_t kEntries = 65536;

std::pair<std::string, bool> DecodeName(const View& name) {
    // MSI packs two characters from this 64-character alphabet into one
    // UTF-16 code unit; 0x4840 at the start denotes a table stream.
    static const char alphabet[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz._";
    bool table = name.Size() >= 2 && name.U16(0) == 0x4840;
    std::string out;
    for (size_t at = table ? 2 : 0; at < name.Size(); at += 2) {
        const uint16_t c = name.U16(at);
        if (c >= 0x3800 && c < 0x4800) {
            const unsigned packed = c - 0x3800;
            out += alphabet[packed & 63]; out += alphabet[packed >> 6];
        } else if (c >= 0x4800 && c < 0x4840) out += alphabet[c - 0x4800];
        else {
            size_t count = 2;
            if (c >= 0xD800 && c <= 0xDBFF && name.Has(at + 2, 2)) count = 4;
            out += bounded::Utf16(name.Slice(at, count)); at += count - 2;
        }
    }
    return {out, table};
}
bool SafeComponent(const std::string& text) {
    return !text.empty() && text != "." && text != ".." && text.find_first_of("/\\:") == std::string::npos &&
           text.find('\0') == std::string::npos;
}
} // namespace

bounded::View File::Sector(uint32_t id) const {
    if (id >= sectors_) throw Error("cfb_sector_out_of_range");
    return View(bytes_).Slice((uint64_t(id) + 1) * sectorSize, sectorSize);
}

std::vector<uint8_t> File::Chain(uint32_t first, const std::vector<uint32_t>& fat,
                                uint64_t expected, bool exact, bool mini) const {
    const uint64_t limit = exact ? kLimit : (8ull << 20);
    const uint32_t step = mini ? 64 : sectorSize;
    if (expected > limit) throw Error("cfb_stream_size_limit");
    if (exact && expected == 0) return {};
    const uint64_t required = (expected + step - 1) / step;
    std::vector<uint8_t> out;
    std::vector<bool> seen(fat.size(), false);
    uint32_t id = first;
    uint64_t count = 0;
    while (id != kEnd) {
        if (id >= fat.size() || seen[id]) throw Error("cfb_chain_cycle_or_index_invalid");
        seen[id] = true;
        if (++count > (limit + step - 1) / step || (exact && count > required)) throw Error("cfb_chain_length_invalid");
        View sector;
        if (mini) sector = View(miniStream_).Slice(uint64_t(id) * 64, 64);
        else {
            if (id >= reserved_.size() || reserved_[id]) throw Error("cfb_chain_uses_reserved_sector");
            sector = Sector(id);
        }
        const size_t take = exact ? static_cast<size_t>(std::min<uint64_t>(step, expected - out.size())) : step;
        const auto* begin = sector.Data(0, take);
        out.insert(out.end(), begin, begin + take);
        id = fat[id];
    }
    if (exact && out.size() != expected) throw Error("cfb_chain_truncated");
    return out;
}

File::File(const ByteSource& source) : bytes_(bounded::Read(source, 0, source.Size(), kLimit)) {
    const View h(bytes_);
    static const uint8_t magic[] = {0xD0,0xCF,0x11,0xE0,0xA1,0xB1,0x1A,0xE1};
    if (!h.Has(0, 512) || std::memcmp(h.Data(0,8),magic,8)) throw Error("not_compound_file");
    version = h.U16(26);
    if (h.U16(28) != 0xFFFE || (version != 3 && version != 4) ||
        h.U16(30) != (version == 3 ? 9 : 12) || h.U16(32) != 6 || h.U32(56) != 4096)
        throw Error("cfb_header_unsupported");
    sectorSize = uint32_t(1) << h.U16(30);
    if (bytes_.size() % sectorSize || bytes_.size() < uint64_t(sectorSize) * 2) throw Error("cfb_file_alignment_invalid");
    sectors_ = static_cast<uint32_t>(bytes_.size() / sectorSize - 1);
    reserved_.assign(sectors_, false);
    const uint32_t fatCount = h.U32(44), difatCount = h.U32(72);
    if (!fatCount || fatCount > sectors_ || difatCount > sectors_ ||
        uint64_t(fatCount) * (sectorSize / 4) > (1u << 20)) throw Error("cfb_fat_count_invalid");
    std::vector<uint32_t> fatSectors;
    auto addFat = [&](uint32_t id) {
        if (id == kFree) return;
        if (id >= sectors_ || reserved_[id] || fatSectors.size() >= fatCount) throw Error("cfb_duplicate_or_invalid_fat_sector");
        reserved_[id] = true; fatSectors.push_back(id);
    };
    for (uint32_t i = 0; i < 109; ++i) addFat(h.U32(76 + i * 4ull));
    std::vector<uint32_t> difatSectors;
    uint32_t difat = h.U32(68);
    for (uint32_t i = 0; i < difatCount; ++i) {
        if (difat >= sectors_ || reserved_[difat]) throw Error("cfb_difat_cycle_or_index_invalid");
        reserved_[difat] = true; difatSectors.push_back(difat);
        const View block = Sector(difat);
        for (uint32_t n = 0; n < sectorSize / 4 - 1; ++n) addFat(block.U32(n * 4ull));
        difat = block.U32(sectorSize - 4);
    }
    if (fatSectors.size() != fatCount || (difatCount && difat != kEnd)) throw Error("cfb_difat_count_mismatch");
    fat_.reserve(size_t(fatCount) * (sectorSize / 4));
    for (uint32_t id : fatSectors) {
        const View block = Sector(id);
        for (uint32_t n = 0; n < sectorSize; n += 4) fat_.push_back(block.U32(n));
    }
    if (fat_.size() < sectors_) throw Error("cfb_fat_too_short");
    for (uint32_t id : fatSectors) if (fat_[id] != kFat) throw Error("cfb_fat_marker_invalid");
    for (uint32_t id : difatSectors) if (fat_[id] != kDifat) throw Error("cfb_difat_marker_invalid");
    const auto directory = Chain(h.U32(48), fat_, 0, false, false);
    if (directory.empty() || directory.size() % 128 || directory.size() / 128 > kEntries)
        throw Error("cfb_directory_size_invalid");
    if (version == 4 && directory.size() / sectorSize != h.U32(40)) throw Error("cfb_directory_sector_count_mismatch");
    const View d(directory);
    entries.resize(directory.size()/128); links_.resize(entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        const View v = d.Slice(i*128, 128);
        Entry& e = entries[i]; e.index = static_cast<uint32_t>(i); e.type = v.U8(66);
        if (!e.type) continue;
        if (e.type != 1 && e.type != 2 && e.type != 5) throw Error("cfb_directory_type_invalid");
        const uint16_t length = v.U16(64);
        if (length < 2 || length > 64 || length % 2 || v.U16(length - 2) != 0) throw Error("cfb_directory_name_invalid");
        const View name = v.Slice(0, length - 2);
        e.name = bounded::Utf16(name); e.decodedName = e.name;
        e.clsid = bounded::Guid(v,80); e.start = v.U32(116); e.size = v.U64(120);
        if (version == 3 && (e.size >> 32)) throw Error("cfb_v3_stream_size_invalid");
        links_[i] = {v.U32(68),v.U32(72),v.U32(76)};
        if (i && e.type == 5) throw Error("cfb_multiple_roots");
    }
    if (entries[0].type != 5) throw Error("cfb_root_missing");
    rootClsid = entries[0].clsid;
    msi = rootClsid == "000c1084-0000-0000-c000-000000000046";
    if (msi) for (size_t i=1;i<entries.size();++i) if(entries[i].type) {
        const View v=d.Slice(i*128,128); const auto decoded=DecodeName(v.Slice(0,v.U16(64)-2));
        entries[i].decodedName=decoded.first; entries[i].table=decoded.second;
    }
    entries[0].reachable=true;
    Walk(links_[0][2]);
    const uint32_t miniCount=h.U32(64);
    if(miniCount>sectors_)throw Error("cfb_minifat_count_invalid");
    if(miniCount){
        const auto mini=Chain(h.U32(60),fat_,uint64_t(miniCount)*sectorSize,true,false);
        if(mini.size()/4>(1u<<20))throw Error("cfb_minifat_limit");
        const View v(mini); miniFat_.reserve(mini.size()/4);
        for(size_t i=0;i<mini.size();i+=4)miniFat_.push_back(v.U32(i));
    }
    if(entries[0].size)miniStream_=Chain(entries[0].start,fat_,entries[0].size,true,false);
}

void File::Walk(uint32_t child) {
    struct Work { uint32_t id,parent,depth; };
    std::vector<Work> todo={{child,0,0}};
    std::set<std::string> paths;
    while(!todo.empty()) {
        const Work w=todo.back();todo.pop_back();
        if(w.id==kFree)continue;
        if(w.id>=entries.size() || entries[w.id].reachable || !entries[w.id].type)throw Error("cfb_directory_cycle_or_index_invalid");
        if(w.depth>64)throw Error("cfb_storage_depth_limit");
        Entry& e=entries[w.id];e.reachable=true;e.parent=w.parent;
        if(!SafeComponent(e.decodedName))throw Error("cfb_unsafe_entry_name");
        const std::string prefix=msi&&e.type==2?(e.table?"tables/":"streams/"):"";
        e.path=entries[w.parent].path+(entries[w.parent].path.empty()?"":"/")+prefix+e.decodedName;
        if(e.path.size()>4096 || !paths.insert(e.path).second)throw Error("cfb_path_collision_or_limit");
        if(e.parent==0 && e.type==2 && !rootNames_.emplace(std::make_pair(e.decodedName,e.table),e.index).second)
            throw Error("cfb_duplicate_root_stream");
        todo.push_back({links_[w.id][0],w.parent,w.depth});
        todo.push_back({links_[w.id][1],w.parent,w.depth});
        if(e.type==1)todo.push_back({links_[w.id][2],w.id,w.depth+1});
        else if(links_[w.id][2]!=kFree)throw Error("cfb_stream_has_children");
    }
}

std::vector<uint8_t> File::ReadStream(uint32_t index) const {
    if(index>=entries.size() || !entries[index].reachable || entries[index].type!=2)throw Error("cfb_stream_index_invalid");
    const Entry& e=entries[index];
    return e.size<4096?Chain(e.start,miniFat_,e.size,true,true):Chain(e.start,fat_,e.size,true,false);
}
const Entry* File::FindRoot(const std::string& name,bool table) const {
    const auto it=rootNames_.find({name,table});return it==rootNames_.end()?nullptr:&entries[it->second];
}
} // namespace cortex::artifacts::compound
