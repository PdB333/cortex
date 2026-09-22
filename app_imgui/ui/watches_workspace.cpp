#include "watches_workspace.h"

#include <imgui.h>

#include <algorithm>

namespace cortex::ui {
namespace {

constexpr const char* kWatchTypes[] = {
    "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "float", "double"
};
constexpr const char* kFreezeTypes[] = {
    "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "float", "double", "bytes"
};

} // namespace

void WatchesWorkspace::Refresh(UiContext& context, bool reportStatus) {
    if (!context.watchesModel) return;
    std::string error;
    if (!context.watchesModel->Refresh(&error)) {
        if (reportStatus) context.status = "Watches refresh failed: " + error;
        return;
    }
    lastRefresh_ = std::chrono::steady_clock::now();
    if (reportStatus)
        context.status = std::to_string(context.watchesModel->Watches().size()) +
                         " watch(es), " +
                         std::to_string(context.watchesModel->Freezes().size()) +
                         " freeze(s)";
}

void WatchesWorkspace::Draw(UiContext& context) {
    const auto session = context.sessions ? context.sessions->Active() : nullptr;
    if (!session || !context.watchesModel) {
        ImGui::TextDisabled("Select a process to use runtime watches.");
        return;
    }

    if (targetId_ != session->Target().id) {
        targetId_ = session->Target().id;
        context.watchesModel->Reset();
        lastRefresh_ = {};
    }

    ImGui::TextUnformatted("Runtime watches & freezes");
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
                Refresh(context, true);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!context.mutationAllowed);
        if (ImGui::SmallButton("Enable runtime")) {
            std::string error;
            if (!context.payload->EnsureReady(&error))
                context.status = "Runtime enable failed: " + error;
            else
                Refresh(context, true);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
    }
    if (ImGui::SmallButton("Refresh")) Refresh(context, true);

    const int refreshMs = context.settings ? context.settings->Values().autoRefreshMs : 750;
    if (context.payload && context.payload->Ready()) {
        const auto now = std::chrono::steady_clock::now();
        if (lastRefresh_.time_since_epoch().count() == 0 ||
            now - lastRefresh_ >= std::chrono::milliseconds(refreshMs))
            Refresh(context, false);
    }

    ImGui::Separator();

