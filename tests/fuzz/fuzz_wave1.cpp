// Dedicated wave-1 fuzzer. Mode byte: PDB, ELF, CAB, CFB/MSI, XML,
// ZIP/APPX, or dictionary-aware DEFLATE. Never writes, executes or loads
// analyzed content. The same entry point is used by libFuzzer and replay.
#include "artifacts/artifact.h"
#include "artifacts/pdb_provider.h"
#include "artifacts/elf_provider.h"
#include "artifacts/cab_provider.h"
#include "artifacts/compound_provider.h"
#include "artifacts/zip_provider.h"
#include "artifacts/xml_reader.h"
#include "artifacts/inflate.h"
#include <algorithm>
#include <cstdint>
#include <vector>

using namespace cortex::artifacts;
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* input,size_t size){
    if(!size||size>(1u<<20))return 0;
    const unsigned mode=input[0]%7;++input;--size;
    if(mode==4){
        try{
            const xml::Document doc(std::vector<uint8_t>(input,input+size));
            for(const auto& n:doc.nodes)(void)json{{"name",n.qname},{"ns",n.ns},{"text",n.text},{"attrs",n.attributes}}.dump();
        }catch(const bounded::Error&){}
        return 0;
    }
    if(mode==6){
        if(!size)return 0;
        const size_t dict=std::min<size_t>(input[0],size-1);
        std::vector<uint8_t> output;std::string error;
        (void)InflateWithDictionary(input+1+dict,size-1-dict,input+1,dict,output,32768,error);
        return 0;
    }
    const char* names[]={"input.pdb","input.elf","input.cab","input.msi","input.xml","input.msix"};
    auto source=FromBytes(std::vector<uint8_t>(input,input+size),names[mode]);
    PdbProvider pdb;ElfProvider elf;CabProvider cab;CompoundProvider cfb;ZipProvider zip;
    const Provider* provider=mode==0?static_cast<const Provider*>(&pdb):mode==1?static_cast<const Provider*>(&elf):
        mode==2?static_cast<const Provider*>(&cab):mode==3?static_cast<const Provider*>(&cfb):static_cast<const Provider*>(&zip);
    InspectOptions options;options.maxEntries=32;std::string error;
    (void)provider->Detect(*source);
    const auto report=provider->Inspect(*source,options,error);(void)report.dump();
    if(report.is_null())return 0;
    error.clear();const auto children=provider->Children(*source,options,error);
    size_t opened=0;
    for(const auto& child:children){
        if(!child.readable)continue;if(opened++==2)break;
        error.clear();auto inner=provider->OpenChild(source,child,error);
        if(inner){const auto bytes=inner->ReadSome(0,64);(void)bytes;}
    }
    return 0;
}
