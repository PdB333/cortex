#include "compound_provider.h"
#include "compound_reader.h"
#include <algorithm>
#include <cstring>

namespace cortex::artifacts {
std::vector<Capability> CompoundProvider::Capabilities() const {
    return {{"static", CapabilityStatus::Experimental, "tests/fixtures/compound; cortex_artifacts_tests",
             "CFB v3/v4, bounded DIFAT/FAT/MiniFAT chains, storage/stream inventory and nested bytes; MSI string pool, schemas, table values and CustomAction data (never run). 64 MiB file/stream cap."},
            {"decompile", CapabilityStatus::Unavailable, "", "nested members are inspected by their own providers"},
            {"live", CapabilityStatus::Unavailable, "", "never activates COM or invokes Windows Installer"}};
}
int CompoundProvider::Detect(const ByteSource& source) const {
    static const uint8_t magic[]={0xD0,0xCF,0x11,0xE0,0xA1,0xB1,0x1A,0xE1};
    uint8_t bytes[8];return source.Read(0,bytes,8)&&!std::memcmp(bytes,magic,8)?80:0;
}
json CompoundProvider::Inspect(const ByteSource& source,const InspectOptions& options,std::string& error) const {
    try{
        const compound::File file(source);
        const size_t limit=std::min<size_t>(options.maxEntries,65536);
        json result={{"format",file.msi?"msi":"cfb"},{"version",file.version},{"sector_size",file.sectorSize},
                     {"mini_sector_size",64},{"root_clsid",file.rootClsid},{"entries",json::array()}};
        size_t streams=0,storages=0,orphans=0;
        bool truncated=false;
        for(const auto& e:file.entries){
            if(!e.type)continue;
            if(!e.reachable){++orphans;continue;}
            streams+=e.type==2;storages+=e.type==1;
            if(e.index==0)continue;
            if(result["entries"].size()>=limit){truncated=true;continue;}
            result["entries"].push_back({{"index",e.index},{"path",e.path},{"name",e.decodedName},
                {"raw_name",e.name},{"kind",e.type==1?"storage":"stream"},{"size",e.size},
                {"clsid",e.clsid},{"table_stream",e.table},{"mini_stream",e.type==2&&e.size<4096}});
        }
        result["stream_count"]=streams;result["storage_count"]=storages;result["unreachable_entries"]=orphans;
        result["truncated"]=truncated;
        result["limits"]={{"max_input_bytes",64u<<20},{"max_stream_bytes",64u<<20},{"max_directory_entries",65536},
                          {"directory_validation","bounds, identities and cycles; red-black balancing is not a conformance check"}};
        if(file.msi)result["msi"]=compound::InspectMsi(file,options);
        return result;
    }catch(const bounded::Error& ex){error=ex.what();return nullptr;}
}
std::vector<Child> CompoundProvider::Children(const ByteSource& source,const InspectOptions& options,std::string& error) const{
    try{
        const compound::File file(source);std::vector<Child> children;
        const size_t limit=std::min<size_t>(options.maxEntries,65536);
        for(const auto& e:file.entries){
            if(!e.reachable||e.type!=2)continue;
            if(children.size()>=limit)break;
            Child c;c.name=e.path;c.offset=e.index;c.size=e.size;c.readable=e.size<=(64u<<20);
            if(!c.readable)c.note="stream_size_limit";
            children.push_back(std::move(c));
        }
        return children;
    }catch(const bounded::Error& ex){error=ex.what();return {};}
}
ByteSourcePtr CompoundProvider::OpenChild(ByteSourcePtr container,const Child& child,std::string& error) const{
    try{
        if(!container)throw bounded::Error("no_container");
        const compound::File file(*container);
        if(child.offset>=file.entries.size() || file.entries[child.offset].path!=child.name)
            throw bounded::Error("cfb_child_identity_invalid");
        return FromBytes(file.ReadStream(static_cast<uint32_t>(child.offset)),container->Name()+"!"+child.name);
    }catch(const bounded::Error& ex){error=ex.what();return {};}
}
} // namespace cortex::artifacts
