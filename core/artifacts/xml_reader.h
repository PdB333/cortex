#pragma once

// Purpose-limited XML reader for package metadata. No DTD, entity declaration,
// external resolution, schema fetching or execution. UTF-8/UTF-16 BOM input;
// predefined/numeric character references; namespace-aware element identities.
// This is a bounded metadata reader, not a general XML validating processor.
#include "bounded_reader.h"
#include <cctype>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace cortex::artifacts::xml {
using bounded::Error;
struct Node {
    size_t parent = size_t(-1);
    std::string qname, local, ns, text;
    std::map<std::string, std::string> attributes;
    std::string Attr(const std::string& key) const {
        const auto it=attributes.find(key);return it==attributes.end()?"":it->second;
    }
};

class Document {
public:
    explicit Document(const std::vector<uint8_t>& input) {
        if(input.empty())throw Error("xml_empty_document");
        if(input.size()>(1u<<20))throw Error("xml_input_limit");
        if(input.size()>=2 && ((input[0]==0xff&&input[1]==0xfe)||(input[0]==0xfe&&input[1]==0xff)))
            source_=bounded::Utf16(bounded::View(input).Slice(2,input.size()-2),input[0]==0xff);
        else{
            const size_t start=input.size()>=3&&input[0]==0xef&&input[1]==0xbb&&input[2]==0xbf?3:0;
            source_.assign(reinterpret_cast<const char*>(input.data()+start),input.size()-start);
            if(bounded::Utf8(reinterpret_cast<const uint8_t*>(source_.data()),source_.size())!=source_)
                throw Error("xml_invalid_utf8");
        }
        for(unsigned char c:source_)if(c<32&&c!=9&&c!=10&&c!=13)throw Error("xml_invalid_control_character");
        Parse();
    }
    std::vector<Node> nodes;
private:
    struct Frame { size_t index;std::map<std::string,std::string> namespaces; };
    bool Is(const char* value)const{return source_.compare(at_,std::char_traits<char>::length(value),value)==0;}
    void Need(char c){if(at_>=source_.size()||source_[at_++]!=c)throw Error("xml_expected_delimiter");}
    void Space(){while(at_<source_.size()&&(source_[at_]==' '||source_[at_]=='\t'||source_[at_]=='\n'||source_[at_]=='\r'))++at_;}
    std::string Name(){
        const size_t start=at_;
        if(at_>=source_.size())throw Error("xml_name_missing");
        auto first=[](unsigned char c){return std::isalpha(c)||c=='_'||c==':'||c>=128;};
        auto rest=[&](unsigned char c){return first(c)||std::isdigit(c)||c=='-'||c=='.';};
        if(!first(source_[at_++]))throw Error("xml_name_invalid");
        while(at_<source_.size()&&rest(source_[at_]))++at_;
        if(at_-start>256)throw Error("xml_name_limit");
        const std::string text=source_.substr(start,at_-start);
        const auto colon=text.find(':');
        if(colon!=std::string::npos&&(colon==0||colon+1==text.size()||text.find(':',colon+1)!=std::string::npos))throw Error("xml_qname_invalid");
        return text;
    }
    std::string Entities(const std::string& raw){
        std::string out;
        for(size_t i=0;i<raw.size();++i){
            if(raw[i]!='&'){out+=raw[i];continue;}
            const size_t end=raw.find(';',i+1);
            if(end==std::string::npos||end-i>16)throw Error("xml_entity_invalid");
            const auto entity=raw.substr(i+1,end-i-1);i=end;
            if(entity=="amp")out+='&';else if(entity=="lt")out+='<';else if(entity=="gt")out+='>';
            else if(entity=="quot")out+='"';else if(entity=="apos")out+='\'';
            else if(!entity.empty()&&entity[0]=='#'){
                const bool hex=entity.size()>1&&entity[1]=='x';size_t p=hex?2:1;
                if(p==entity.size())throw Error("xml_numeric_entity_empty");
                uint32_t value=0;
                for(;p<entity.size();++p){
                    const char c=entity[p];const int digit=c>='0'&&c<='9'?c-'0':hex&&c>='A'&&c<='F'?c-'A'+10:hex&&c>='a'&&c<='f'?c-'a'+10:-1;
                    if(digit<0||digit>=(hex?16:10)||value>0x10ffffu/(hex?16:10))throw Error("xml_numeric_entity_invalid");
                    value=value*(hex?16:10)+digit;
                }
                if(value>0x10ffff||(value>=0xd800&&value<=0xdfff)||value==0xfffe||value==0xffff||
                   (value<32&&value!=9&&value!=10&&value!=13))throw Error("xml_numeric_entity_invalid");
                bounded::AppendCodePoint(out,value);
            }else throw Error("xml_declared_entities_not_supported");
        }
        return out;
    }
    std::string Quoted(){
        if(at_>=source_.size()||(source_[at_]!='\''&&source_[at_]!='"'))throw Error("xml_attribute_quote_missing");
        const char quote=source_[at_++];const size_t start=at_;
        while(at_<source_.size()&&source_[at_]!=quote){if(source_[at_]=='<')throw Error("xml_attribute_contains_markup");++at_;}
        if(at_==source_.size()||at_-start>65536)throw Error("xml_attribute_limit_or_unterminated");
        const std::string result=Entities(source_.substr(start,at_-start));++at_;return result;
    }
    static std::string Prefix(const std::string& name){const auto p=name.find(':');return p==std::string::npos?"":name.substr(0,p);}
    static std::string Local(const std::string& name){const auto p=name.find(':');return p==std::string::npos?name:name.substr(p+1);}
    void Text(std::string text){
        textBytes_+=text.size();if(textBytes_>(2u<<20))throw Error("xml_text_budget");
        if(stack_.empty()){
            for(unsigned char c:text)if(c!=' '&&c!='\t'&&c!='\r'&&c!='\n')throw Error("xml_text_outside_root");
        }else nodes[stack_.back().index].text+=text;
    }
    void Parse(){
        size_t attributes=0;
        while(at_<source_.size()){
            if(source_[at_]!='<'){
                const size_t start=at_,end=source_.find('<',at_);at_=end==std::string::npos?source_.size():end;
                const auto raw=source_.substr(start,at_-start);if(raw.find("]]>")!=std::string::npos)throw Error("xml_cdata_close_in_text");
                Text(Entities(raw));continue;
            }
            if(Is("<!--")){
                const size_t end=source_.find("-->",at_+4);if(end==std::string::npos)throw Error("xml_comment_unterminated");
                if(source_.find("--",at_+4)!=end)throw Error("xml_comment_invalid");at_=end+3;continue;
            }
            if(Is("<?")){
                const size_t end=source_.find("?>",at_+2);if(end==std::string::npos)throw Error("xml_processing_instruction_unterminated");
                at_=end+2;continue;
            }
            if(Is("<![CDATA[")){
                if(stack_.empty())throw Error("xml_cdata_outside_root");
                const size_t end=source_.find("]]>",at_+9);if(end==std::string::npos)throw Error("xml_cdata_unterminated");
                Text(source_.substr(at_+9,end-at_-9));at_=end+3;continue;
            }
            if(Is("<!"))throw Error("xml_dtd_or_declaration_forbidden");
            if(Is("</")){
                at_+=2;const auto name=Name();Space();Need('>');
                if(stack_.empty()||nodes[stack_.back().index].qname!=name)throw Error("xml_closing_tag_mismatch");
                stack_.pop_back();continue;
            }
            Need('<');Node node;node.qname=Name();node.local=Local(node.qname);
            if(nodes.size()>=4096||stack_.size()>=64)throw Error("xml_node_or_depth_limit");
            if(stack_.empty()&&!nodes.empty())throw Error("xml_multiple_roots");
            std::map<std::string,std::string> ns=stack_.empty()?std::map<std::string,std::string>{{"xml","http://www.w3.org/XML/1998/namespace"}}:stack_.back().namespaces;
            std::map<std::string,std::string> raw;
            bool empty=false;
            while(true){
                const size_t previous=at_;Space();
                if(Is("/>")){at_+=2;empty=true;break;}if(Is(">")){++at_;break;}
                if(at_==previous)throw Error("xml_attribute_separator_missing");
                const auto key=Name();Space();Need('=');Space();const auto value=Quoted();
                if(++attributes>32768||raw.size()>=256||!raw.emplace(key,value).second)throw Error("xml_attribute_duplicate_or_limit");
                if(key=="xmlns"||Prefix(key)=="xmlns"){
                    const std::string prefix=key=="xmlns"?"":Local(key);
                    if(prefix=="xmlns"||(prefix=="xml"&&value!="http://www.w3.org/XML/1998/namespace"))throw Error("xml_reserved_namespace");
                    ns[prefix]=value;if(ns.size()>256)throw Error("xml_namespace_limit");
                }
            }
            const auto prefix=Prefix(node.qname);const auto it=ns.find(prefix);
            if(it==ns.end()&&!prefix.empty())throw Error("xml_element_prefix_undeclared");
            node.ns=it==ns.end()?"":it->second;
            std::set<std::pair<std::string,std::string>> expanded;
            for(const auto& [key,value]:raw){
                if(key=="xmlns"||Prefix(key)=="xmlns")continue;
                const auto p=Prefix(key);const auto n=ns.find(p);
                if(!p.empty()&&n==ns.end())throw Error("xml_attribute_prefix_undeclared");
                const std::string uri=p.empty()?"":n->second;
                if(!expanded.emplace(uri,Local(key)).second)throw Error("xml_attribute_expanded_duplicate");
                node.attributes.emplace(key,value);
            }
            node.parent=stack_.empty()?size_t(-1):stack_.back().index;
            nodes.push_back(std::move(node));
            if(!empty)stack_.push_back({nodes.size()-1,std::move(ns)});
        }
        if(nodes.empty()||!stack_.empty())throw Error("xml_incomplete_document");
    }
    std::string source_;size_t at_=0,textBytes_=0;std::vector<Frame> stack_;
};
} // namespace cortex::artifacts::xml
