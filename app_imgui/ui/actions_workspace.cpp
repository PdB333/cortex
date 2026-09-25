#include "actions_workspace.h"

#include <imgui.h>

namespace cortex::ui {

void ActionsWorkspace::Draw(UiContext& context) {
    const auto session = context.sessions ? context.sessions->Active() : nullptr;
    if (!session || !context.actionsModel) {
        ImGui::TextDisabled("Select a process to inspect the reversible action journal.");
        return;
    }

    if (targetId_ != session->Target().id) {
        targetId_ = session->Target().id;
        context.actionsModel->Reset();
        rollbackCheckpoint_ = 0;
    }

    ImGui::TextUnformatted("Reversible actions");
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
                context.actionsModel->Refresh(nullptr);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!context.mutationAllowed);
        if (ImGui::SmallButton("Enable runtime")) {
            std::string error;
            if (!context.payload->EnsureReady(&error))
                context.status = "Runtime enable failed: " + error;
            else
                context.actionsModel->Refresh(nullptr);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
    }

    if (ImGui::SmallButton("Refresh")) {
        std::string error;
        if (!context.actionsModel->Refresh(&error))
            context.status = "Actions refresh failed: " + error;
        else {
            rollbackCheckpoint_ = context.actionsModel->Checkpoint();
            context.status = std::to_string(context.actionsModel->Actions().size()) +
                             " reversible action(s)";
        }
    }

    ImGui::SameLine();
    ImGui::TextDisabled("checkpoint %llu",
                        static_cast<unsigned long long>(context.actionsModel->Checkpoint()));

    ImGui::Separator();

    ImGui::BeginDisabled(!context.mutationAllowed || context.actionsModel->Actions().empty());
    if (ImGui::Button("Rollback all")) {
        std::string error;
        if (!context.actionsModel->RollbackAll(context.mutationAllowed, &error))
            context.status = "Rollback all failed: " + error;
        else
            context.status = "All reversible actions rolled back";
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear history")) {
        std::string error;
        if (!context.actionsModel->Clear(context.mutationAllowed, &error))
            context.status = "Clear actions failed: " + error;
        else
            context.status = "Action history cleared";
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::SetNextItemWidth(180);
    ImGui::InputScalar("Rollback checkpoint", ImGuiDataType_U64, &rollbackCheckpoint_);
    ImGui::SameLine();
    ImGui::BeginDisabled(!context.mutationAllowed ||
                         rollbackCheckpoint_ > context.actionsModel->Checkpoint());
    if (ImGui::Button("Rollback to")) {
        std::string error;
        if (!context.actionsModel->RollbackTo(
                rollbackCheckpoint_, context.mutationAllowed, &error))
            context.status = "Rollback to checkpoint failed: " + error;
        else
            context.status = "Rolled back to checkpoint " +
                             std::to_string(rollbackCheckpoint_);
    }
    ImGui::EndDisabled();

    if (ImGui::BeginTable("ActionsTable", 3,
                          ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_BordersInnerH |
                          ImGuiTableFlags_Resizable |
                          ImGuiTableFlags_ScrollY,
                          ImGui::GetContentRegionAvail())) {
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("Time (ms)", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableSetupColumn("Reversible action", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (const auto& row : context.actionsModel->Actions()) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (ImGui::Selectable(std::to_string(row.id).c_str(), false,
                                  ImGuiSelectableFlags_SpanAllColumns))
                rollbackCheckpoint_ = row.id;
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%llu", static_cast<unsigned long long>(row.timestampMs));
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(row.description.c_str());
        }
        ImGui::EndTable();
    }
}

} // namespace cortex::ui
