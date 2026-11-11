#include "protocol.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>

namespace cortex::remote {
void Require(bool value, const char* error) { if (!value) throw std::runtime_error(error); }
uint64_t Number(const std::string& text) {
    Require(!text.empty() && text.size() <= 20, "invalid_number");
    size_t at = 0; unsigned base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) { at = 2; base = 16; }
    uint64_t value = 0;
    for (; at < text.size(); ++at) {
        const unsigned char c = static_cast<unsigned char>(text[at]);
        unsigned digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 99;
        Require(digit < base && value <= (std::numeric_limits<uint64_t>::max() - digit) / base, "invalid_number");
        value = value * base + digit;
    }
    return value;
}
bool ParseEndpoint(const std::string& text, Endpoint& out, bool listening) {
    try {
        if (text.empty() || text.size() > 21) return false;
        size_t colon = text.find(':'); if (colon == std::string::npos || text.find(':', colon + 1) != std::string::npos) return false;
        std::string host = text.substr(0, colon), port = text.substr(colon + 1);
        unsigned count = 0, first = 0; size_t begin = 0;
        while (begin < host.size()) {
            size_t end = host.find('.', begin); std::string part = host.substr(begin, end == std::string::npos ? end : end - begin);
            if (part.empty() || part.size() > 3 || (part.size() > 1 && part[0] == '0') || part.find_first_not_of("0123456789") != std::string::npos) return false;
            auto v = Number(part); if (v > 255) return false; if (count == 0) first = static_cast<unsigned>(v); ++count;
            if (end == std::string::npos) break;
            begin = end + 1; if (begin == host.size()) return false;
        }
        if (count != 4 || first >= 224 || (first == 0 && !(listening && host == "0.0.0.0"))) return false;
        if (port.empty() || port.find_first_not_of("0123456789") != std::string::npos) return false;
        auto p = Number(port); if (p == 0 || p > 65535) return false;
        out = {host, static_cast<uint16_t>(p)}; return true;
    } catch (...) { return false; }
}
std::array<uint8_t, 16> EncodeHeader(FrameKind kind, uint32_t id, uint32_t size) {
    Require(size > 0 && size <= kMaxFrame, "frame_size_limit");
    std::array<uint8_t, 16> h{'C','R','T','X',1,static_cast<uint8_t>(kind),0,0};
    for (int i = 0; i < 4; ++i) { h[8+i] = static_cast<uint8_t>(id >> (24-8*i)); h[12+i] = static_cast<uint8_t>(size >> (24-8*i)); }
    return h;
}
Header DecodeHeader(const std::array<uint8_t, 16>& h) {
    Require(h[0]=='C' && h[1]=='R' && h[2]=='T' && h[3]=='X', "bad_frame_magic");
    Require(h[4] == 1, "unsupported_protocol_version");
    Require(h[5] >= 1 && h[5] <= 3 && h[6] == 0 && h[7] == 0, "invalid_frame_header");
    uint32_t id=0,size=0;
    for (int i=0;i<4;++i){id=(id<<8)|h[8+i];size=(size<<8)|h[12+i];}
    Require(size > 0 && size <= kMaxFrame, "frame_size_limit");
    return {static_cast<FrameKind>(h[5]),id,size};
}
json ParseJson(const std::string& text) {
    Require(!text.empty() && text.size() <= kMaxFrame, "json_size_limit");
    bool quoted=false, escaped=false; unsigned depth=0;
    for (char c:text) {
        if (quoted) { if(escaped) escaped=false; else if(c=='\\') escaped=true; else if(c=='"') quoted=false; continue; }
        if(c=='"') quoted=true;
        else if(c=='{'||c=='['){Require(++depth<=32,"json_depth_limit");}
        else if(c=='}'||c==']'){Require(depth>0,"invalid_json");--depth;}
    }
    Require(!quoted && depth==0,"invalid_json");
    std::vector<std::set<std::string>> keys;
    auto callback=[&](int, json::parse_event_t event, json& value) {
        if(event==json::parse_event_t::object_start) keys.emplace_back();
        if(event==json::parse_event_t::key){Require(!keys.empty(),"invalid_json");Require(keys.back().insert(value.get<std::string>()).second,"duplicate_json_key");}
        if(event==json::parse_event_t::object_end){Require(!keys.empty(),"invalid_json");keys.pop_back();}
        return true;
    };
    json result;
    try { result=json::parse(text,callback); }
    catch(const json::exception&) { throw std::runtime_error("invalid_json"); }
    Require(result.is_object(),"object_required");return result;
}
std::string DumpJson(const json& value) {
    auto result=value.dump(-1,' ',false,json::error_handler_t::replace);
    Require(result.size()<=kMaxFrame,"response_size_limit");return result;
}
std::string ReadFile(const std::string& path,size_t maxBytes) {
    std::ifstream input(std::filesystem::u8path(path),std::ios::binary);
    Require(bool(input),"file_open_failed");
    std::string out;out.resize(maxBytes+1);input.read(out.data(),static_cast<std::streamsize>(out.size()));
    auto n=input.gcount();Require(n>=0&&static_cast<size_t>(n)<=maxBytes,"file_size_limit");Require(!input.bad(),"file_read_failed");out.resize(static_cast<size_t>(n));return out;
}
std::string Text(const json& o,const char* k,size_t max) {
    Require(o.is_object()&&o.contains(k)&&o[k].is_string(),"string_argument_required");auto s=o[k].get<std::string>();
    Require(!s.empty()&&s.size()<=max&&s.find('\0')==std::string::npos,"invalid_string");return s;
}
uint64_t UInt(const json& o,const char* k,uint64_t low,uint64_t high) {
    Require(o.is_object()&&o.contains(k)&&o[k].is_number_integer(),"integer_argument_required");
    if(!o[k].is_number_unsigned())Require(o[k].get<int64_t>()>=0,"negative_integer");
    auto n=o[k].get<uint64_t>();Require(n>=low&&n<=high,"integer_out_of_range");return n;
}
bool Flag(const json& o,const char* k,bool fallback) {
    if(!o.contains(k))return fallback;Require(o[k].is_boolean(),"boolean_argument_required");return o[k].get<bool>();
}
void Keys(const json& o,std::initializer_list<const char*> allowed) {
    Require(o.is_object(),"object_required");std::set<std::string> names;for(auto k:allowed)names.insert(k);
    for(auto it=o.begin();it!=o.end();++it)Require(names.count(it.key())>0,"unknown_argument");
}
json ReadConfig(const std::string& path,const char* schema) {
    Require(!path.empty(),"remote_config_not_configured");auto c=ParseJson(ReadFile(path,65536));Require(Text(c,"schema",64)==schema,"unsupported_config_schema");return c;
}
std::string ConfigPath(const std::string& config,const std::string& value) {
    auto p=std::filesystem::u8path(value);if(p.is_relative())p=std::filesystem::u8path(config).parent_path()/p;return p.lexically_normal().u8string();
}
TlsFiles ReadTlsFiles(const std::string& path,const json& c){return{ConfigPath(path,Text(c,"ca_certificate")),ConfigPath(path,Text(c,"certificate")),ConfigPath(path,Text(c,"private_key"))};}
std::string Hex(uint64_t n){char b[24];std::snprintf(b,sizeof(b),"0x%llx",static_cast<unsigned long long>(n));return b;}
std::string ByteHex(const Bytes& bytes){static const char d[]="0123456789abcdef";std::string s;s.reserve(bytes.size()*2);for(auto b:bytes){s+=d[b>>4];s+=d[b&15];}return s;}
Bytes Unhex(const std::string& s){Require(!s.empty()&&s.size()<=kMaxMemory*2&&s.size()%2==0,"invalid_hex_length");auto digit=[](char c){return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;};Bytes b;b.reserve(s.size()/2);for(size_t i=0;i<s.size();i+=2){int a=digit(s[i]),v=digit(s[i+1]);Require(a>=0&&v>=0,"invalid_hex");b.push_back(static_cast<uint8_t>(a*16+v));}return b;}
void ValidateArguments(const std::string& action,const json& a) {
    Require(action=="info"||action=="processes"||action=="maps"||action=="read"||action=="write","unknown_action");
    if(action=="info")Keys(a,{});
    if(action=="processes")Keys(a,{"limit"});
    if(action=="maps")Keys(a,{"pid","target_id","limit"});
    if(action=="read")Keys(a,{"pid","target_id","address","length"});
    if(action=="write")Keys(a,{"pid","target_id","address","hex","expected_hex","expected_name","mutation_permission"});
    if(a.contains("limit"))UInt(a,"limit",1,256);
    if(action=="maps"||action=="read"||action=="write"){UInt(a,"pid",1,2147483647);Text(a,"target_id",256);}
    if(action=="read"||action=="write"){
        auto addr=Number(Text(a,"address",20));size_t len=0;
        if(action=="read")len=static_cast<size_t>(UInt(a,"length",1,kMaxMemory));
        else{auto b=Unhex(Text(a,"hex",kMaxMemory*2));auto expected=Unhex(Text(a,"expected_hex",kMaxMemory*2));Require(b.size()==expected.size(),"expected_length_mismatch");len=b.size();Text(a,"expected_name",128);Require(Flag(a,"mutation_permission"),"mutation_permission_required");}
        Require(addr<=kMaxUserAddress&&len-1<=kMaxUserAddress-addr,"address_out_of_user_range");
    }
}
} // namespace cortex::remote
