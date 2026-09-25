#include "snapshots_workspace.h"

#include <imgui.h>

#include <algorithm>
#include <cstring>

namespace cortex::ui {
namespace {

void Copy(char* destination, size_t capacity, const char* source) {
    if (!destination || capacity == 0) return;
    std::memset(destination, 0, capacity);
    if (!source) return;
    const size_t size = std::strlen(source);
    std::memcpy(destination, source, std::min(capacity - 1, size));
}

} // namespace

void SnapshotsWorkspace::Draw(UiContext& context) {
    const auto session = context.sessions ? context.sessions->Active() : nullptr;
    if (!session || !context.snapshotsModel) {
        ImGui::TextDisabled("Select a process to capture memory snapshots.");
        return;
    }

    if (targetId_ != session->Target().id) {
        targetId_ = session->Target().id;
        context.snapshotsModel->Reset();
        fromId_ = -1;
        toId_ = -1;
        rangesJson_.fill(0);
        Copy(rangesJson_.data(), rangesJson_.size(),
             "[\n  {\"address\":\"0x0\",\"size\":64}\n]");
    }

    ImGui::TextUnformatted("Memory snapshots");
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
                context.snapshotsModel->Refresh(nullptr);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!context.mutationAllowed);
        if (ImGui::SmallButton("Enable runtime")) {
            std::string error;
            if (!context.payload->EnsureReady(&error))
                context.status = "Runtime enable failed: " + error;
            else
                context.snapshotsModel->Refresh(nullptr);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
    }
    if (ImGui::SmallButton("Refresh")) {
        std::string error;
        if (!context.snapshotsModel->Refresh(&error))
            context.status = "Snapshot refresh failed: " + error;
        else
            context.status = std::to_string(context.snapshotsModel->Snapshots().size()) +
                             " snapshot(s)";
    }

    ImGui::Separator();

    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##SnapshotLabel", "Optional label",
                             label_.data(), label_.size());
    ImGui::TextDisabled("Ranges JSON");
    ImGui::InputTextMultiline("##SnapshotRanges", rangesJson_.data(),
                              rangesJson_.size(), ImVec2(-1, 100));
    ImGui::BeginDisabled(!context.payload || !context.payload->Ready());
    if (ImGui::Button("Capture snapshot")) {
        std::string error;
        if (!context.snapshotsModel->Create(
                rangesJson_.data(), label_.data(), &error))
            context.status = "Snapshot capture failed: " + error;
        else {
            label_.fill(0);
            context.status = "Snapshot captured";
        }
    }
    ImGui::EndDisabled();

    const float tableHeight =
        std::clamp(ImGui::GetContentRegionAvail().y * 0.42f, 180.0f, 320.0f);

    int rewindId = -1;
    int deleteId = -1;
    if (ImGui::BeginTable("SnapshotsTable", 7,
                          ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_BordersInnerH |
                          ImGuiTableFlags_Resizable |
                          ImGuiTableFlags_ScrollY,
                          ImVec2(0, tableHeight))) {
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 50);
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch, 0.28f);
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 120);
        ImGui::TableSetupColumn("Ranges", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Bytes", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("Diff", ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableHeadersRow();

        for (const auto& snap : context.snapshotsModel->Snapshots()) {
            ImGui::PushID(snap.id);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", snap.id);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(snap.label.empty() ? "-" : snap.label.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%llu", static_cast<unsigned long long>(snap.timestampMs));
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%llu", static_cast<unsigned long long>(snap.rangeCount));
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%llu", static_cast<unsigned long long>(snap.totalBytes));
            ImGui::TableSetColumnIndex(5);
            const bool isFrom = fromId_ == snap.id;
            const bool isTo = toId_ == snap.id;
            if (ImGui::SmallButton(isFrom ? "From*" : "From")) fromId_ = snap.id;
            ImGui::SameLine();
            if (ImGui::SmallButton(isTo ? "To*" : "To")) toId_ = snap.id;
            ImGui::TableSetColumnIndex(6);
            ImGui::BeginDisabled(!context.mutationAllowed);
            if (ImGui::SmallButton("Rewind")) rewindId = snap.id;
            ImGui::SameLine();
            if (ImGui::SmallButton("Delete")) deleteId = snap.id;
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (rewindId >= 0) {
        std::string error;
        if (!context.snapshotsModel->Rewind(
                rewindId, context.mutationAllowed, &error))
            context.status = "Snapshot rewind failed: " + error;
        else
            context.status = "Snapshot rewound";
    }
    if (deleteId >= 0) {
        std::string error;
        if (!context.snapshotsModel->Delete(
                deleteId, context.mutationAllowed, &error))
            context.status = "Snapshot delete failed: " + error;
        else {
            if (fromId_ == deleteId) fromId_ = -1;
            if (toId_ == deleteId) toId_ = -1;
            context.status = "Snapshot deleted";
        }
    }

    ImGui::BeginDisabled(fromId_ < 0 || toId_ < 0 || fromId_ == toId_);
    if (ImGui::Button("Diff selected snapshots")) {
        std::string error;
        if (!context.snapshotsModel->Diff(fromId_, toId_, &error))
            context.status = "Snapshot diff failed: " + error;
        else
            context.status = "Snapshot diff complete";
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::SetNextItemWidth(180);
    ImGui::InputTextWithHint("##LastChangeAddress", "Address",
                             lastChangeAddress_.data(), lastChangeAddress_.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    ImGui::InputInt("Size##LastChange", &lastChangeSize_);
    lastChangeSize_ = std::clamp(lastChangeSize_, 1, 4096);
    ImGui::SameLine();
    ImGui::BeginDisabled(lastChangeAddress_[0] == '\0');
    if (ImGui::Button("Last change")) {
        std::string error;
        if (!context.snapshotsModel->LastChange(
                lastChangeAddress_.data(), lastChangeSize_, &error))
            context.status = "Last-change query failed: " + error;
        else
            context.status = "Last-change query complete";
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::TextUnformatted("Result");
    ImGui::BeginChild("SnapshotResult", ImVec2(0, 0), ImGuiChildFlags_Borders);
    const auto& result = context.snapshotsModel->ResultJson();
    ImGui::TextWrapped("%s", result.empty() ? "No snapshot operation result yet." : result.c_str());
    ImGui::EndChild();
}

} // namespace cortex::ui
