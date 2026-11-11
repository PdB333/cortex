#include "client.h"
#include "tls.h"
#include <chrono>
#include <iostream>
#include <mutex>
#include <stdexcept>

namespace cortex::remote {
namespace {
void Id(const std::string& id){Require(id.size()==32&&id.find_first_not_of("0123456789abcdef")==std::string::npos,"invalid_session_id");}

void ValidateResult(const std::string& action,const json& args,const json& result){
    Require(result.is_object()&&result.contains("ok")&&result["ok"].is_boolean(),"invalid_response");
    if(!result["ok"].get<bool>()){Text(result,"error",1024);return;}
    if(action=="info"){Require(result.contains("capabilities")&&result["capabilities"].is_object(),"invalid_capabilities");return;}
    size_t limit=args.contains("limit")?static_cast<size_t>(UInt(args,"limit",1,256)):256;
    if(action=="processes"){
        Require(result.contains("processes")&&result["processes"].is_array()&&result["processes"].size()<=limit,"invalid_process_list");
        for(const auto& row:result["processes"]){UInt(row,"pid",1,2147483647);Text(row,"name",128);Text(row,"target_id",256);}
        return;
    }
    Require(UInt(result,"pid",1,2147483647)==UInt(args,"pid",1,2147483647)&&Text(result,"target_id",256)==Text(args,"target_id",256),"response_target_mismatch");
    if(action=="maps"){
        Require(result.contains("maps")&&result["maps"].is_array()&&result["maps"].size()<=limit,"invalid_map_list");
        for(const auto& row:result["maps"]){
            auto first=Number(Text(row,"start",20)),last=Number(Text(row,"end",20));
            Require(last>first&&last-1<=kMaxUserAddress,"invalid_memory_map");
            Require(row.contains("name")&&row["name"].is_string()&&row["name"].get_ref<const std::string&>().size()<=1024,"invalid_map_name");
            Flag(row,"read");Flag(row,"write");Flag(row,"execute");
        }
        return;
    }
    Require(Number(Text(result,"address",20))==Number(Text(args,"address",20)),"response_address_mismatch");
    size_t expected=action=="read"?static_cast<size_t>(UInt(args,"length",1,kMaxMemory)):Unhex(Text(args,"hex",kMaxMemory*2)).size();
    Require(UInt(result,"length",1,kMaxMemory)==expected,"response_length_mismatch");
    if(action=="read")Require(Unhex(Text(result,"hex",kMaxMemory*2)).size()==expected,"response_length_mismatch");
}
}
json RemoteRequest(const std::string& configPath,const std::string& action,const json& args,bool operatorAllowsWrites){
    // The vendored PSA crypto state is shared; serialize calls in one process.
    static std::mutex requests;std::lock_guard<std::mutex> guard(requests);
    bool mayHaveWritten=false;std::string stage="validate";
    try{
        ValidateArguments(action,args);
        if(action=="write")Require(operatorAllowsWrites,"write_authority_required");
        auto c=ReadConfig(configPath,"cortex.remote.client.v1");
        Keys(c,{"schema","endpoint","server_name","ca_certificate","certificate","private_key","expected_platform","allow_writes","timeout_ms"});
        Endpoint endpoint;Require(ParseEndpoint(Text(c,"endpoint",32),endpoint),"invalid_endpoint");
        const auto serverName=Text(c,"server_name",253);const auto platform=Text(c,"expected_platform",32);
        Require(platform=="ps5"||platform=="freebsd"||platform=="fixture","invalid_expected_platform");
        if(action=="write")Require(Flag(c,"allow_writes"),"client_config_read_only");
        int timeout=c.contains("timeout_ms")?static_cast<int>(UInt(c,"timeout_ms",100,15000)):5000;
        stage="credentials";TlsContext context(ReadTlsFiles(configPath,c),false);
        stage="connect";auto socket=Channel::Connect(context,endpoint,serverName,timeout);
        stage="hello";auto hello=socket->Receive(FrameKind::Hello,0);
        Keys(hello,{"schema","protocol","version","session","instance","platform","capabilities"});
        Require(Text(hello,"schema",64)=="cortex.remote.hello.v1"&&Text(hello,"protocol",32)=="cortex-remote"&&UInt(hello,"version",1,1)==1,"unsupported_protocol_version");
        Require(Text(hello,"platform",32)==platform,"unexpected_remote_platform");
        auto session=Text(hello,"session",32),instance=Text(hello,"instance",32);Id(session);Id(instance);
        Require(hello.contains("capabilities")&&hello["capabilities"].is_object(),"invalid_capabilities");
        if(action=="write")Require(Flag(hello["capabilities"],"memory_write"),"agent_read_only");
        json request={{"schema","cortex.remote.request.v1"},{"session",session},{"instance",instance},{"action",action},{"arguments",args}};
        stage="request";mayHaveWritten=action=="write";
        socket->Send(FrameKind::Request,1,request);
        stage="response";auto response=socket->Receive(FrameKind::Response,1);
        Keys(response,{"schema","session","instance","result"});
        Require(Text(response,"schema",64)=="cortex.remote.response.v1"&&Text(response,"session",32)==session&&Text(response,"instance",32)==instance,"response_session_mismatch");
        Require(response.contains("result")&&response["result"].is_object(),"invalid_response");auto result=response["result"];
        ValidateResult(action,args,result);
        if(action=="write"){
            const auto state=Text(result,"write_state",32);
            Require(state=="not_written"||state=="may_have_changed"||state=="readback_matched","invalid_write_state");
            if(result["ok"].get<bool>())Require(state=="readback_matched","invalid_write_state");
            // The client never recommends retrying an attempted write, even on
            // an authenticated refusal. A new operator-approved request is distinct.
            result["retry_safe"]=false;
        }
        result["protocol"]="cortex-remote";result["protocol_version"]=1;result["platform"]=platform;
        result["instance"]=instance;result["peer_certificate_sha256"]=socket->PeerFingerprint();
        return result;
    }catch(const std::exception& e){return{{"ok",false},{"error",e.what()},{"stage",stage},{"write_state",mayHaveWritten?"may_have_changed":"not_written"},{"retry_safe",action!="write"}};}
}
int RemoteMain(int argc,char** argv){
    try{
        if(argc<2||std::string(argv[1])=="--help"){
            std::cerr<<"Cortex Remote (own protocol, mutual TLS)\n"
                "usage: cortex ps5 info|processes|maps|read|write --config client.json\n"
                "  processes: [--limit 1..256]\n"
                "  maps: --pid PID --target-id ID [--limit N]\n"
                "  read: --pid PID --target-id ID --address 0xADDRESS --length 1..65536\n"
                "  write: --pid PID --target-id ID --address 0xADDRESS --hex HEX\n"
                "         --expected-hex HEX --expected-name NAME --allow-writes --confirm-write\n"
                "The target ID comes from processes and changes after agent/process restart.\n"
                "Both client and agent configs must permit writes. No third-party protocol.\n";
            return argc<2?2:0;
        }
        std::string action=argv[1],config;json args=json::object();bool writes=false;
        for(int i=2;i<argc;++i){std::string key=argv[i];
            if(key=="--allow-writes"){writes=true;continue;}
            if(key=="--confirm-write"){args["mutation_permission"]=true;continue;}
            Require(i+1<argc,"missing_option_value");std::string value=argv[++i];
            if(key=="--config")config=value;
            else if(key=="--pid"||key=="--length"||key=="--limit")args[key.substr(2)]=Number(value);
            else if(key=="--address"||key=="--hex")args[key.substr(2)]=value;
            else if(key=="--target-id")args["target_id"]=value;
            else if(key=="--expected-hex")args["expected_hex"]=value;
            else if(key=="--expected-name")args["expected_name"]=value;
            else throw std::runtime_error("unknown_option:"+key);
        }
        auto result=RemoteRequest(config,action,args,writes);std::cout<<result.dump(2,' ',false,json::error_handler_t::replace)<<'\n';return result.value("ok",false)?0:1;
    }catch(const std::exception& e){std::cout<<json{{"ok",false},{"error",e.what()}}.dump()<<'\n';return 2;}
}
} // namespace cortex::remote
