// Pure Cortex Remote decoder fuzzing: no sockets, credential files or process I/O.
#include "../../core/remote/protocol.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data,size_t size){
    using namespace cortex::remote;
    if(size>kMaxFrame+16)return 0;
    if(size>=16){
        std::array<uint8_t,16> header{};std::copy(data,data+16,header.begin());
        try{
            auto decoded=DecodeHeader(header);
            if(decoded.size==size-16){
                auto value=ParseJson(std::string(reinterpret_cast<const char*>(data+16),size-16));
                if(value.contains("action")&&value["action"].is_string()&&value.contains("arguments"))
                    ValidateArguments(value["action"].get<std::string>(),value["arguments"]);
                (void)DumpJson(value);
            }
        }catch(const std::exception&){}
    }
    if(size){
        try{(void)ParseJson(std::string(reinterpret_cast<const char*>(data),size));}catch(const std::exception&){}
        try{Endpoint endpoint;(void)ParseEndpoint(std::string(reinterpret_cast<const char*>(data),std::min<size_t>(size,40)),endpoint);}catch(const std::exception&){}
    }
    return 0;
}
