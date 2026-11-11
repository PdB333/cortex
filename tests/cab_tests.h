#pragma once
#include "artifacts/artifact.h"
#include "artifacts/cab_provider.h"
#include <algorithm>
#include <iostream>
#include <random>

namespace cab_tests {
using namespace cortex::artifacts;
inline int Run(const std::string& fixtures) {
    int failed=0;
    auto check=[&](bool ok,const char* label){if(!ok){++failed;std::cerr<<"CAB: "<<label<<'\n';}};
    auto expected=OpenFile(fixtures+"/cab/expected.bin");
    check(bool(expected),"expected bytes exist"); if(!expected)return failed;
    const auto plain=expected->ReadSome(0,static_cast<size_t>(expected->Size()));
    for(const char* name:{"stored.cab","mszip-history.cab","microsoft-mszip.cab"}) {
        try {
            auto source=OpenFile(fixtures+"/cab/"+name);check(bool(source),"archive exists");if(!source)continue;
            const auto& registry=DefaultRegistry();
            const json report=registry.Inspect(*source); check(report.value("ok",false)&&report.value("format","")=="cab","registry detects CAB");
            const json children=registry.ListChildren(*source);
            check(children.value("ok",false),"file list available");
            std::vector<uint8_t> joined;
            for(const auto& child:children.at("children")) {
                check(child.at("readable")==true,"supported file is readable");
                std::string error;
                auto inner=registry.OpenChild(source,child.at("name"),error);
                check(bool(inner),"stored/MSZIP child expands");
                if(!inner){std::cerr<<name<<": "<<error<<'\n';continue;}
                const auto bytes=inner->ReadSome(0,static_cast<size_t>(inner->Size()));
                joined.insert(joined.end(),bytes.begin(),bytes.end());
            }
            check(joined==plain,"all expanded bytes match independent input");
            for(const auto& folder:report.at("data").at("folders"))check(!folder.contains("error"),"valid data block checksums");
            InspectOptions small;small.maxEntries=0;
            check(registry.Inspect(*source,small).at("data").at("files").empty(),"zero list bound");
            const auto original=source->ReadSome(0,static_cast<size_t>(source->Size()));
            auto bad=original;
            // Only mutate the first CFDATA payload, leaving all directory fields intact.
            uint32_t dataOffset=0;ReadU32(*source,36,dataOffset);
            if(dataOffset+9<bad.size())bad[dataOffset+9]^=0x55;
            const auto broken=registry.Inspect(*FromBytes(bad,"bad.cab"));
            check(broken.at("data").at("folders").at(0).contains("error"),"tampered CFDATA refused");
            for(size_t n=0;n<std::min<size_t>(original.size(),2048);n+=31)
                (void)registry.Inspect(*FromBytes(std::vector<uint8_t>(original.begin(),original.begin()+n),"cut.cab")).dump();
            std::mt19937 rng(0x434142u);
            for(unsigned n=0;n<100;++n){
                auto bytes=original;
                for(unsigned j=0;j<3;++j)bytes[rng()%bytes.size()]=static_cast<uint8_t>(rng());
                (void)registry.Inspect(*FromBytes(std::move(bytes),"mutated.cab")).dump();
            }
        }catch(const std::exception& ex){++failed;std::cerr<<"CAB "<<name<<": "<<ex.what()<<'\n';}
    }
    try {
        auto unsafe=OpenFile(fixtures+"/cab/unsafe.cab");check(bool(unsafe),"unsafe-name fixture exists");
        if(unsafe){std::string error;check(!DefaultRegistry().OpenChild(unsafe,"../escape.bin",error),"path traversal rejected");}
        auto stored=OpenFile(fixtures+"/cab/stored.cab");
        if(stored){
            auto bytes=stored->ReadSome(0,static_cast<size_t>(stored->Size()));
            bytes[42]=3; // LZX metadata with stored payload: inventory only, never decoded.
            auto unsupported=FromBytes(bytes,"lzx.cab");
            const auto report=DefaultRegistry().ListChildren(*unsupported);
            check(report.at("children").at(0).at("readable")==false,"LZX explicitly unavailable");
            std::string error;check(!DefaultRegistry().OpenChild(unsupported,"a.bin",error),"unsupported compression not guessed");
        }
    }catch(const std::exception& ex){++failed;std::cerr<<"CAB negatives: "<<ex.what()<<'\n';}
    return failed;
}
} // namespace cab_tests
