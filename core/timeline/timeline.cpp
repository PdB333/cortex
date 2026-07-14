#include "timeline.h"
#include "../memory/memory.h"

#include <windows.h>
#include <algorithm>
#include <map>
#include <mutex>

namespace timeline {
namespace {
struct Checkpoint { CheckpointInfo info; std::vector<Range> ranges; };
std::mutex g_mutex;
std::map<int,Checkpoint> g_checkpoints;
int g_nextId=1;
constexpr size_t kMaxCheckpointBytes=64u*1024u*1024u;
constexpr size_t kMaxCheckpoints=64;

const Range* FindContaining(const Checkpoint& checkpoint, uintptr_t address, size_t size) {
    for(const auto& range:checkpoint.ranges) {
        if(address>=range.address && size<=range.bytes.size() && address-range.address<=range.bytes.size()-size) return &range;
    }
    return nullptr;
}
}

int Capture(const std::vector<std::pair<uintptr_t,size_t>>& requested,const std::string& label,std::string& error) {
    if(requested.empty()){error="no_ranges";return -1;}
    Checkpoint checkpoint; size_t total=0;
    for(const auto& item:requested) {
        if(item.second==0 || item.second>kMaxCheckpointBytes-total){error="checkpoint_too_large";return -1;}
        std::vector<uint8_t> bytes;
        if(!memory::ReadBytes(item.first,item.second,bytes)){error="read_failed";return -1;}
        checkpoint.ranges.push_back({item.first,std::move(bytes)}); total+=item.second;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    const int id=g_nextId++;
    checkpoint.info={id,GetTickCount64(),label,checkpoint.ranges.size(),total};
    g_checkpoints.emplace(id,std::move(checkpoint));
    while(g_checkpoints.size()>kMaxCheckpoints) g_checkpoints.erase(g_checkpoints.begin());
    return id;
}

std::vector<CheckpointInfo> List(){std::lock_guard<std::mutex> lock(g_mutex);std::vector<CheckpointInfo> out;for(const auto& item:g_checkpoints)out.push_back(item.second.info);return out;}

bool Diff(int fromId,int toId,std::vector<Change>& changes,std::string& error) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto a=g_checkpoints.find(fromId),b=g_checkpoints.find(toId);
    if(a==g_checkpoints.end()||b==g_checkpoints.end()){error="checkpoint_not_found";return false;}
    changes.clear();
    for(const auto& beforeRange:a->second.ranges) {
        const Range* afterRange=FindContaining(b->second,beforeRange.address,beforeRange.bytes.size());
        if(!afterRange) continue;
        const size_t afterOffset=beforeRange.address-afterRange->address;
        size_t i=0;
        while(i<beforeRange.bytes.size()) {
            if(beforeRange.bytes[i]==afterRange->bytes[afterOffset+i]){i++;continue;}
            const size_t start=i;
            while(i<beforeRange.bytes.size()&&beforeRange.bytes[i]!=afterRange->bytes[afterOffset+i])i++;
            changes.push_back({beforeRange.address+start,
                std::vector<uint8_t>(beforeRange.bytes.begin()+start,beforeRange.bytes.begin()+i),
                std::vector<uint8_t>(afterRange->bytes.begin()+afterOffset+start,afterRange->bytes.begin()+afterOffset+i)});
        }
    }
    return true;
}

bool Restore(int id,std::vector<Range>& previous,std::string& error) {
    std::vector<Range> target;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it=g_checkpoints.find(id);if(it==g_checkpoints.end()){error="checkpoint_not_found";return false;}
        target=it->second.ranges;
    }
    previous.clear();
    for(const auto& range:target){std::vector<uint8_t> bytes;if(!memory::ReadBytes(range.address,range.bytes.size(),bytes)){error="current_read_failed";return false;}previous.push_back({range.address,std::move(bytes)});}
    for(const auto& range:target)if(!memory::WriteBytes(range.address,range.bytes)){RestoreRanges(previous);error="restore_write_failed";return false;}
    return true;
}

bool RestoreRanges(const std::vector<Range>& ranges){bool ok=true;for(const auto& range:ranges)ok=memory::WriteBytes(range.address,range.bytes)&&ok;return ok;}

bool LastChange(uintptr_t address,size_t size,Transition& transition,std::string& error) {
    if(size==0||size>4096){error="invalid_size";return false;}
    transition = {};
    std::lock_guard<std::mutex> lock(g_mutex);
    const Checkpoint* previous=nullptr;
    for(const auto& item:g_checkpoints) {
        const Range* currentRange=FindContaining(item.second,address,size);
        if(!currentRange) continue;
        if(previous) {
            const Range* previousRange=FindContaining(*previous,address,size);
            if(previousRange) {
                const size_t po=address-previousRange->address,co=address-currentRange->address;
                if(!std::equal(previousRange->bytes.begin()+po,previousRange->bytes.begin()+po+size,currentRange->bytes.begin()+co)) {
                    transition={previous->info.id,item.second.info.id,item.second.info.timestampMs,
                        std::vector<uint8_t>(previousRange->bytes.begin()+po,previousRange->bytes.begin()+po+size),
                        std::vector<uint8_t>(currentRange->bytes.begin()+co,currentRange->bytes.begin()+co+size)};
                }
            }
        }
        previous=&item.second;
    }
    if(!previous || transition.toId==0){error="no_change_found";return false;}
    return true;
}

bool Remove(int id){std::lock_guard<std::mutex> lock(g_mutex);return g_checkpoints.erase(id)!=0;}
void Clear(){std::lock_guard<std::mutex> lock(g_mutex);g_checkpoints.clear();}
} // namespace timeline
