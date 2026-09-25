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
    ImGui::SameLine();
    if (ImGui::SmallButton("Latest crash report")) {
        std::string error;
        const std::string configured =
            context.settings ? context.settings->Values().diagnosticsCrashDirectory
                             : std::string();
        if (!context.diagnosticsModel->LoadLatestCrash(
                session->Target(), configured, &error))
            context.status = "Crash report lookup failed: " + error;
        else if (context.diagnosticsModel->CrashFound())
            context.status = "Crash report: " + context.diagnosticsModel->CrashDirectory();
        else
            context.status = "No crash report found for this target";
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
        if (ImGui::BeginTabItem("Crash report")) {
            if (!context.diagnosticsModel->CrashFound()) {
                ImGui::TextDisabled("Use Latest crash report to load the newest bundle for this process.");
            } else {
                ImGui::TextWrapped("Directory: %s",
                                   context.diagnosticsModel->CrashDirectory().c_str());
                if (ImGui::BeginTabBar("CrashReportTabs")) {
                    if (ImGui::BeginTabItem("Report")) {
                        ImGui::BeginChild("CrashReportJson", ImVec2(0, 0), ImGuiChildFlags_Borders);
                        ImGui::TextWrapped("%s",
                            context.diagnosticsModel->CrashReportJson().empty()
                                ? "No report.json data."
                                : context.diagnosticsModel->CrashReportJson().c_str());
                        ImGui::EndChild();
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Symbolized")) {
                        ImGui::BeginChild("CrashSymbolizedJson", ImVec2(0, 0), ImGuiChildFlags_Borders);
                        ImGui::TextWrapped("%s",
                            context.diagnosticsModel->CrashSymbolizedJson().empty()
                                ? "No symbolized crash data."
                                : context.diagnosticsModel->CrashSymbolizedJson().c_str());
                        ImGui::EndChild();
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Hooks")) {
                        ImGui::BeginChild("CrashHooksJson", ImVec2(0, 0), ImGuiChildFlags_Borders);
                        ImGui::TextWrapped("%s",
                            context.diagnosticsModel->CrashHooksJson().empty()
                                ? "No hook snapshot in the crash bundle."
                                : context.diagnosticsModel->CrashHooksJson().c_str());
                        ImGui::EndChild();
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Breadcrumbs")) {
                        ImGui::BeginChild("CrashBreadcrumbsJson", ImVec2(0, 0), ImGuiChildFlags_Borders);
                        ImGui::TextWrapped("%s",
                            context.diagnosticsModel->CrashBreadcrumbsJson().empty()
                                ? "No breadcrumbs in the crash bundle."
                                : context.diagnosticsModel->CrashBreadcrumbsJson().c_str());
                        ImGui::EndChild();
                        ImGui::EndTabItem();
                    }
                    ImGui::EndTabBar();
                }
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

} // namespace cortex::ui
