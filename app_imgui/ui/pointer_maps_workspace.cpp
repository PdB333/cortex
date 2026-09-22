#include "pointer_maps_workspace.h"

#include <imgui.h>

#include <algorithm>

namespace cortex::ui {

void PointerMapsWorkspace::SyncSelection(UiContext& context) {
    if (!context.pointerMapsModel) return;
    selected_.erase(
        std::remove_if(selected_.begin(), selected_.end(), [&](const std::string& name) {
            return std::none_of(context.pointerMapsModel->Maps().begin(),
                                context.pointerMapsModel->Maps().end(),
                                [&](const auto& row) { return row.name == name; });
        }),
        selected_.end());
}

void PointerMapsWorkspace::Draw(UiContext& context) {
    const auto session = context.sessions ? context.sessions->Active() : nullptr;
    if (!session || !context.pointerMapsModel) {
        ImGui::TextDisabled("Select a process to capture pointer maps.");
        return;
    }

    if (targetId_ != session->Target().id) {
        targetId_ = session->Target().id;
        context.pointerMapsModel->Reset();
        selected_.clear();
    }

    ImGui::TextUnformatted("Pointer maps");
    ImGui::SameLine();
    ImGui::TextDisabled(context.payload && context.payload->Ready()
                            ? "runtime connected" : "runtime disconnected");
    ImGui::SameLine();
    if (context.payload && !context.payload->Ready()) {
        if (ImGui::SmallButton("Connect existing")) {
            std::string error;
            if (!context.payload->TryConnectExisting(&error))
                context.status = "Runtime connect failed: " + error;
            else
                context.pointerMapsModel->Refresh(nullptr);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!context.mutationAllowed);
        if (ImGui::SmallButton("Enable runtime")) {
            std::string error;
            if (!context.payload->EnsureReady(&error))
                context.status = "Runtime enable failed: " + error;
            else
                context.pointerMapsModel->Refresh(nullptr);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
    }
    if (ImGui::SmallButton("Refresh")) {
        std::string error;
        if (!context.pointerMapsModel->Refresh(&error))
            context.status = "Pointer map refresh failed: " + error;
        else {
            SyncSelection(context);
            context.status = std::to_string(context.pointerMapsModel->Maps().size()) +
                             " pointer map(s)";
        }
    }

    ImGui::Separator();

    ImGui::SetNextItemWidth(130);
    ImGui::InputTextWithHint("##PointerMapName", "Map name",
                             name_.data(), name_.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(190);
    ImGui::InputTextWithHint("##PointerMapTarget", "Target address/expression",
                             target_.data(), target_.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    ImGui::InputInt("Depth", &maxDepth_);
    maxDepth_ = std::clamp(maxDepth_, 1, 16);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    ImGui::InputInt("Max offset", &maxOffset_);
    maxOffset_ = std::clamp(maxOffset_, 1, 1 << 20);
    ImGui::SameLine();
    ImGui::BeginDisabled(!context.mutationAllowed ||
                         name_[0] == '\0' || target_[0] == '\0');
    if (ImGui::Button("Capture")) {
        std::string error;
        if (!context.pointerMapsModel->Capture(
                name_.data(), target_.data(), maxDepth_, maxOffset_,
                context.mutationAllowed, &error))
            context.status = "Pointer map capture failed: " + error;
        else {
            name_.fill(0);
            context.status = "Pointer map captured";
        }
    }
    ImGui::EndDisabled();

    const float mapsHeight =
        std::clamp(ImGui::GetContentRegionAvail().y * 0.42f, 180.0f, 330.0f);

    std::string deleteName;
    if (ImGui::BeginTable("PointerMapsTable", 7,
                          ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_BordersInnerH |
                          ImGuiTableFlags_Resizable |
                          ImGuiTableFlags_ScrollY,
                          ImVec2(0, mapsHeight))) {
        ImGui::TableSetupColumn("Use", ImGuiTableColumnFlags_WidthFixed, 45);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.22f);
        ImGui::TableSetupColumn("Target", ImGuiTableColumnFlags_WidthStretch, 0.27f);
        ImGui::TableSetupColumn("Created", ImGuiTableColumnFlags_WidthFixed, 115);
        ImGui::TableSetupColumn("Paths", ImGuiTableColumnFlags_WidthFixed, 75);
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 75);
        ImGui::TableHeadersRow();

        for (const auto& map : context.pointerMapsModel->Maps()) {
            ImGui::PushID(map.name.c_str());
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            bool checked = std::find(selected_.begin(), selected_.end(), map.name) != selected_.end();
            if (ImGui::Checkbox("##Use", &checked)) {
                if (checked) selected_.push_back(map.name);
                else selected_.erase(std::remove(selected_.begin(), selected_.end(), map.name),
                                     selected_.end());
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(map.name.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(map.target.c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%llu", static_cast<unsigned long long>(map.createdMs));
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%llu", static_cast<unsigned long long>(map.pathCount));
            ImGui::TableSetColumnIndex(5);
            ImGui::TextUnformatted(map.truncated ? "truncated" : "complete");
            ImGui::TableSetColumnIndex(6);
            ImGui::BeginDisabled(!context.mutationAllowed);
            if (ImGui::SmallButton("Delete")) deleteName = map.name;
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (!deleteName.empty()) {
        std::string error;
        if (!context.pointerMapsModel->Delete(
                deleteName, context.mutationAllowed, &error))
            context.status = "Pointer map delete failed: " + error;
        else {
            SyncSelection(context);
            context.status = "Pointer map deleted";
        }
    }

    ImGui::Spacing();
    ImGui::Text("%zu map(s) selected", selected_.size());
    ImGui::SameLine();
    ImGui::BeginDisabled(selected_.size() < 2);
    if (ImGui::Button("Intersect selected")) {
        std::string error;
        if (!context.pointerMapsModel->Intersect(selected_, &error))
            context.status = "Pointer map intersection failed: " + error;
        else
            context.status = std::to_string(context.pointerMapsModel->Paths().size()) +
                             " stable path(s)";
    }
    ImGui::EndDisabled();

    if (ImGui::BeginTable("PointerPathCandidates", 5,
                          ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_BordersInnerH |
                          ImGuiTableFlags_Resizable |
                          ImGuiTableFlags_ScrollY,
                          ImGui::GetContentRegionAvail())) {
        ImGui::TableSetupColumn("Module", ImGuiTableColumnFlags_WidthStretch, 0.22f);
        ImGui::TableSetupColumn("Base offset", ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("Offsets", ImGuiTableColumnFlags_WidthStretch, 0.38f);
        ImGui::TableSetupColumn("Sessions", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Score", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableHeadersRow();

        for (const auto& path : context.pointerMapsModel->Paths()) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(path.module.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%lld", static_cast<long long>(path.baseOffset));
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(path.offsetsJson.c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%d", path.sessions);
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%.4f", path.score);
        }
        ImGui::EndTable();
    }
}

} // namespace cortex::ui
