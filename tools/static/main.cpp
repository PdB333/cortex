#include "artifacts/artifact.h"
#include <iostream>
#include <limits>
#include <string>

// A small platform-neutral entry point for the same artifact registry used
// by Cortex's GUI/CLI/MCP. It does not provide a process backend or debugger.
int main(int argc,char** argv){
    using namespace cortex::artifacts;
    try{
        std::string path,child;bool list=false;InspectOptions options;
        for(int i=1;i<argc;++i){
            const std::string arg=argv[i];
            if(arg=="--help"){
                std::cout<<"cortex-inspect FILE [--children] [--child MEMBER] [--max N]\n"
                         <<"Static reads only. No target execution, installation or live debugger.\n";return 0;
            }
            if(arg=="--children"){list=true;continue;}
            if(arg=="--child"||arg=="--max"){
                if(++i==argc)throw std::runtime_error("missing option value");
                if(arg=="--child")child=argv[i];
                else{
                    const std::string value=argv[i];size_t count=0;
                    if(value.empty()||value.find_first_not_of("0123456789")!=std::string::npos)throw std::runtime_error("invalid max count");
                    const auto n=std::stoull(value,&count);if(count!=value.size()||n>65536)throw std::runtime_error("max count must be 0..65536");
                    options.maxEntries=static_cast<size_t>(n);
                }
                continue;
            }
            if(!arg.empty()&&arg[0]=='-')throw std::runtime_error("unknown option: "+arg);
            if(!path.empty())throw std::runtime_error("only one file is accepted");path=arg;
        }
        if(path.empty())throw std::runtime_error("usage: cortex-inspect FILE [--children] [--child MEMBER] [--max N]");
        std::string error;auto source=OpenFile(path,&error);
        if(!source)throw std::runtime_error(error);
        const auto& registry=DefaultRegistry();
        if(!child.empty()){
            source=registry.OpenChild(source,child,error,options);
            if(!source)throw std::runtime_error(error);
        }
        const json result=list?registry.ListChildren(*source,options):registry.Inspect(*source,options);
        std::cout<<result.dump(2)<<'\n';return result.value("ok",false)?0:1;
    }catch(const std::exception& ex){
        const auto result=cortex::artifacts::json{{"ok",false},{"error",ex.what()}};
        std::cerr<<result.dump(-1,' ',false,cortex::artifacts::json::error_handler_t::replace)<<'\n';return 2;
    }
}
