#include "watches_workspace.h"
#include "widgets.h"

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
        HintText("Select a process to use runtime watches.");
        return;
    }

    if (targetId_ != session->Target().id) {
        targetId_ = session->Target().id;
        context.watchesModel->Reset();
        lastRefresh_ = {};
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Runtime watches & freezes");
    FlowSameLine(std::max(TextWidth("runtime connected"), TextWidth("runtime disconnected")));
    ImGui::TextDisabled(context.payload && context.payload->Ready()
                            ? "runtime connected" : "runtime disconnected");
    FlowSameLine(ButtonWidth("Connect existing"));
    if (context.payload && !context.payload->Ready()) {
        if (ImGui::Button("Connect existing")) {
            std::string error;
            if (!context.payload->TryConnectExisting(&error))
                context.status = "Runtime connect failed: " + error;
            else
                Refresh(context, true);
        }
        FlowSameLine(ButtonWidth("Enable runtime"));
        ImGui::BeginDisabled(!context.mutationAllowed);
        if (ImGui::Button("Enable runtime")) {
            context.RunInBackground("Loading the Cortex runtime into the target", [=, &context]() {
                std::string error;
                if (!context.payload->EnsureReady(&error))
                    context.status = "Runtime enable failed: " + error;
                else
                    Refresh(context, true);
            });
        }
        ImGui::EndDisabled();
        FlowSameLine(ButtonWidth("Refresh"));
    }
    if (ImGui::Button("Refresh")) Refresh(context, true);

    const int refreshMs = context.settings ? context.settings->Values().autoRefreshMs : 750;
    if (context.payload && context.payload->Ready()) {
        const auto now = std::chrono::steady_clock::now();
        if (lastRefresh_.time_since_epoch().count() == 0 ||
            now - lastRefresh_ >= std::chrono::milliseconds(refreshMs))
            Refresh(context, false);
    }

    ImGui::Separator();

    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Watch");
    FlowSameLine(Px(170));
    ImGui::SetNextItemWidth(Px(170));
    ImGui::InputTextWithHint("##WatchAddress", "Address",
                             watchAddress_.data(), watchAddress_.size());
    FlowSameLine(Px(95));
    ImGui::SetNextItemWidth(Px(95));
    ImGui::Combo("##WatchType", &watchType_, kWatchTypes, IM_ARRAYSIZE(kWatchTypes));
    FlowSameLine(Px(170));
    ImGui::SetNextItemWidth(Px(170));
    ImGui::InputTextWithHint("##WatchLabel", "Label",
                             watchLabel_.data(), watchLabel_.size());
    FlowSameLine(ButtonWidth("Add watch"));
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

    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Freeze");
    FlowSameLine(Px(160));
    ImGui::SetNextItemWidth(Px(160));
    ImGui::InputTextWithHint("##FreezeAddress", "Address",
                             freezeAddress_.data(), freezeAddress_.size());
    FlowSameLine(Px(95));
    ImGui::SetNextItemWidth(Px(95));
    ImGui::Combo("##FreezeType", &freezeType_, kFreezeTypes, IM_ARRAYSIZE(kFreezeTypes));
    FlowSameLine(Px(125));
    ImGui::SetNextItemWidth(Px(125));
    ImGui::InputTextWithHint("##FreezeValue", "Value",
                             freezeValue_.data(), freezeValue_.size());
    FlowSameLine(Px(135));
    ImGui::SetNextItemWidth(Px(135));
    ImGui::InputTextWithHint("##FreezeLabel", "Label",
                             freezeLabel_.data(), freezeLabel_.size());
    FlowSameLine(Px(100));
    ImGui::SetNextItemWidth(Px(100));
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
    // Side by side when both lists get a readable width, stacked otherwise.
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const bool sideBySide = avail.x >= 2.0f * Px(300.0f) + spacing;
    const ImVec2 watchSize = sideBySide
        ? ImVec2((avail.x - spacing) * 0.5f, 0.0f)
        : ImVec2(0.0f, std::max(Px(140.0f), (avail.y - ImGui::GetStyle().ItemSpacing.y) * 0.5f));

    int deleteWatch = -1;
    ImGui::BeginChild("WatchList", watchSize, ImGuiChildFlags_Borders);
    ImGui::Text("Watches (%zu)", context.watchesModel->Watches().size());
    ImGui::Separator();
    if (BeginDataTable("WatchesTable", 5,
                          ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_BordersInnerH |
                          ImGuiTableFlags_Resizable |
                          ImGuiTableFlags_ScrollY,
                          ImGui::GetContentRegionAvail())) {
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, Px(45));
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch, 0.28f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, Px(65));
        ImGui::TableSetupColumn("Value / Label", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, Px(70));
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

    if (sideBySide) ImGui::SameLine();

    int deleteFreeze = -1;
    ImGui::BeginChild("FreezeList", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::Text("Freezes (%zu)", context.watchesModel->Freezes().size());
    ImGui::Separator();
    if (BeginDataTable("FreezesTable", 6,
                          ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_BordersInnerH |
                          ImGuiTableFlags_Resizable |
                          ImGuiTableFlags_ScrollY,
                          ImGui::GetContentRegionAvail())) {
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, Px(45));
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch, 0.25f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, Px(65));
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.25f);
        ImGui::TableSetupColumn("Label / TTL", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, Px(70));
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
