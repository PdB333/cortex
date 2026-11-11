#pragma once
#include "artifacts/artifact.h"
#include "artifacts/xml_reader.h"
#include <iostream>
#include <random>

namespace appx_tests {
using namespace cortex::artifacts;
inline int Run(const std::string& fixtures){
    int failed=0;
    auto check=[&](bool ok,const char* msg){if(!ok){++failed;std::cerr<<"APPX: "<<msg<<'\n';}};
    for(const char* name:{"sample.msix","utf16.appx","tampered.msix","dtd.msix","sample.msixbundle"}){
        try{
            auto source=OpenFile(fixtures+"/appx/"+name);check(bool(source),"fixture exists");if(!source)continue;
            const auto& registry=DefaultRegistry();const json report=registry.Inspect(*source);
            check(report.value("ok",false),"ZIP inspection succeeds");const auto& a=report.at("data").at("appx");
            const std::string kind(name);
            if(kind=="dtd.msix"){
                check(report.at("format")=="zip"&&a.at("error")=="xml_dtd_or_declaration_forbidden","DTD rejected, no external entity resolution");continue;
            }
            if(kind=="sample.msixbundle"){
                check(report.at("format")=="appxbundle"&&a.at("packages").at(0).at("present")==true,"bundle member linked");
                std::string error;auto inner=registry.OpenChild(source,"sample.msix",error);
                check(bool(inner)&&registry.Inspect(*inner).value("format","")=="appx","nested MSIX parsed");continue;
            }
            check(report.at("format")=="appx","APPX/MSIX structural format");
            check(a.at("identity").at("Name")=="Cortex.Fixture"&&a.at("identity").at("Publisher")=="CN=Cortex & Test","namespace and entity decoding");
            check(a.at("applications").at(0).at("Executable")=="never-run.exe","executable name listed only");
            check(a.at("signature").at("verification")=="not_performed","no false signature trust");
            check(a.at("block_map").at("authentication")=="not_verified","block hash is not authentication");
            if(kind=="tampered.msix")check(a.at("block_map").at("failed_files")==1,"changed data fails block hash with valid ZIP CRC");
            else check(a.at("block_map").at("matched_files")==3&&a.at("block_map").at("failed_files")==0,"SHA256 64-KiB blocks match hashlib");
            InspectOptions zero;zero.maxEntries=0;
            const auto bounded=registry.Inspect(*source,zero).at("data").at("appx");
            check(bounded.at("applications").empty()&&bounded.at("block_map").at("files").empty(),"bounded metadata");
        }catch(const std::exception& ex){++failed;std::cerr<<"APPX "<<name<<": "<<ex.what()<<'\n';}
    }
    auto parse=[](const std::string& text){return xml::Document(std::vector<uint8_t>(text.begin(),text.end()));};
    try{
        const auto doc=parse("<?xml version='1.0'?><p:R xmlns:p='urn:test'><p:C A='&#65;&#x1f600;'><!-- hi --><![CDATA[x<y]]></p:C></p:R>");
        check(doc.nodes.size()==2&&doc.nodes[1].ns=="urn:test"&&doc.nodes[1].text=="x<y","XML namespaces, numeric entities, CDATA");
    }catch(const std::exception& ex){++failed;std::cerr<<ex.what()<<'\n';}
    for(const char* invalid:{"", "<a><b></a>", "<a/><b/>", "<a x='1' x='2'/>",
                            "<!DOCTYPE a><a/>", "<a>&unknown;</a>", "<p:a/>", "<a>&#0;</a>",
                            "<a xmlns:p='urn:x' xmlns:q='urn:x' p:x='1' q:x='2'/>", "<a><!-- x--x --></a>"}){
        bool rejected=false;try{(void)parse(invalid);}catch(const bounded::Error&){rejected=true;}
        check(rejected,"malformed/unsafe XML refused");
    }
    std::string deep;for(int i=0;i<70;++i)deep+="<a>";for(int i=0;i<70;++i)deep+="</a>";
    bool rejected=false;try{(void)parse(deep);}catch(const bounded::Error&){rejected=true;}check(rejected,"XML nesting bound");
    std::mt19937 rng(0x584D4Cu);const std::string original="<R xmlns='urn:x'><C Id='safe'>text &amp; more</C></R>";
    for(int n=0;n<500;++n){
        auto text=original;for(int j=0;j<3;++j)text[rng()%text.size()]=static_cast<char>(rng());
        try{(void)parse(text);}catch(const bounded::Error&){}
    }
    return failed;
}
} // namespace appx_tests
