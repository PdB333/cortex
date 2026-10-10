#include "../mcp_bridge/pe_unwind.h"

#include <algorithm>
#include <iostream>
#include <vector>

using json = nlohmann::json;
static int errors=0;
static void Check(bool good,const char* why){
    if(!good){std::cerr<<"FAIL: "<<why<<'\n';++errors;}
}
static void U16(std::vector<uint8_t>& image,size_t pos,uint16_t value){
    image.at(pos)=static_cast<uint8_t>(value);
    image.at(pos+1)=static_cast<uint8_t>(value>>8);
}
static void U32(std::vector<uint8_t>& image,size_t pos,uint32_t value){
    for(size_t n=0;n<4;++n)image.at(pos+n)=static_cast<uint8_t>(value>>(8*n));
}

int main(){
    constexpr uint64_t base=0x180000000ull;
    std::vector<uint8_t> image(0x10000,0);
    image[0]='M';image[1]='Z';
    U32(image,0x3c,0x100);
    U32(image,0x100,0x4550); // PE signature
    U16(image,0x104,0x8664); // IMAGE_FILE_MACHINE_AMD64
    U16(image,0x100+20,0xf0); // optional header size
    U16(image,0x100+24,0x20b); // PE32+
    U32(image,0x100+24+56,0x10000); // SizeOfImage
    U32(image,0x100+24+108,16); // NumberOfRvaAndSizes
    U32(image,0x100+24+112+24,0x5000); // exception directory RVA
    U32(image,0x100+24+112+28,36); // three RUNTIME_FUNCTION entries
    U32(image,0x5000+0,0x1000);
    U32(image,0x5000+4,0x1080);
    U32(image,0x5000+8,0x6000);
    U32(image,0x500c+0,0x1200);
    U32(image,0x500c+4,0x1280);
    U32(image,0x500c+8,0x6020);
    U32(image,0x5018+0,0x1300);
    U32(image,0x5018+4,0x1350);
    U32(image,0x5018+8,0x6040);
    size_t readCalls=0;
    auto fetch=[&](uint64_t addr,size_t count,std::vector<uint8_t>& out)->bool{
        ++readCalls;
        if(addr<base || addr-base>image.size() ||
           count>image.size()-(addr-base))return false;
        out.assign(image.begin()+static_cast<std::ptrdiff_t>(addr-base),
                   image.begin()+static_cast<std::ptrdiff_t>(addr-base+count));
        return true;
    };
    const auto resolved=cortex::test::pe_unwind::Find(base,image.size(),base+0x1214,fetch);
    Check(resolved["status"]=="unwind_range" &&
          resolved["begin_rva"]=="0x1200" &&
          resolved["end_rva"]=="0x1280" &&
          resolved["begin_address"]=="0x180001200" &&
          resolved["size"]==0x80 &&
          resolved["source"]=="live_pe_exception_directory",
          "exact x64 unwind range mapped from loaded image metadata");
    Check(readCalls<=22,"binary search bounded, no whole module scan");
    Check(cortex::test::pe_unwind::Find(base,image.size(),base+0x1100,fetch)
          ["status"]=="unavailable","gap between entries not inferred");
    Check(cortex::test::pe_unwind::Find(base,image.size(),base+0x1280,fetch)
          ["status"]=="unavailable","end is exclusive");
    Check(cortex::test::pe_unwind::Find(base,image.size(),base+image.size(),fetch)
          ["reason"]=="address_outside_loaded_module","out of module rejected");
    const auto unreadable=cortex::test::pe_unwind::Find(base,image.size(),base+0x1214,
        [](uint64_t,size_t,std::vector<uint8_t>&)->bool{return false;});
    Check(unreadable["reason"]=="pe_header_unreadable","memory failures explicit");
    auto modified=image;
    modified[0]='N';
    auto changed=[&](uint64_t addr,size_t count,std::vector<uint8_t>& out)->bool{
        if(addr<base||count>modified.size()-(addr-base))return false;
        out.assign(modified.begin()+static_cast<std::ptrdiff_t>(addr-base),
                   modified.begin()+static_cast<std::ptrdiff_t>(addr-base+count));
        return true;
    };
    Check(cortex::test::pe_unwind::Find(base,image.size(),base+0x1214,changed)
          ["reason"]=="not_pe_image","no invented functions for non-PE code");
    modified=image;
    U16(modified,0x104,0x14c); // x86
    Check(cortex::test::pe_unwind::Find(base,image.size(),base+0x1214,changed)
          ["reason"]=="not_amd64_pe32plus","PE32/x86 handled as unsupported");
    modified=image;
    U32(modified,0x100+24+112+28,0x10000000);
    Check(cortex::test::pe_unwind::Find(base,image.size(),base+0x1214,changed)
          ["reason"]=="invalid_exception_directory","table budget and image bounds enforced");
    modified=image;
    U32(modified,0x500c+4,0x1000);
    Check(cortex::test::pe_unwind::Find(base,image.size(),base+0x1214,changed)
          ["reason"]=="invalid_unwind_entry","invalid ranges do not create fake function");
    if(errors)return 1;
    std::cout<<"PASS: bounded x64 PE unwind range lookup from loaded-memory metadata\n";
    return 0;
}
