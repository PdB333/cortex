#include "ai_investigation_workspace.h"
#include "widgets.h"
#include "../../core/remote/client.h"
#include <imgui.h>
#include <cstdio>

namespace cortex::ui {
void AiInvestigationWorkspace::Ps5Run(UiContext& context,const std::string& action){
    using json=nlohmann::json;json args=json::object();
    if(action=="maps"||action=="read"||action=="write"){args["pid"]=ps5Pid_;args["target_id"]=ps5Target_.data();}
    if(action=="read"||action=="write")args["address"]=ps5Address_.data();
    if(action=="read")args["length"]=ps5Length_;
    if(action=="write"){args["hex"]=ps5Hex_.data();args["expected_hex"]=ps5Expected_.data();args["expected_name"]=ps5Name_.data();args["mutation_permission"]=true;}
    const std::string config=ps5Config_.data();const bool writes=ps5AllowWrites_;
    context.RunInBackground("Cortex Remote: "+action,[this,&context,config,action,args,writes]{
        ps5Result_=cortex::remote::RemoteRequest(config,action,args,writes);
        if(action=="processes"&&ps5Result_.value("ok",false))ps5Processes_=ps5Result_["processes"];
        context.status=ps5Result_.value("ok",false)?"Cortex Remote: "+action+" completed":"Cortex Remote: "+ps5Result_.value("error",std::string("failed"));
    });
}
void AiInvestigationWorkspace::DrawPs5(UiContext& context){
    HintText("Our Cortex agent and Cortex Remote protocol, with mutual TLS. The native PS5 backend requires an authorized execution environment. "
             "No discovery or third-party debugger. Reads may briefly stop the target if its operator allowed ptrace.");
    if(ImGui::InputTextWithHint("Client TLS profile","Path to client.json",ps5Config_.data(),ps5Config_.size())){
        ps5Target_.fill(0);ps5Name_.fill(0);ps5Processes_=nullptr;ps5Result_=nullptr;ps5AllowWrites_=false;
    }
    if(ImGui::Button("Probe Cortex agent")){Ps5Run(context,"info");return;}
    ImGui::SameLine();if(ImGui::Button("List remote processes")){Ps5Run(context,"processes");return;}
    if(ps5Processes_.is_array()){
        if(ImGui::BeginChild("Remote processes",ImVec2(0,Px(135)),ImGuiChildFlags_Borders)){
            for(const auto& row:ps5Processes_){
                if(!row.is_object()||!row.contains("pid")||!row["pid"].is_number_integer()||!row.contains("name")||!row["name"].is_string()||!row.contains("target_id")||!row["target_id"].is_string())continue;
                auto rawPid=row["pid"].get<int64_t>();if(rawPid<1||rawPid>2147483647)continue;int pid=static_cast<int>(rawPid);
                std::string name=row["name"].get<std::string>(),token=row["target_id"].get<std::string>();if(token.size()>256||name.size()>128)continue;
                std::string label=std::to_string(pid)+"  "+name+"##remote"+std::to_string(pid);
                if(ImGui::Selectable(label.c_str(),ps5Pid_==pid&&token==ps5Target_.data())){
                    ps5Pid_=pid;std::snprintf(ps5Name_.data(),ps5Name_.size(),"%s",name.c_str());std::snprintf(ps5Target_.data(),ps5Target_.size(),"%s",token.c_str());
                    ps5Expected_.fill(0);ps5Hex_.fill(0);ps5Result_=nullptr;
                }
            }
        }
        ImGui::EndChild();
    }
    ImGui::Text("Selected remote PID: %d",ps5Pid_);
    ImGui::TextWrapped("Target generation: %s",ps5Target_[0]?ps5Target_.data():"select a process from the list");
    if(ImGui::Button("Read remote memory map")){Ps5Run(context,"maps");return;}
    ImGui::InputText("Remote address",ps5Address_.data(),ps5Address_.size());
    ImGui::InputInt("Bytes (1-65536)",&ps5Length_);
    if(ImGui::Button("Read remote memory")){Ps5Run(context,"read");return;}
    ImGui::Separator();ImGui::Checkbox("Allow writes for this panel session",&ps5AllowWrites_);
    bool confirm=false;
    if(ps5AllowWrites_){
        HintText("The client profile AND agent must also allow writes. No automatic retry or rollback. "
                 "An uncertain result may mean memory changed. The GUI accepts up to 256 replacement bytes.");
        ImGui::InputText("Expected process name",ps5Name_.data(),ps5Name_.size());
        ImGui::InputText("Expected bytes (hex)",ps5Expected_.data(),ps5Expected_.size());
        ImGui::InputText("New bytes (hex)",ps5Hex_.data(),ps5Hex_.size());
        if(ImGui::Button("Review remote write..."))ImGui::OpenPopup("Confirm Cortex Remote write");
        if(ImGui::BeginPopupModal("Confirm Cortex Remote write",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){
            ImGui::TextWrapped("Profile: %s",ps5Config_.data());ImGui::Text("PID: %d  Name: %s",ps5Pid_,ps5Name_.data());
            ImGui::Text("Address: %s",ps5Address_.data());ImGui::TextWrapped("Target: %s",ps5Target_.data());
            ImGui::TextWrapped("The agent will verify identity and expected bytes, write and read back. This is not a global transaction.");
            if(ImGui::Button("Confirm write")){ImGui::CloseCurrentPopup();confirm=true;}
            ImGui::SameLine();if(ImGui::Button("Cancel"))ImGui::CloseCurrentPopup();ImGui::EndPopup();
        }
    }
    if(confirm){Ps5Run(context,"write");return;}
    if(!ps5Result_.is_null()){
        ImGui::Separator();auto text=ps5Result_.dump(2,' ',false,nlohmann::json::error_handler_t::replace);
        if(ImGui::Button("Copy remote result"))ImGui::SetClipboardText(text.c_str());
        ImGui::BeginChild("Cortex Remote result",ImVec2(0,0),ImGuiChildFlags_Borders,ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(text.c_str());ImGui::EndChild();
    }
}
} // namespace cortex::ui
