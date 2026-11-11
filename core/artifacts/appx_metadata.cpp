#include "appx_metadata.h"
#include "xml_reader.h"
#include "zip_provider.h"
#include "sha256.h"
#include <algorithm>
#include <limits>
#include <map>
#include <set>

// Microsoft APPX manifest, bundle and block-map schemas. Files are never
// registered/installed. SHA-256 block checks compare data to an UNTRUSTED
// block map and are deliberately separate from signature authentication.
namespace cortex::artifacts {
namespace {
using bounded::Error;
constexpr const char* kBundle="http://schemas.microsoft.com/appx/2013/bundle";
constexpr const char* kBlockMap="http://schemas.microsoft.com/appx/2010/blockmap";
constexpr const char* kSha256="http://www.w3.org/2001/04/xmlenc#sha256";
bool PackageNamespace(const std::string& ns){
    return ns=="http://schemas.microsoft.com/appx/manifest/foundation/windows10"||
           ns=="http://schemas.microsoft.com/appx/2010/manifest"||ns=="http://schemas.microsoft.com/appx/2013/manifest";
}
uint64_t Number(const std::string& value){
    if(value.empty()||value.size()>20)throw Error("appx_number_invalid");
    uint64_t out=0;
    for(char c:value){if(c<'0'||c>'9'||out>(std::numeric_limits<uint64_t>::max()-(c-'0'))/10)throw Error("appx_number_invalid");out=out*10+c-'0';}
    return out;
}
std::string Base64(const std::array<uint8_t,32>& hash){
    static const char* digits="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for(size_t i=0;i<hash.size();i+=3){
        const size_t n=std::min<size_t>(3,hash.size()-i);
        const uint32_t x=(uint32_t(hash[i])<<16)|(n>1?uint32_t(hash[i+1])<<8:0)|(n>2?hash[i+2]:0);
        out+=digits[(x>>18)&63];out+=digits[(x>>12)&63];out+=n>1?digits[(x>>6)&63]:'=';out+=n>2?digits[x&63]:'=';
    }
    return out;
}
std::string Path(std::string name){
    std::replace(name.begin(),name.end(),'\\','/');
    if(name.empty()||name[0]=='/'||name.find(':')!=std::string::npos||name.find('\0')!=std::string::npos)throw Error("appx_path_invalid");
    size_t start=0;
    while(start<=name.size()){
        const size_t end=name.find('/',start);const auto part=name.substr(start,end==std::string::npos?std::string::npos:end-start);
        if(part.empty()||part=="."||part=="..")throw Error("appx_path_invalid");
        if(end==std::string::npos)break;start=end+1;
    }
    return name;
}
json Attributes(const xml::Node& node){
    json result=json::object();
    for(const auto& [key,value]:node.attributes)
        result[key]=value.size()<=4096?json(value):json{{"text",bounded::Utf8(reinterpret_cast<const uint8_t*>(value.data()),4096)},{"truncated",true}};
    return result;
}

class Package {
public:
    Package(const ByteSource& source,const std::vector<Child>& children,size_t limit):source_(source),limit_(std::min<size_t>(limit,4096)){
        for(const auto& child:children){
            const auto inserted=files_.emplace(child.name,&child);
            if(!inserted.second)duplicates_.insert(child.name);
        }
    }
    bool Has(const std::string& name)const{return files_.count(name)>0;}
    std::vector<uint8_t> Read(const std::string& name,uint64_t cap){
        const auto it=files_.find(name);
        if(it==files_.end())throw Error("appx_member_missing:"+name);
        if(duplicates_.count(name))throw Error("appx_duplicate_member:"+name);
        const Child& child=*it->second;
        if(!child.readable)throw Error("appx_member_unreadable:"+name);
        if(child.size>cap||child.size>remaining_||child.storedSize>(64u<<20))throw Error("appx_read_budget");
        remaining_-=child.size;
        ByteSourcePtr parent(&source_,[](const ByteSource*){});
        std::string error;auto inner=ZipProvider().OpenChild(parent,child,error);
        if(!inner)throw Error("appx_member:"+error);
        return bounded::Read(*inner,0,inner->Size(),cap);
    }
    json Inspect(bool bundle){
        const std::string manifest=bundle?"AppxMetadata/AppxBundleManifest.xml":"AppxManifest.xml";
        const xml::Document doc(Read(manifest,1u<<20));
        const auto& root=doc.nodes[0];
        if((bundle&&(root.local!="Bundle"||root.ns!=kBundle))||
           (!bundle&&(root.local!="Package"||!PackageNamespace(root.ns))))throw Error("appx_manifest_root_or_namespace_invalid");
        json result={{"format",bundle?"appxbundle":"appx"},{"manifest",manifest},{"namespace",root.ns},
                     {"container_aliases",bundle?json::array({"appxbundle","msixbundle"}):json::array({"appx","msix"})},
                     {"applications",json::array()},{"dependencies",json::array()},{"capabilities",json::array()},
                     {"packages",json::array()},{"truncated",false}};
        size_t identities=0;
        for(size_t i=1;i<doc.nodes.size();++i){
            const auto& n=doc.nodes[i];const auto& parent=doc.nodes[n.parent];
            if(n.parent==0&&n.local=="Identity"&&n.ns==root.ns){result["identity"]=Attributes(n);++identities;}
            else if(n.local=="Application"&&parent.local=="Applications"&&parent.parent==0&&n.ns==root.ns)
                Add(result,"applications",Attributes(n));
            else if(parent.local=="Dependencies"&&parent.parent==0)
                Add(result,"dependencies",{{"element",n.local},{"namespace",n.ns},{"attributes",Attributes(n)}});
            else if(parent.local=="Capabilities"&&parent.parent==0)
                Add(result,"capabilities",{{"element",n.local},{"namespace",n.ns},{"attributes",Attributes(n)}});
            else if(bundle&&n.local=="Package"&&parent.local=="Packages"&&parent.parent==0&&n.ns==kBundle){
                json item=Attributes(n);
                try{const std::string path=Path(n.Attr("FileName"));item["member"]=path;item["present"]=Has(path);}
                catch(const Error& ex){item["error"]=ex.what();}
                Add(result,"packages",std::move(item));
            }
        }
        if(identities!=1)result["identity_error"]="expected_exactly_one_identity";
        result["signature"]={{"present",Has("AppxSignature.p7x")},{"verification","not_performed"},
                             {"note","signature presence and block matches do not prove publisher identity or package authenticity"}};
        if(Has("AppxBlockMap.xml")){
            try{result["block_map"]=Blocks();}catch(const Error& ex){result["block_map"]={{"error",ex.what()},{"status","not_verified"}};}
        }else result["block_map"]={{"status","absent"}};
        result["limits"]={{"manifest_bytes",1u<<20},{"total_expanded_budget",64u<<20},
                          {"schema_validation","structural_metadata_only"},{"installation","never"}};
        return result;
    }
private:
    void Add(json& out,const char* key,json row){
        if(out[key].size()>=limit_){out["truncated"]=true;return;}out[key].push_back(std::move(row));
    }
    json Blocks(){
        const xml::Document doc(Read("AppxBlockMap.xml",1u<<20));
        const auto& root=doc.nodes[0];
        if(root.local!="BlockMap"||root.ns!=kBlockMap)throw Error("appx_blockmap_root_invalid");
        const std::string method=root.Attr("HashMethod");
        json result={{"hash_method",method},{"files",json::array()},{"matched_files",0},{"failed_files",0},
                     {"truncated",false},{"authentication","not_verified"}};
        if(method!=kSha256){result["status"]="unsupported_hash_method";return result;}
        size_t matched=0,failed=0;std::set<std::string> seen;
        for(size_t i=1;i<doc.nodes.size();++i){
            const auto& file=doc.nodes[i];if(file.parent!=0||file.local!="File"||file.ns!=kBlockMap)continue;
            if(result["files"].size()>=limit_){result["truncated"]=true;break;}
            json row={{"name",file.Attr("Name")}};
            try{
                const std::string name=Path(file.Attr("Name"));if(!seen.insert(name).second)throw Error("appx_blockmap_duplicate_file");
                const uint64_t size=Number(file.Attr("Size"));const auto data=Read(name,64u<<20);
                if(data.size()!=size)throw Error("appx_blockmap_file_size_mismatch");
                size_t offset=0,blocks=0;bool good=true;
                for(size_t j=i+1;j<doc.nodes.size();++j){
                    const auto& block=doc.nodes[j];if(block.parent!=i)continue;
                    if(block.local!="Block"||block.ns!=kBlockMap)throw Error("appx_blockmap_element_unsupported");
                    if(offset>=size)throw Error("appx_blockmap_extra_block");
                    const size_t take=std::min<size_t>(65536,data.size()-offset);Sha256 hash;
                    hash.Update(data.data()+offset,take);good &= Base64(hash.Final())==block.Attr("Hash");
                    offset+=take;++blocks;
                }
                if(offset!=size)throw Error("appx_blockmap_missing_block");
                row["blocks"]=blocks;row["status"]=good?"matches_block_map":"hash_mismatch";
                if(good)++matched;else++failed;
            }catch(const Error& ex){row["status"]="failed";row["error"]=ex.what();++failed;}
            result["files"].push_back(std::move(row));
        }
        result["matched_files"]=matched;result["failed_files"]=failed;
        result["status"]=failed?"mismatch_or_unreadable":result["truncated"].get<bool>()?"partial":"listed_files_match";
        result["note"]="Only files listed in this untrusted map were checked; compressed block sizes/local headers and signature trust are not verified";
        return result;
    }
    const ByteSource& source_;size_t limit_;uint64_t remaining_=64ull<<20;
    std::map<std::string,const Child*> files_;std::set<std::string> duplicates_;
};
} // namespace
json InspectAppx(const ByteSource& source,const std::vector<Child>& children,const InspectOptions& options){
    Package package(source,children,options.maxEntries);
    const bool manifest=package.Has("AppxManifest.xml"),bundle=package.Has("AppxMetadata/AppxBundleManifest.xml");
    if(!manifest&&!bundle)return json::object();
    try{
        if(manifest&&bundle)throw Error("appx_ambiguous_package_and_bundle");
        return package.Inspect(bundle);
    }catch(const Error& ex){return {{"error",ex.what()},{"status","unrecognized_or_invalid_manifest"}};}
}
} // namespace cortex::artifacts
