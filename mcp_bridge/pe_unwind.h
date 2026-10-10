#pragma once

#include <nlohmann/json.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace cortex::test::pe_unwind {
using json = nlohmann::json;

// Read bytes from the original process, not from a possibly outdated copy
// of the executable on disk. The caller owns PID/generation checks.
using ReadAt = std::function<bool(uint64_t, size_t, std::vector<uint8_t>&)>;

inline uint16_t U16(const std::vector<uint8_t>& bytes, size_t offset) {
    return static_cast<uint16_t>(bytes[offset]) |
           (static_cast<uint16_t>(bytes[offset+1]) << 8);
}
inline uint32_t U32(const std::vector<uint8_t>& bytes, size_t offset) {
    return static_cast<uint32_t>(bytes[offset]) |
           (static_cast<uint32_t>(bytes[offset+1]) << 8) |
           (static_cast<uint32_t>(bytes[offset+2]) << 16) |
           (static_cast<uint32_t>(bytes[offset+3]) << 24);
}
inline std::string Hex(uint64_t value) {
    std::ostringstream out; out << "0x" << std::hex << value; return out.str();
}

inline json Find(uint64_t base, uint64_t loadedSize, uint64_t ip,
                 const ReadAt& read) {
    const auto unavailable=[](const char* reason){
        return json{{"status","unavailable"},{"reason",reason}};
    };
    // Only PE32+ AMD64 exception-directory entries. Other architectures,
    // leaf functions and JIT code may not have corresponding unwind ranges.
    if (!read || !base || ip < base || !loadedSize ||
        loadedSize > UINT64_MAX-base || ip-base >= loadedSize)
        return unavailable("address_outside_loaded_module");

    auto readExact=[&](uint64_t address,size_t count,std::vector<uint8_t>& out){
        out.clear();
        if(address<base || address-base>=loadedSize ||
           count>loadedSize-(address-base))return false;
        return read(address,count,out) && out.size()==count;
    };
    std::vector<uint8_t> dos;
    if(!readExact(base,64,dos))return unavailable("pe_header_unreadable");
    if(dos[0]!='M' || dos[1]!='Z')return unavailable("not_pe_image");
    const uint32_t ntRva=U32(dos,0x3c);
    if(ntRva<64 || ntRva>4096)return unavailable("invalid_pe_header_offset");
    std::vector<uint8_t> nt;
    if(!readExact(base+ntRva,168,nt))return unavailable("pe_header_unreadable");
    if(U32(nt,0)!=0x4550 || U16(nt,4)!=0x8664 ||
       U16(nt,20)<144 || U16(nt,24)!=0x20b)
        return unavailable("not_amd64_pe32plus");
    const uint32_t imageSize=U32(nt,24+56);
    const uint32_t dirCount=U32(nt,24+108);
    if(imageSize<4096 || imageSize>loadedSize || dirCount<=3)
        return unavailable("invalid_pe_image_size_or_directories");
    const uint32_t directory=U32(nt,24+112+3*8);
    const uint32_t directorySize=U32(nt,24+112+3*8+4);
    if(!directory || directorySize<12)
        return unavailable("no_exception_directory");
    if(directorySize>8u*1024u*1024u ||
       directory>=imageSize || directorySize>imageSize-directory ||
       directory>=loadedSize || directorySize>loadedSize-directory)
        return unavailable("invalid_exception_directory");
    const size_t entries=directorySize/12;
    if(entries==0)return unavailable("empty_exception_directory");

    // PE .pdata entries are sorted by BeginAddress. Binary search reads at
    // most ~20 small rows, avoiding a whole-image scan. Malformed or missing
    // metadata never becomes a guessed function boundary.
    const uint64_t rva=ip-base;
    size_t low=0,high=entries;
    std::vector<uint8_t> row;
    while(low<high) {
        const size_t mid=low+(high-low)/2;
        if(!readExact(base+directory+mid*12,12,row))
            return unavailable("unwind_table_unreadable");
        const uint32_t begin=U32(row,0);
        const uint32_t end=U32(row,4);
        if(begin>=end || end>imageSize)
            return unavailable("invalid_unwind_entry");
        if(begin<=rva)low=mid+1;
        else high=mid;
    }
    if(low==0)return unavailable("address_not_covered");
    if(!readExact(base+directory+(low-1)*12,12,row))
        return unavailable("unwind_table_unreadable");
    const uint32_t begin=U32(row,0);
    const uint32_t end=U32(row,4);
    const uint32_t unwindRva=U32(row,8);
    if(begin>=end || end>imageSize || rva<begin || rva>=end ||
       !unwindRva || unwindRva>=imageSize)
        return unavailable("address_not_covered");
    return {
        {"status","unwind_range"},
        {"begin_rva",Hex(begin)},{"end_rva",Hex(end)},
        {"begin_address",Hex(base+begin)},{"end_address",Hex(base+end)},
        {"size",end-begin},{"source","live_pe_exception_directory"},
        {"caveat","This is a compiler/loader unwind range, not necessarily a complete source-level function."}
    };
}
} // namespace cortex::test::pe_unwind
