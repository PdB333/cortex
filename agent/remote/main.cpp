#include "backend.h"
#include "../../core/remote/tls.h"
#include <csignal>
#include <iostream>
#include <set>
#include <stdexcept>

namespace {
volatile std::sig_atomic_t stopRequested=0;
void Stop(int){stopRequested=1;}
}
int main(int argc,char** argv){
    using namespace cortex::remote;
    try{
        std::string path;bool fixture=false;
        for(int i=1;i<argc;++i){std::string arg=argv[i];if(arg=="--fixture"){fixture=true;continue;}if(arg=="--config"&&i+1<argc){path=argv[++i];continue;}if(arg=="--help"){std::cout<<"cortex-agent --config agent.json [--fixture]\nNative backend: FreeBSD/PS5 under existing OS permissions.\n--fixture explicitly uses a test vector and reports platform=fixture.\n";return 0;}throw std::runtime_error("unknown_option:"+arg);}
#ifdef CORTEX_REMOTE_PS5
        // An operator installs this file separately; it is never generated with
        // default credentials and is not supplied by a network caller.
        if(path.empty())path="/data/cortex/agent.json";
        Require(!fixture,"fixture_backend_not_allowed_in_PS5_build");
#endif
        auto config=ReadConfig(path,"cortex.remote.agent.v1");
        Keys(config,{"schema","listen","ca_certificate","certificate","private_key","allow_writes","allow_ptrace_stop","allowed_pids","timeout_ms","max_connections"});
        Endpoint endpoint;Require(ParseEndpoint(Text(config,"listen",32),endpoint,true),"invalid_listen_endpoint");
        int timeout=config.contains("timeout_ms")?static_cast<int>(UInt(config,"timeout_ms",100,15000)):5000;
        uint64_t maxConnections=config.contains("max_connections")?UInt(config,"max_connections",1,1000000):0;
        AgentPolicy policy;policy.allowWrites=Flag(config,"allow_writes");policy.allowPtraceStop=Flag(config,"allow_ptrace_stop");
        if(config.contains("allowed_pids")){
            Require(config["allowed_pids"].is_array()&&config["allowed_pids"].size()<=256,"invalid_pid_allowlist");std::set<uint32_t> unique;
            for(const auto& p:config["allowed_pids"]){auto n=static_cast<uint32_t>(UInt(json{{"pid",p}},"pid",1,2147483647));Require(unique.insert(n).second,"duplicate_allowed_pid");policy.allowedPids.push_back(n);}
        }
        if(!fixture)Require(!policy.allowedPids.empty(),"native_pid_allowlist_required");
#ifdef CORTEX_REMOTE_PS5
        auto backend=MakeNativeBackend(policy);
#else
        auto backend=fixture?MakeFixtureBackend(policy):MakeNativeBackend(policy);
#endif
        TlsContext tls(ReadTlsFiles(path,config),true);auto instance=tls.RandomId();Listener listener(endpoint);
        std::signal(SIGINT,Stop);std::signal(SIGTERM,Stop);
        std::cout<<json{{"ready",true},{"protocol","cortex-remote"},{"version",1},{"platform",backend->Platform()},{"listen",Text(config,"listen",32)},{"instance",instance},{"capabilities",backend->Capabilities()}}.dump()<<std::endl;
        uint64_t count=0;
        while(!stopRequested&&(!maxConnections||count<maxConnections)){
            try{
                auto channel=listener.Accept(tls,timeout);if(!channel)continue;++count;
                channel->Handshake();const auto session=tls.RandomId();
                json hello={{"schema","cortex.remote.hello.v1"},{"protocol","cortex-remote"},{"version",1},{"session",session},{"instance",instance},{"platform",backend->Platform()},{"capabilities",backend->Capabilities()}};
                channel->Send(FrameKind::Hello,0,hello);
                auto request=channel->Receive(FrameKind::Request,1);
                Keys(request,{"schema","session","instance","action","arguments"});
                Require(Text(request,"schema",64)=="cortex.remote.request.v1","invalid_request_schema");
                Require(Text(request,"session",32)==session&&Text(request,"instance",32)==instance,"request_session_mismatch");
                auto action=Text(request,"action",32);Require(request.contains("arguments"),"missing_arguments");
                json result;
                try{ValidateArguments(action,request["arguments"]);result=backend->Execute(action,request["arguments"],instance);}
                catch(const std::exception& e){result={{"ok",false},{"error",e.what()},{"write_state","not_written"},{"retry_safe",false}};}
                if(action=="write"){
                    if(!result.contains("write_state"))result["write_state"]="may_have_changed";
                    result["retry_safe"]=false;
                }
                // No memory bytes, keys, file paths or target-provided text in
                // the audit log. Identity is the authenticated cert fingerprint.
                std::cerr<<json{{"event","request"},{"peer",channel->PeerFingerprint()},{"session",session},{"action",action},{"ok",result.value("ok",false)}}.dump()<<'\n';
                channel->Send(FrameKind::Response,1,{{"schema","cortex.remote.response.v1"},{"session",session},{"instance",instance},{"result",result}});
                // Exactly one request per TLS connection: no repeated sequence,
                // implicit replay, remotely adjustable endpoint or retry cache.
            }catch(const std::exception& e){std::cerr<<json{{"event","connection_rejected"},{"error",e.what()}}.dump(-1,' ',false,json::error_handler_t::replace)<<'\n';}
        }
        return 0;
    }catch(const std::exception& e){std::cerr<<json{{"ready",false},{"error",e.what()}}.dump(-1,' ',false,json::error_handler_t::replace)<<'\n';return 1;}
}