    ImGui::TextDisabled("Watch");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(170);
    ImGui::InputTextWithHint("##WatchAddress", "Address",
                             watchAddress_.data(), watchAddress_.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(95);
    ImGui::Combo("##WatchType", &watchType_, kWatchTypes, IM_ARRAYSIZE(kWatchTypes));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(170);
    ImGui::InputTextWithHint("##WatchLabel", "Label",
                             watchLabel_.data(), watchLabel_.size());
    ImGui::SameLine();
    ImGui::BeginDisabled(!context.mutationAllowed || watchAddress_[0] == '\0');
    if (ImGui::Button("Add watch")) {
        std::string error;
        if (!context.watchesModel->AddWatch(
                watchAddress_.data(), kWatchTypes[watchType_],
                watchLabel_.data(), context.mutationAllowed, &error))
            context.status = "Add watch failed: " + error;
        else {
            watchAddress_.fill(0);
            watchLabel_.fill(0);
            context.status = "Watch added";
        }
    }
    ImGui::EndDisabled();

    ImGui::TextDisabled("Freeze");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    ImGui::InputTextWithHint("##FreezeAddress", "Address",
                             freezeAddress_.data(), freezeAddress_.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(95);
    ImGui::Combo("##FreezeType", &freezeType_, kFreezeTypes, IM_ARRAYSIZE(kFreezeTypes));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(125);
    ImGui::InputTextWithHint("##FreezeValue", "Value",
                             freezeValue_.data(), freezeValue_.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(135);
    ImGui::InputTextWithHint("##FreezeLabel", "Label",
                             freezeLabel_.data(), freezeLabel_.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    ImGui::InputInt("TTL ms", &freezeTtlMs_);
    freezeTtlMs_ = std::max(0, freezeTtlMs_);
    ImGui::SameLine();
    ImGui::BeginDisabled(!context.mutationAllowed ||
                         freezeAddress_[0] == '\0' || freezeValue_[0] == '\0');
    if (ImGui::Button("Add freeze")) {
        std::string error;
        if (!context.watchesModel->AddFreeze(
                freezeAddress_.data(), kFreezeTypes[freezeType_],
                freezeValue_.data(), freezeLabel_.data(), freezeTtlMs_,
                context.mutationAllowed, &error))
            context.status = "Add freeze failed: " + error;
        else {
            freezeAddress_.fill(0);
            freezeValue_.fill(0);
            freezeLabel_.fill(0);
            freezeTtlMs_ = 0;
            context.status = "Freeze added";
        }
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    const float half = std::max(260.0f, (ImGui::GetContentRegionAvail().x -
                                       ImGui::GetStyle().ItemSpacing.x) * 0.5f);

    int deleteWatch = -1;
    ImGui::BeginChild("WatchList", ImVec2(half, 0), ImGuiChildFlags_Borders);
    ImGui::Text("Watches (%zu)", context.watchesModel->Watches().size());
    ImGui::Separator();
    if (ImGui::BeginTable("WatchesTable", 5,
                          ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_BordersInnerH |
                          ImGuiTableFlags_Resizable |
                          ImGuiTableFlags_ScrollY,
                          ImGui::GetContentRegionAvail())) {
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 45);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch, 0.28f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 65);
        ImGui::TableSetupColumn("Value / Label", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableHeadersRow();
        for (const auto& row : context.watchesModel->Watches()) {
            ImGui::PushID(row.id);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", row.id);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(row.address.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(row.type.c_str());
            ImGui::TableSetColumnIndex(3);
            if (row.hasValue) ImGui::Text("%s  %s", row.value.c_str(), row.label.c_str());
            else ImGui::TextDisabled("%s", row.label.empty() ? "(no value)" : row.label.c_str());
            ImGui::TableSetColumnIndex(4);
            ImGui::BeginDisabled(!context.mutationAllowed);
            if (ImGui::SmallButton("Remove")) deleteWatch = row.id;
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    int deleteFreeze = -1;
    ImGui::BeginChild("FreezeList", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::Text("Freezes (%zu)", context.watchesModel->Freezes().size());
    ImGui::Separator();
    if (ImGui::BeginTable("FreezesTable", 6,
                          ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_BordersInnerH |
                          ImGuiTableFlags_Resizable |
                          ImGuiTableFlags_ScrollY,
                          ImGui::GetContentRegionAvail())) {
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 45);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch, 0.25f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 65);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.25f);
        ImGui::TableSetupColumn("Label / TTL", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableHeadersRow();
        for (const auto& row : context.watchesModel->Freezes()) {
            ImGui::PushID(row.id);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", row.id);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(row.address.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(row.type.c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(row.valueBytes.c_str());
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%s%s%lld ms",
                        row.label.c_str(),
                        row.label.empty() ? "" : "  ",
                        static_cast<long long>(row.ttlMsRemaining));
            ImGui::TableSetColumnIndex(5);
            ImGui::BeginDisabled(!context.mutationAllowed);
            if (ImGui::SmallButton("Remove")) deleteFreeze = row.id;
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    if (deleteWatch >= 0) {
        std::string error;
        if (!context.watchesModel->DeleteWatch(
                deleteWatch, context.mutationAllowed, &error))
            context.status = "Delete watch failed: " + error;
        else
            context.status = "Watch removed";
    }
    if (deleteFreeze >= 0) {
        std::string error;
        if (!context.watchesModel->DeleteFreeze(
                deleteFreeze, context.mutationAllowed, &error))
            context.status = "Delete freeze failed: " + error;
        else
            context.status = "Freeze removed";
    }
}

} // namespace cortex::ui
