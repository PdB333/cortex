#include "backend.h"
#include <algorithm>

namespace cortex::remote {
namespace {
class Fixture final : public Backend {
    AgentPolicy policy_;Bytes memory_;
    static constexpr uint32_t pid_=42;
    static constexpr uint64_t base_=0x100000;
    std::string Token(const std::string& instance)const{return instance+":42:fixture-1";}
public:
    explicit Fixture(const AgentPolicy& policy):policy_(policy),memory_(1048576){for(size_t i=0;i<memory_.size();++i)memory_[i]=static_cast<uint8_t>(i);}
    std::string Platform()const override{return "fixture";}
    json Capabilities()const override{return{{"processes",true},{"maps",true},{"memory_read",true},{"memory_write",policy_.allowWrites},{"target_generation",true},{"read_effect","fixture_only"},{"max_memory_bytes",kMaxMemory},{"breakpoints",false},{"hardware_tested",false}};}
    json Execute(const std::string& action,const json& args,const std::string& instance)override{
        ValidateArguments(action,args);
        if(action=="info")return{{"ok",true},{"capabilities",Capabilities()},{"backend","in-process test vector"}};
        if(action=="processes")return{{"ok",true},{"processes",json::array({{{"pid",pid_},{"name","CortexFixture"},{"target_id",Token(instance)}}})},{"total",1},{"truncated",false}};
        Require(UInt(args,"pid",1,2147483647)==pid_,"pid_not_allowed");
        Require(Text(args,"target_id",256)==Token(instance),"stale_target");
        if(action=="maps")return{{"ok",true},{"pid",pid_},{"target_id",Token(instance)},{"maps",json::array({{{"name","fixture.data"},{"start",Hex(base_)},{"end",Hex(base_+memory_.size())},{"offset","0x0"},{"read",true},{"write",true},{"execute",false}}})},{"total",1},{"truncated",false}};
        const uint64_t address=Number(Text(args,"address",20));
        Bytes replacement;size_t length=0;
        if(action=="write"){Require(policy_.allowWrites,"agent_read_only");Require(Text(args,"expected_name",128)=="CortexFixture","process_identity_mismatch");replacement=Unhex(Text(args,"hex",kMaxMemory*2));length=replacement.size();}
        else length=static_cast<size_t>(UInt(args,"length",1,kMaxMemory));
        Require(address>=base_&&address-base_<=memory_.size()&&length<=memory_.size()-(address-base_),"memory_range_unavailable");
        const size_t offset=static_cast<size_t>(address-base_);Bytes before(memory_.begin()+offset,memory_.begin()+offset+length);
        if(action=="read")return{{"ok",true},{"pid",pid_},{"target_id",Token(instance)},{"address",Hex(address)},{"length",length},{"hex",ByteHex(before)}};
        Require(before==Unhex(Text(args,"expected_hex",kMaxMemory*2)),"expected_bytes_mismatch");
        // Allocate the result and readback storage BEFORE touching memory.
        // No fallible allocation can then be misreported as a pre-write failure.
        json result={{"ok",true},{"pid",pid_},{"target_id",Token(instance)},{"address",Hex(address)},{"length",length},{"write_state","readback_matched"},{"atomic",false}};
        Bytes after(length);
        std::copy(replacement.begin(),replacement.end(),memory_.begin()+offset);
        std::copy(memory_.begin()+offset,memory_.begin()+offset+length,after.begin());
        if(after!=replacement){result["ok"]=false;result["write_state"]="may_have_changed";}
        return result;
    }
};
}
std::unique_ptr<Backend> MakeFixtureBackend(const AgentPolicy& policy){return std::make_unique<Fixture>(policy);}
} // namespace cortex::remote
