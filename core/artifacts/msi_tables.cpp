#include "compound_reader.h"
#include <algorithm>
#include <map>
#include <set>

// MSI is a database stored in CFB streams, not an executable to invoke.
// Integer bias, packed stream names and column-major storage are format
// facts cross-checked against Wine/ReactOS MSI code and Microsoft-authored
// fixtures. No MSI/COM dependency is linked by this parser.
namespace cortex::artifacts::compound {
namespace {
using bounded::View;
using bounded::Error;
constexpr uint64_t kReadLimit = 64ull << 20;
constexpr uint64_t kTextLimit = 16ull << 20;
constexpr uint32_t kMaxStrings = 1u << 20;

std::string DecodeText(const View& bytes, uint32_t codepage) {
    bool ascii = true;
    for(size_t i=0;i<bytes.Size();++i)ascii &= bytes.U8(i)<128;
    if(codepage==1200)return bounded::Utf16(bytes);
    if(ascii || codepage==65001)return bounded::Utf8(bytes.Data(0,bytes.Size()),bytes.Size());
    if(codepage!=1252 && codepage!=28591)throw Error("msi_string_codepage_unsupported:"+std::to_string(codepage));
    static const uint16_t cp1252[32]={0x20AC,0xFFFD,0x201A,0x0192,0x201E,0x2026,0x2020,0x2021,
        0x02C6,0x2030,0x0160,0x2039,0x0152,0xFFFD,0x017D,0xFFFD,0xFFFD,0x2018,0x2019,0x201C,
        0x201D,0x2022,0x2013,0x2014,0x02DC,0x2122,0x0161,0x203A,0x0153,0xFFFD,0x017E,0x0178};
    std::string out;
    for(size_t i=0;i<bytes.Size();++i){
        const uint8_t c=bytes.U8(i);
        bounded::AppendCodePoint(out,codepage==1252 && c>=128 && c<160?cp1252[c-128]:c);
    }
    return out;
}

struct Column { uint16_t number=0,type=0; std::string name; unsigned width=0; };
class Database {
public:
    Database(const File& file,const InspectOptions& options) : file_(file),limit_(std::min<size_t>(options.maxEntries,4096)) {
        const View pool=Load("_StringPool",true), data=Load("_StringData",true);
        if(pool.Size()<4 || pool.Size()%4 || pool.Size()/4>kMaxStrings)throw Error("msi_string_pool_size_invalid");
        codepage_=pool.U16(0)|(uint32_t(pool.U16(2)&0x7FFF)<<16);
        strWidth_=(pool.U16(2)&0x8000)?3:2;
        strings_.push_back(""); uint64_t offset=0,decoded=0;
        for(uint64_t at=4;at<pool.Size();) {
            uint64_t length=pool.U16(at); const uint16_t refs=pool.U16(at+2);
            if(length==0 && refs==0){strings_.push_back("");at+=4;continue;}
            if(length==0){length=pool.U32(at+4);at+=8;}else at+=4;
            if(length>(1u<<20))throw Error("msi_string_length_limit");
            std::string text=DecodeText(data.Slice(offset,length),codepage_);offset+=length;
            decoded+=text.size();if(decoded>kTextLimit)throw Error("msi_string_text_budget");
            strings_.push_back(std::move(text));
        }
        if(offset!=data.Size())warnings_.push_back("string_data_has_unreferenced_bytes");
        const View tables=Load("_Tables",true);
        if(tables.Size()%strWidth_ || tables.Size()/strWidth_>4096)throw Error("msi_tables_catalog_invalid");
        std::set<std::string> unique;
        for(size_t at=0;at<tables.Size();at+=strWidth_){
            const std::string name=String(UInt(tables,at,strWidth_));
            if(name.empty() || name.size()>256 || !unique.insert(name).second)throw Error("msi_table_name_invalid_or_duplicate");
            names_.push_back(name);
        }
        const View columns=Load("_Columns",true);
        const uint32_t rowSize=2*strWidth_+4;
        if(columns.Size()%rowSize || columns.Size()/rowSize>65536)throw Error("msi_columns_catalog_invalid");
        const size_t rows=columns.Size()/rowSize;
        for(size_t row=0;row<rows;++row){
            const std::string table=String(UInt(columns,row*strWidth_,strWidth_));
            const uint16_t number=columns.U16(uint64_t(strWidth_)*rows+row*2)^0x8000;
            const std::string name=String(UInt(columns,uint64_t(strWidth_+2)*rows+row*strWidth_,strWidth_));
            const uint16_t type=columns.U16(uint64_t(2*strWidth_+2)*rows+row*2)^0x8000;
            if(number==0 || number>256 || table.empty() || name.empty() || table.size()>256 || name.size()>256 || !(type&0x100))
                throw Error("msi_column_definition_invalid");
            const bool binary=(type&0xFFF)==0x900;
            const unsigned width=binary?2:(type&0x800)?strWidth_:((type&255)<=2?2:4);
            schemas_[table].push_back({number,type,name,width});
        }
        for(auto& [name,cols]:schemas_){
            std::sort(cols.begin(),cols.end(),[](const Column& a,const Column& b){return a.number<b.number;});
            std::set<std::string> used;
            for(size_t i=0;i<cols.size();++i)
                if(cols[i].number!=i+1 || !used.insert(cols[i].name).second)throw Error("msi_columns_duplicate_or_gap");
        }
    }
    json Inspect(){
        json result={{"codepage",codepage_},{"string_reference_bytes",strWidth_},{"string_count",strings_.size()-1},
                     {"table_count",names_.size()},{"tables",json::array()},{"truncated",false},
                     {"execution", "never: CustomAction, sequences and conditions are data only"}};
        size_t emittedRows=0,emittedCells=0;
        for(const std::string& name:names_){
            if(result["tables"].size()>=limit_){result["truncated"]=true;break;}
            json table={{"name",name},{"columns",json::array()},{"rows",json::array()}};
            try{
                const auto it=schemas_.find(name);
                if(it==schemas_.end())throw Error("msi_table_schema_missing");
                const auto& columns=it->second;
                uint64_t stride=0;
                for(const auto& c:columns){
                    table["columns"].push_back({{"name",c.name},{"number",c.number},{"type_code",HexU64(c.type)},
                        {"storage_bytes",c.width},{"key",(c.type&0x2000)!=0},{"nullable",(c.type&0x1000)!=0},
                        {"kind",(c.type&0xFFF)==0x900?"binary":(c.type&0x800)?"string":"integer"}});
                    stride+=c.width;
                }
                const View data=Load(name,false);
                if(!stride || data.Size()%stride)throw Error("msi_table_storage_width_mismatch");
                const uint64_t count=data.Size()/stride;table["row_count"]=count;
                for(uint64_t row=0;row<count;++row){
                    if(emittedRows>=limit_ || emittedCells+columns.size()>65536){table["truncated"]=true;result["truncated"]=true;break;}
                    json record=json::object();uint64_t colOffset=0;
                    std::vector<std::string> binaryColumns;
                    for(const auto& c:columns){
                        const uint32_t raw=UInt(data,colOffset*count+row*c.width,c.width);
                        colOffset+=c.width;
                        if(!raw)record[c.name]=nullptr;
                        else if((c.type&0xFFF)==0x900){record[c.name]={{"stored_marker",raw}};binaryColumns.push_back(c.name);}
                        else if(c.type&0x800)record[c.name]=Preview(String(raw));
                        else if(c.width==2)record[c.name]=static_cast<int>(raw)-32768;
                        else record[c.name]=static_cast<int32_t>(raw^0x80000000u);
                    }
                    if(!binaryColumns.empty()){
                        std::string stream=name;bool complete=true;
                        for(const auto& c:columns)if(c.type&0x2000){
                            const json& value=record[c.name];
                            if(value.is_string())stream+='.'+value.get<std::string>();
                            else if(value.is_number_integer())stream+='.'+std::to_string(value.get<int64_t>());
                            else complete=false;
                        }
                        for(const auto& c:binaryColumns){
                            if(complete){
                                record[c]["stream_name"]=stream;
                                const Entry* entry=file_.FindRoot(stream,false);
                                record[c]["present"]=entry!=nullptr;
                                if(entry)record[c]["child"]=entry->path;
                            }else record[c]["error"]="primary_key_not_resolved";
                        }
                    }
                    table["rows"].push_back(std::move(record));++emittedRows;emittedCells+=columns.size();
                }
            }catch(const Error& ex){table["error"]=ex.what();}
            result["tables"].push_back(std::move(table));
        }
        result["embedded_cabinets"]=json::array();
        for(const auto& e:file_.entries){
            if(!e.reachable || e.type!=2 || e.table || e.parent!=0 || e.size<4)continue;
            if(result["embedded_cabinets"].size()>=limit_)break;
            // A name is only a candidate until the child provider verifies MSCF.
            if(e.decodedName.size()>=4 && e.decodedName.substr(e.decodedName.size()-4)==".cab")
                result["embedded_cabinets"].push_back({{"name",e.decodedName},{"child",e.path},{"size",e.size},{"status","name_candidate"}});
        }
        result["limits"]={{"max_total_rows",limit_},{"max_cells",65536},{"string_preview_bytes",4096},
            {"note","No MSI transforms, installation actions, signature trust verification or non-supported codepage guessing"}};
        if(!warnings_.empty())result["warnings"]=warnings_;
        return result;
    }
private:
    View Load(const std::string& name,bool required){
        const auto existing=cache_.find(name);if(existing!=cache_.end())return View(existing->second);
        const Entry* e=file_.FindRoot(name,true);
        if(!e){if(required)throw Error("msi_required_stream_missing:"+name);return {};}
        if(e->size>remaining_)throw Error("msi_stream_read_budget");remaining_-=e->size;
        return View(cache_.emplace(name,file_.ReadStream(e->index)).first->second);
    }
    uint32_t UInt(const View& data,uint64_t at,unsigned width) const{
        if(width==2)return data.U16(at);
        if(width==3)return data.U16(at)|(uint32_t(data.U8(at+2))<<16);
        return data.U32(at);
    }
    const std::string& String(uint32_t id)const{
        if(id>=strings_.size())throw Error("msi_string_reference_invalid");return strings_[id];
    }
    json Preview(const std::string& text){
        if(text.size()<=4096)return text;
        return {{"text",bounded::Utf8(reinterpret_cast<const uint8_t*>(text.data()),4096)},
                {"original_utf8_bytes",text.size()},{"truncated",true}};
    }
    const File& file_;
    size_t limit_;
    uint32_t codepage_=0,strWidth_=2;
    uint64_t remaining_=kReadLimit;
    std::vector<std::string> strings_,names_,warnings_;
    std::map<std::string,std::vector<Column>> schemas_;
    std::map<std::string,std::vector<uint8_t>> cache_;
};
} // namespace

json InspectMsi(const File& file,const InspectOptions& options){
    try{return Database(file,options).Inspect();}
    catch(const Error& ex){return {{"error",ex.what()},{"execution","never"}};}
}
} // namespace cortex::artifacts::compound
