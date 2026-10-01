#include "actions_workspace.h"
#include "widgets.h"

#include <imgui.h>

#include <algorithm>

namespace cortex::ui {

void ActionsWorkspace::Draw(UiContext& context) {
    const auto session = context.sessions ? context.sessions->Active() : nullptr;
    if (!session || !context.actionsModel) {
        HintText("Select a process to inspect the reversible action journal.");
        return;
    }

    if (targetId_ != session->Target().id) {
        targetId_ = session->Target().id;
        context.actionsModel->Reset();
        rollbackCheckpoint_ = 0;
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Reversible actions");
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
                context.actionsModel->Refresh(nullptr);
        }
        FlowSameLine(ButtonWidth("Enable runtime"));
        ImGui::BeginDisabled(!context.mutationAllowed);
        if (ImGui::Button("Enable runtime")) {
            context.RunInBackground("Loading the Cortex runtime into the target", [=, &context]() {
                std::string error;
                if (!context.payload->EnsureReady(&error))
                    context.status = "Runtime enable failed: " + error;
                else
                    context.actionsModel->Refresh(nullptr);
            });
        }
        ImGui::EndDisabled();
        FlowSameLine(ButtonWidth("Refresh"));
    }

    if (ImGui::Button("Refresh")) {
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
    FlowSameLine(ButtonWidth("Clear history"));
    if (ImGui::Button("Clear history")) {
        std::string error;
        if (!context.actionsModel->Clear(context.mutationAllowed, &error))
            context.status = "Clear actions failed: " + error;
        else
            context.status = "Action history cleared";
    }
    ImGui::EndDisabled();

    FlowSameLine(Px(180));
    ImGui::SetNextItemWidth(Px(180));
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

    if (BeginDataTable("ActionsTable", 3,
                          ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_BordersInnerH |
                          ImGuiTableFlags_Resizable |
                          ImGuiTableFlags_ScrollY,
                          ImGui::GetContentRegionAvail())) {
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, Px(90));
        ImGui::TableSetupColumn("Time (ms)", ImGuiTableColumnFlags_WidthFixed, Px(150));
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
