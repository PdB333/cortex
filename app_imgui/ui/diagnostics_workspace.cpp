#include "diagnostics_workspace.h"

#include <imgui.h>

namespace cortex::ui {

void DiagnosticsWorkspace::Draw(UiContext& context) {
    const auto session = context.sessions ? context.sessions->Active() : nullptr;
    if (!session || !context.diagnosticsModel) {
        ImGui::TextDisabled("Select a process to inspect runtime diagnostics.");
        return;
    }
    if (targetId_ != session->Target().id) {
        targetId_ = session->Target().id;
        context.diagnosticsModel->Reset();
    }

    ImGui::TextUnformatted("Runtime diagnostics");
    ImGui::SameLine();
    if (context.payload && !context.payload->Ready()) {
        if (ImGui::SmallButton("Connect existing")) {
            std::string error;
            if (!context.payload->TryConnectExisting(&error))
                context.status = "Runtime connect failed: " + error;
        }
        ImGui::SameLine();
    }
    if (ImGui::SmallButton("Refresh")) {
        std::string error;
        if (!context.diagnosticsModel->Refresh(&error))
            context.status = "Diagnostics refresh failed: " + error;
        else
            context.status = context.diagnosticsModel->Summary();
    }

    ImGui::Separator();
    ImGui::TextUnformatted(context.diagnosticsModel->Summary().empty()
                               ? "No diagnostic snapshot loaded."
                               : context.diagnosticsModel->Summary().c_str());

    const auto& stats = context.diagnosticsModel->ToolStats();
    ImGui::TextDisabled("Tools: %d total | GET %d | POST %d | DELETE %d | public %d",
                        stats.total, stats.get, stats.post, stats.remove, stats.publicCount);

    ImGui::Spacing();
    ImGui::Text("Hooks (%zu)", context.diagnosticsModel->Hooks().size());
    if (ImGui::BeginTable("DiagnosticHooks", 3,
                          ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_BordersInnerH |
                          ImGuiTableFlags_Resizable,
                          ImVec2(0, 180))) {
        ImGui::TableSetupColumn("Hook", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Installed", ImGuiTableColumnFlags_WidthFixed, 85);
        ImGui::TableSetupColumn("Backend", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const auto& hook : context.diagnosticsModel->Hooks()) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(hook.name.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(hook.installed ? "yes" : "no");
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(hook.backend.empty() ? "-" : hook.backend.c_str());
        }
        ImGui::EndTable();
    }

    if (ImGui::BeginTabBar("DiagnosticsTabs")) {
        if (ImGui::BeginTabItem("Status")) {
            ImGui::BeginChild("DiagnosticsStatus", ImVec2(0, 0), ImGuiChildFlags_Borders);
            ImGui::TextWrapped("%s", context.diagnosticsModel->StatusJson().empty()
                                      ? "No status data."
                                      : context.diagnosticsModel->StatusJson().c_str());
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Health")) {
            ImGui::BeginChild("DiagnosticsHealth", ImVec2(0, 0), ImGuiChildFlags_Borders);
            ImGui::TextWrapped("%s", context.diagnosticsModel->HealthJson().empty()
                                      ? "No health data."
                                      : context.diagnosticsModel->HealthJson().c_str());
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

} // namespace cortex::ui
