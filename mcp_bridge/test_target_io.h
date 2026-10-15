#pragma once
#include "test_runner.h"
#include "target/session.h"
#include "target/module_provider.h"
#include <windows.h>
#include <cmath>
#include <cstring>
#include <limits>

namespace cortex::test {
inline std::string Lower(std::string s){
    std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});return s;
}
inline uint64_t ParseAddress(const std::string& expression,const cortex::target::TargetDescriptor& target){
    if(expression.empty() || expression.front()=='-' || expression.front()=='+')throw std::runtime_error("invalid_address");
    const auto plus=expression.find('+');
    auto number=[](const std::string& s){size_t used=0;if(s.empty()||s.front()=='-')throw std::runtime_error("invalid_address");
        const auto n=std::stoull(s,&used,0);if(used!=s.size())throw std::runtime_error("invalid_address");return n;};
    if(plus==std::string::npos)return number(expression);
    const std::string name=Lower(expression.substr(0,plus));const uint64_t offset=number(expression.substr(plus+1));
    std::string error;
    for(const auto& m:cortex::target::ListTargetModules(target,&error))if(Lower(m.name)==name){
        if(offset>=m.size || offset>std::numeric_limits<uint64_t>::max()-m.base)throw std::runtime_error("module_offset_outside_image");
        return m.base+offset;
    }
    throw std::runtime_error("module_not_loaded");
}
inline json ReadSample(const cortex::target::SessionPtr& session,const json& spec){
    json result={{"request",spec},{"ok",false}};
    try{
        const std::string type=spec.at("type").get<std::string>();
        const uint64_t address=ParseAddress(spec.at("address").get<std::string>(),session->Target());
        size_t size=8;
        if(type=="i8"||type=="u8")size=1;
        else if(type=="i16"||type=="u16")size=2;
        else if(type=="i32"||type=="u32"||type=="float")size=4;
        else if(type=="bytes")size=spec.at("count").get<size_t>();
        if(!size||size>32||address>std::numeric_limits<uintptr_t>::max()-size)throw std::runtime_error("invalid_read_range");
        std::vector<unsigned char> bytes(size);size_t got=0;
        if(!session->ReadMemory(address,bytes.data(),size,&got)||got!=size)throw std::runtime_error("read_failed");
        json value;
        if(type=="bytes"){
            const char* digits="0123456789abcdef";std::string hex;
            for(auto b:bytes){hex+=digits[b>>4];hex+=digits[b&15];}value=hex;
        }else if(type=="float"){
            float v=0;std::memcpy(&v,bytes.data(),4);if(!std::isfinite(v))throw std::runtime_error("non_finite_value");value=v;
        }else if(type=="double"){
            double v=0;std::memcpy(&v,bytes.data(),8);if(!std::isfinite(v))throw std::runtime_error("non_finite_value");value=v;
        }else if(type=="i64"){int64_t v=0;std::memcpy(&v,bytes.data(),8);value=std::to_string(v);}
        else if(type=="u64"){uint64_t v=0;std::memcpy(&v,bytes.data(),8);value=std::to_string(v);}
        else if(type=="i8"){int8_t v=0;std::memcpy(&v,bytes.data(),1);value=v;}
        else if(type=="i16"){int16_t v=0;std::memcpy(&v,bytes.data(),2);value=v;}
        else if(type=="i32"){int32_t v=0;std::memcpy(&v,bytes.data(),4);value=v;}
        else {uint32_t v=0;std::memcpy(&v,bytes.data(),size);value=v;}
        result["ok"]=true;result["value"]=value;
    }catch(const std::exception& e){result["error"]=e.what();}
    return result;
}
struct WindowSearch {DWORD pid;std::vector<HWND> windows;};
inline BOOL CALLBACK CollectTestWindows(HWND w,LPARAM parameter){
    auto& query=*reinterpret_cast<WindowSearch*>(parameter);DWORD pid=0;
    GetWindowThreadProcessId(w,&pid);
    if(pid==query.pid&&IsWindowVisible(w)&&GetWindow(w,GW_OWNER)==nullptr)query.windows.push_back(w);
    return TRUE;
}
inline HWND TestWindow(uint64_t pid){
    WindowSearch query{static_cast<DWORD>(pid),{}};EnumWindows(CollectTestWindows,reinterpret_cast<LPARAM>(&query));
    // Never guess among launcher dialogs or multiple application windows.
    if(query.windows.size()!=1)return nullptr;
    return query.windows.front();
}
inline bool PostKey(HWND w,uint64_t expectedPid,int key,bool down){
    DWORD pid=0;if(!IsWindow(w)||!GetWindowThreadProcessId(w,&pid)||pid!=expectedPid||
        !IsWindowVisible(w)||IsIconic(w)||TestWindow(expectedPid)!=w)return false;
    const UINT scan=MapVirtualKeyW(static_cast<UINT>(key),MAPVK_VK_TO_VSC);
    LPARAM flags=static_cast<LPARAM>(1 | (scan<<16));
    if(key==VK_LEFT||key==VK_RIGHT||key==VK_UP||key==VK_DOWN||key==VK_INSERT||key==VK_DELETE||
       key==VK_HOME||key==VK_END||key==VK_PRIOR||key==VK_NEXT||key==VK_RCONTROL||key==VK_RMENU)
        flags|=static_cast<LPARAM>(1u<<24);
    if(!down)flags|=static_cast<LPARAM>(3u<<30);
    return PostMessageW(w,down?WM_KEYDOWN:WM_KEYUP,static_cast<WPARAM>(key),flags)!=FALSE;
}
// Client-coordinates only. Never moves the physical mouse or steals focus.
// WM_* support is engine dependent; success means queued, not processed.
inline bool PostMouse(HWND w,uint64_t expectedPid,int x,int y,
                      const std::string& button,bool down){
    DWORD pid=0;
    if(!IsWindow(w)||!GetWindowThreadProcessId(w,&pid)||pid!=expectedPid||
       !IsWindowVisible(w)||IsIconic(w)||TestWindow(expectedPid)!=w)return false;
    RECT rect{};
    if(!GetClientRect(w,&rect)||x<0||y<0||x>=rect.right-rect.left||
       y>=rect.bottom-rect.top||x>32767||y>32767)return false;
    UINT message=0;
    WPARAM flags=0;
    if(button=="left"){message=down?WM_LBUTTONDOWN:WM_LBUTTONUP;flags=down?MK_LBUTTON:0;}
    else if(button=="right"){message=down?WM_RBUTTONDOWN:WM_RBUTTONUP;flags=down?MK_RBUTTON:0;}
    else return false;
    const LPARAM point=MAKELPARAM(static_cast<WORD>(x),static_cast<WORD>(y));
    if(down&&!PostMessageW(w,WM_MOUSEMOVE,0,point))return false;
    return PostMessageW(w,message,flags,point)!=FALSE;
}
} // namespace cortex::test
