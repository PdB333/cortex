// Deterministic mutational runner for environments without libFuzzer support.
// Links the exact LLVMFuzzerTestOneInput harness and sanitizer-instrumented
// parsers. This is NOT coverage-guided fuzzing and is labelled accordingly.
// args: CORPUS_DIR ITERATIONS [ONLY_CASE] [REPRO_FILE]
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t*,size_t);
int main(int argc,char** argv){
    namespace fs=std::filesystem;
    try{
        if(argc<3)throw std::runtime_error("usage: mutation_runner CORPUS_DIR ITERATIONS [ONLY_CASE] [REPRO_FILE]");
        const uint64_t iterations=std::stoull(argv[2]);if(iterations>1000000)throw std::runtime_error("iteration limit");
        const uint64_t only=argc>3?std::stoull(argv[3]):UINT64_MAX;
        const fs::path repro=argc>4?fs::u8path(argv[4]):fs::path();
        std::vector<fs::path> paths;
        for(const auto& entry:fs::directory_iterator(fs::u8path(argv[1])))
            if(entry.is_regular_file()&&entry.file_size()>0&&entry.file_size()<=(1u<<20))paths.push_back(entry.path());
        std::sort(paths.begin(),paths.end());if(paths.empty()||paths.size()>1024)throw std::runtime_error("corpus missing or too large");
        std::vector<std::vector<uint8_t>> seeds;
        uint64_t total=0;
        for(const auto& path:paths){
            std::ifstream file(path,std::ios::binary);if(!file)throw std::runtime_error("cannot read seed");
            seeds.emplace_back(std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>());
            total+=seeds.back().size();if(total>(64u<<20))throw std::runtime_error("corpus byte budget");
            if(only==UINT64_MAX){
                std::cerr<<"seed "<<path.filename().u8string()<<'\n';
                LLVMFuzzerTestOneInput(seeds.back().data(),seeds.back().size());
            }
        }
        std::mt19937 rng(0xC047E1u);
        const auto started=std::chrono::steady_clock::now();uint64_t executed=0;
        for(uint64_t n=0;n<iterations;++n){
            const size_t selected=rng()%seeds.size();auto bytes=seeds[selected];
            const unsigned mode=rng()%5;
            if(mode==0){
                const unsigned edits=1+rng()%8;
                for(unsigned i=0;i<edits;++i)bytes[rng()%bytes.size()]^=uint8_t(1u<<(rng()%8));
            }else if(mode==1){
                const size_t at=rng()%bytes.size(),count=std::min<size_t>(1+rng()%8,bytes.size()-at);
                const uint8_t value=(rng()%2)?0:255;std::fill(bytes.begin()+at,bytes.begin()+at+count,value);
            }else if(mode==2){bytes.resize(rng()%(bytes.size()+1));}
            else if(mode==3){
                const size_t at=rng()%bytes.size(),count=std::min<size_t>(1+rng()%32,bytes.size()-at);
                const auto copy=std::vector<uint8_t>(bytes.begin()+at,bytes.begin()+at+count);
                bytes.insert(bytes.begin()+at,copy.begin(),copy.end());
            }else{
                for(unsigned i=0,edits=1+rng()%6;i<edits;++i)bytes[rng()%bytes.size()]=static_cast<uint8_t>(rng());
            }
            if(only!=UINT64_MAX&&n!=only)continue;
            std::cerr<<"case "<<n<<" seed "<<selected<<" bytes "<<bytes.size()<<std::endl;
            if(!repro.empty()){
                std::ofstream saved(repro,std::ios::binary|std::ios::trunc);saved.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
                if(!saved)throw std::runtime_error("reproducer write failed");
            }
            LLVMFuzzerTestOneInput(bytes.data(),bytes.size());++executed;
        }
        const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count();
        std::cout<<"{\"runner\":\"deterministic_mutation_not_coverage_guided\",\"seed\":12601313,\"corpus\":"<<seeds.size()
                 <<",\"executed\":"<<executed<<",\"elapsed_ms\":"<<ms<<"}\n";
        return 0;
    }catch(const std::exception& ex){std::cerr<<"mutation failure: "<<ex.what()<<'\n';return 1;}
}
