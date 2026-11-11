#pragma once
#include "artifacts/artifact.h"
#include "artifacts/compound_reader.h"
#include <iostream>
#include <random>

namespace compound_tests {
using namespace cortex::artifacts;
inline int Run(const std::string& fixtures){
    int failed=0;
    auto check=[&](bool ok,const char* what){if(!ok){++failed;std::cerr<<"CFB/MSI: "<<what<<'\n';}};
    for(const char* name:{"structured-v3.cfb","structured-v4.cfb"}){
        try{
            auto source=OpenFile(fixtures+"/compound/"+name);check(bool(source),"fixture exists");if(!source)continue;
            const auto& registry=DefaultRegistry();
            const auto report=registry.Inspect(*source);check(report.value("ok",false)&&report.value("format","")=="cfb","CFB recognized");
            const auto& d=report.at("data");
            check(d.at("stream_count")==3&&d.at("storage_count")==1,"storage tree inventory");
            const json list=registry.ListChildren(*source);check(list.at("children").size()==3,"all children listed");
            std::string error;
            auto tiny=registry.OpenChild(source,"tiny.txt",error),nested=registry.OpenChild(source,"folder/nested.txt",error),large=registry.OpenChild(source,"large.bin",error);
            check(bool(tiny)&&bool(nested)&&bool(large),"FAT and MiniFAT children read");
            if(tiny){const auto b=tiny->ReadSome(0,11);check(std::string(b.begin(),b.end())=="Cortex tiny","mini stream bytes");}
            if(nested){const auto b=nested->ReadSome(0,12);check(std::string(b.begin(),b.end())=="Cortex child","nested stream bytes");}
            if(large){const auto b=large->ReadSome(0,4096);bool same=b.size()==4096;for(size_t i=0;i<b.size();++i)same &= b[i]==uint8_t((i*17+3)%256);check(same,"normal stream bytes");}
            InspectOptions zero;zero.maxEntries=0;
            check(registry.Inspect(*source,zero).at("data").at("entries").empty(),"bounded listing");
            const auto original=source->ReadSome(0,static_cast<size_t>(source->Size()));
            const compound::File parsed(*source);
            auto bad=original;
            const auto* entry=parsed.FindRoot("large.bin",false);
            check(entry!=nullptr,"root stream lookup");
            if(entry){
                const size_t at=parsed.sectorSize+entry->start*4ull;
                for(unsigned i=0;i<4;++i)bad[at+i]=static_cast<uint8_t>(entry->start>>(8*i));
                check(!registry.OpenChild(FromBytes(bad,"cycle.cfb"),"large.bin",error),"FAT cycle refused");
            }
            bad=original;
            const size_t childAt=parsed.sectorSize*2+76;
            for(unsigned i=0;i<4;++i)bad[childAt+i]=0;
            check(!registry.Inspect(*FromBytes(bad,"cycle.cfb")).value("ok",true),"directory cycle refused");
            std::mt19937 rng(0x434642u);
            for(unsigned n=0;n<150;++n){
                auto b=original;for(unsigned j=0;j<4;++j)b[rng()%b.size()]=static_cast<uint8_t>(rng());
                (void)registry.Inspect(*FromBytes(std::move(b),"mutation.cfb")).dump();
            }
        }catch(const std::exception& ex){++failed;std::cerr<<"CFB "<<name<<": "<<ex.what()<<'\n';}
    }
    try{
        auto source=OpenFile(fixtures+"/compound/microsoft.msi");check(bool(source),"Microsoft MSI fixture exists");
        if(source){
            const auto& registry=DefaultRegistry();
            const json report=registry.Inspect(*source);
            check(report.value("ok",false)&&report.value("format","")=="msi","MSI structurally recognized");
            const auto& msi=report.at("data").at("msi");
            check(!msi.contains("error"),"MSI catalogs decoded");
            if(msi.contains("error")){std::cerr<<msi.dump(2)<<'\n';return failed;}
            auto table=[&](const char* name){for(const auto& t:msi.at("tables"))if(t.at("name")==name)return t;return json();};
            const json property=table("Property"),actions=table("CustomAction"),numbers=table("Numbers");
            check(property.at("rows").size()==2,"Property row count");
            bool product=false;
            for(const auto& r:property.at("rows"))product |= r.at("Property")=="ProductName"&&r.at("Value")=="Cortex inert fixture";
            check(product,"string pool values match producer");
            check(actions.at("rows").at(0).at("Action")=="NeverRun"&&actions.at("rows").at(0).at("Type")==51&&
                  actions.at("rows").at(0).at("Target")=="THIS_IS_DATA_ONLY","CustomAction remains plain data");
            check(numbers.at("rows").at(0).at("Value")==-123456&&numbers.at("rows").at(1).at("Value")==70000&&
                  numbers.at("rows").at(2).at("Value").is_null(),"biased integers and nulls");
            std::string error;
            auto cab=registry.OpenChild(source,"streams/payload.cab",error);check(bool(cab),"embedded CAB readable");
            if(cab){
                check(registry.Inspect(*cab).value("format","")=="cab","nested CAB parser used");
                auto inner=registry.OpenChild(cab,"a.bin",error);check(bool(inner)&&inner->Size()==32768,"MSI -> CAB -> file reading");
            }
            InspectOptions one;one.maxEntries=1;
            const auto small=registry.Inspect(*source,one).at("data").at("msi");
            check(small.at("tables").size()<=1&&small.at("truncated")==true,"MSI bounded total rows/tables");
        }
    }catch(const std::exception& ex){++failed;std::cerr<<"MSI real: "<<ex.what()<<'\n';}
    return failed;
}
} // namespace compound_tests
