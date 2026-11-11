#include "backend.h"
#include "../../core/remote/queries.h"
#include <algorithm>
#include <cstring>
#include <set>
#include <stdexcept>

#if defined(__FreeBSD__) || defined(CORTEX_REMOTE_PS5)
#include <sys/types.h>
#include <sys/param.h>
#include <sys/user.h>
#include <sys/proc.h>
#include <sys/ptrace.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>

namespace cortex::remote {
namespace {
using Clock=std::chrono::steady_clock;
struct Mapping {uint64_t begin,end,offset;int protection;std::string name;};
class Native final : public Backend {
    AgentPolicy policy_;std::set<uint32_t> quarantined_;
    kinfo_proc Process(uint32_t pid)const{
        Require(std::find(policy_.allowedPids.begin(),policy_.allowedPids.end(),pid)!=policy_.allowedPids.end(),"pid_not_allowed");
        Require(pid!=static_cast<uint32_t>(getpid()),"agent_self_inspection_refused");
        int mib[]={CTL_KERN,KERN_PROC,KERN_PROC_PID,static_cast<int>(pid)};
        kinfo_proc info{};size_t size=sizeof(info);
        Require(sysctl(mib,4,&info,&size,nullptr,0)==0,"process_query_denied_or_unavailable");
        // Do not guess offsets for a console firmware with a different ABI.
        Require(size==sizeof(info)&&info.ki_structsize==static_cast<int>(sizeof(info))&&info.ki_pid==static_cast<pid_t>(pid),"process_layout_mismatch");
        Require(info.ki_start.tv_sec>0&&info.ki_start.tv_usec>=0&&info.ki_start.tv_usec<1000000,"process_generation_unavailable");
        return info;
    }
    static std::string Name(const kinfo_proc& p){return std::string(p.ki_comm,strnlen(p.ki_comm,sizeof(p.ki_comm)));}
    static std::string Token(const kinfo_proc& p,const std::string& instance){return instance+":"+std::to_string(p.ki_pid)+":"+std::to_string(p.ki_start.tv_sec)+":"+std::to_string(p.ki_start.tv_usec);}
    std::vector<Mapping> Mappings(uint32_t pid)const{
        int mib[]={CTL_KERN,KERN_PROC,KERN_PROC_VMMAP,static_cast<int>(pid)};size_t size=0;
        Require(sysctl(mib,4,nullptr,&size,nullptr,0)==0,"maps_query_denied_or_unavailable");
        Require(size<=8u*1024u*1024u,"map_bytes_limit");
        // A changing map may grow. One bounded reserve; no unbounded retry.
        size_t capacity=std::min<size_t>(8u*1024u*1024u,size+65536);Bytes data(capacity);size=capacity;
        Require(sysctl(mib,4,data.data(),&size,nullptr,0)==0&&size<=data.size(),"maps_changed_or_unavailable");
        std::vector<Mapping> maps;
        for(size_t at=0;at<size;){
            Require(size-at>=sizeof(int),"truncated_map_record");int recordSize=0;std::memcpy(&recordSize,data.data()+at,sizeof(recordSize));
            Require(recordSize>=static_cast<int>(offsetof(kinfo_vmentry,kve_path))&&static_cast<size_t>(recordSize)<=sizeof(kinfo_vmentry)&&static_cast<size_t>(recordSize)<=size-at,"map_layout_mismatch");
            kinfo_vmentry v{};std::memcpy(&v,data.data()+at,static_cast<size_t>(recordSize));
            Require(v.kve_end>=v.kve_start,"invalid_memory_map");
            if(v.kve_start<=kMaxUserAddress&&v.kve_end>v.kve_start&&v.kve_end-1<=kMaxUserAddress){
                Require(maps.size()<16384,"map_count_limit");
                maps.push_back({v.kve_start,v.kve_end,v.kve_offset,v.kve_protection,std::string(v.kve_path,strnlen(v.kve_path,std::min<size_t>(sizeof(v.kve_path),128)))});
            }
            at+=static_cast<size_t>(recordSize);
        }
        return maps;
    }
    void Range(uint32_t pid,uint64_t address,size_t length,bool write)const{
        for(const auto& m:Mappings(pid))if(address>=m.begin&&address<m.end&&length<=m.end-address){Require((m.protection&KVME_PROT_READ)!=0,"region_not_readable");if(write)Require((m.protection&KVME_PROT_WRITE)!=0,"region_not_writable");return;}
        throw std::runtime_error("memory_range_unavailable");
    }
public:
    explicit Native(const AgentPolicy& policy):policy_(policy){}
    std::string Platform()const override{
#ifdef CORTEX_REMOTE_PS5
        return "ps5";
#else
        return "freebsd";
#endif
    }
    json Capabilities()const override{return{{"processes",true},{"maps",true},{"memory_read",policy_.allowPtraceStop},{"memory_write",policy_.allowWrites&&policy_.allowPtraceStop},{"target_generation",true},{"read_effect","temporary_ptrace_stop"},{"max_memory_bytes",kMaxMemory},{"max_write_bytes",kMaxWriteMemory},{"memory_scan",policy_.allowPtraceStop},{"map_pagination",true},{"breakpoints",false},{"hardware_tested",false},{"abi_policy","SDK layout must match; no guessed offsets"}};}
    json Execute(const std::string& action,const json& args,const std::string& instance)override{
        bool attempted=false;
        try{return Run(action,args,instance,attempted);}
        catch(const std::exception& e){return{{"ok",false},{"error",e.what()},{"write_state",attempted?"may_have_changed":"not_written"},{"retry_safe",false}};}
    }
private:
    json Run(const std::string& action,const json& args,const std::string& instance,bool& attempted){
        ValidateArguments(action,args);
        if(action=="info")return{{"ok",true},{"backend","native sysctl/ptrace under existing OS permissions"},{"capabilities",Capabilities()}};
        if(action=="processes"){
            size_t limit=args.contains("limit")?static_cast<size_t>(UInt(args,"limit",1,256)):256;json rows=json::array(),unavailable=json::array();
            for(auto pid:policy_.allowedPids){try{auto p=Process(pid);if(rows.size()<limit)rows.push_back({{"pid",pid},{"name",Name(p)},{"target_id",Token(p,instance)}});}catch(const std::exception& e){unavailable.push_back({{"pid",pid},{"error",e.what()}});}}
            return{{"ok",true},{"processes",rows},{"unavailable",unavailable},{"allowlisted",policy_.allowedPids.size()},{"truncated",rows.size()==limit&&policy_.allowedPids.size()>limit}};
        }
        uint32_t pid=static_cast<uint32_t>(UInt(args,"pid",1,2147483647));auto p=Process(pid);
        Require(Token(p,instance)==Text(args,"target_id",256),"stale_target");
        if(action=="maps"){
            auto maps=Mappings(pid);Require(Token(Process(pid),instance)==Token(p,instance),"stale_target");
            std::vector<MemoryMapping> rows;rows.reserve(maps.size());
            for(const auto& m:maps)rows.push_back({m.begin,m.end,m.offset,bool(m.protection&KVME_PROT_READ),bool(m.protection&KVME_PROT_WRITE),bool(m.protection&KVME_PROT_EXEC),m.name});
            return MapPage(rows,args,pid,Token(p,instance));
        }
        Require(policy_.allowPtraceStop,"ptrace_stop_not_authorized");Require(!quarantined_.count(pid),"prior_detach_failed_restart_agent_after_manual_recovery");
        Require(p.ki_stat!=SSTOP&&p.ki_stat!=SZOMB&&(p.ki_flag&P_TRACED)==0,"target_not_in_running_untraced_state");
        bool write=action=="write",attached=false,possiblyWritten=false;int preserveSignal=0;
        if(write){Require(policy_.allowWrites,"agent_read_only");Require(Name(p)==Text(args,"expected_name",128),"process_identity_mismatch");}
        uint64_t address=Number(Text(args,"address",20));Bytes replacement=write?Unhex(Text(args,"hex",kMaxMemory*2)):Bytes();size_t length=write?replacement.size():static_cast<size_t>(UInt(args,"length",1,kMaxMemory));
        Range(pid,address,length,write);
        auto detach=[&](){if(!attached)return true;bool ok=ptrace(PT_DETACH,static_cast<pid_t>(pid),reinterpret_cast<caddr_t>(1),preserveSignal)==0;if(ok)attached=false;else quarantined_.insert(pid);return ok;};
        try{
            Require(ptrace(PT_ATTACH,static_cast<pid_t>(pid),nullptr,0)==0,"ptrace_attach_denied");attached=true;
            auto deadline=Clock::now()+std::chrono::seconds(2);int status=0;
            for(;;){pid_t got=waitpid(static_cast<pid_t>(pid),&status,WNOHANG|WUNTRACED);if(got==static_cast<pid_t>(pid)){if(WIFEXITED(status)||WIFSIGNALED(status)){attached=false;throw std::runtime_error("target_exited");}Require(WIFSTOPPED(status),"unexpected_wait_state");if(WSTOPSIG(status)!=SIGSTOP){preserveSignal=WSTOPSIG(status);throw std::runtime_error("unexpected_stop_signal");}break;}Require(got==0||(got<0&&errno==EINTR),"waitpid_failed");Require(Clock::now()<deadline,"ptrace_stop_timeout");usleep(1000);}
            const auto stopped=Process(pid);
            Require(Token(stopped,instance)==Token(p,instance),"stale_target");
            if(write)Require(Name(stopped)==Text(args,"expected_name",128),"process_identity_mismatch");
            Range(pid,address,length,write);
            auto io=[&](Bytes& bytes,int operation){ptrace_io_desc desc{};desc.piod_op=operation;desc.piod_offs=reinterpret_cast<void*>(static_cast<uintptr_t>(address));desc.piod_addr=bytes.data();desc.piod_len=bytes.size();Require(ptrace(PT_IO,static_cast<pid_t>(pid),reinterpret_cast<caddr_t>(&desc),0)==0&&desc.piod_len==bytes.size(),"ptrace_io_failed_or_partial");};
            Bytes before(length);io(before,PIOD_READ_D);
            json result={{"ok",true},{"pid",pid},{"target_id",Token(p,instance)},{"address",Hex(address)},{"length",length},{"effect","temporary_ptrace_stop"}};
            if(write){Require(before==Unhex(Text(args,"expected_hex",kMaxMemory*2)),"expected_bytes_mismatch");possiblyWritten=true;attempted=true;io(replacement,PIOD_WRITE_D);Bytes after(length);io(after,PIOD_READ_D);Require(after==replacement,"write_readback_mismatch");result["write_state"]="readback_matched";result["atomic"]=false;}
            else result["hex"]=ByteHex(before);
            Require(detach(),"ptrace_detach_failed");return result;
        }catch(const std::exception& e){bool clean=detach();return{{"ok",false},{"error",e.what()},{"write_state",possiblyWritten?"may_have_changed":"not_written"},{"target_may_be_stopped",!clean},{"retry_safe",false}};}
    }
};
} // namespace
std::unique_ptr<Backend> MakeNativeBackend(const AgentPolicy& policy){return std::make_unique<Native>(policy);}
} // namespace cortex::remote
#else
namespace cortex::remote {
std::unique_ptr<Backend> MakeNativeBackend(const AgentPolicy&){throw std::runtime_error("native_backend_requires_FreeBSD_or_PS5_build; no silent fixture fallback");}
}
#endif
