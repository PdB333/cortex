#include "bottom_panel_workspace.h"

#include <imgui.h>

#include <algorithm>

namespace cortex::ui {

void BottomPanelWorkspace::RefreshRuntime(UiContext& context, bool reportStatus) {
    if (!context.runtimeEventsModel) return;

    std::string eventError;
    std::string logError;
    const bool eventsOk = context.runtimeEventsModel->RefreshEvents(&eventError);
    const bool logOk = context.runtimeEventsModel->RefreshApiLog(&logError);
    lastRefresh_ = std::chrono::steady_clock::now();

    if (!reportStatus) return;
    if (!eventsOk) context.status = "Runtime events refresh failed: " + eventError;
    else if (!logOk) context.status = "API log refresh failed: " + logError;
    else context.status =
        std::to_string(context.runtimeEventsModel->Events().size()) +
        " event(s), " +
        std::to_string(context.runtimeEventsModel->ApiLog().size()) +
        " console line(s)";
}

void BottomPanelWorkspace::Draw(UiContext& context) {
    const auto session = context.sessions ? context.sessions->Active() : nullptr;

    if (session && context.payload && context.payload->Ready() && autoRefresh_) {
        const int refreshMs =
            context.settings ? context.settings->Values().autoRefreshMs : 750;
        const auto now = std::chrono::steady_clock::now();
        if (lastRefresh_.time_since_epoch().count() == 0 ||
            now - lastRefresh_ >= std::chrono::milliseconds(refreshMs))
            RefreshRuntime(context, false);
    }

    if (context.aiActivityModel) {
        ImGui::Text("AI: %zu session(s), %zu active",
                    context.aiActivityModel->SessionCount(),
                    context.aiActivityModel->ActiveTaskCount());
        ImGui::SameLine();
        ImGui::TextDisabled(context.aiActivityModel->Listening()
                                ? "listener ready" : "listener unavailable");
    } else {
        ImGui::TextDisabled("AI activity model unavailable");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Auto refresh runtime", &autoRefresh_);
    ImGui::SameLine();
    ImGui::BeginDisabled(!session || !context.payload || !context.payload->Ready());
    if (ImGui::SmallButton("Refresh runtime"))
        RefreshRuntime(context, true);
    ImGui::EndDisabled();

    ImGui::Separator();

    if (!ImGui::BeginTabBar("CortexBottomPanelTabs")) return;

    if (ImGui::BeginTabItem("Events")) {
        if (!session) {
            ImGui::TextDisabled("Select a process to inspect runtime events.");
        } else if (!context.runtimeEventsModel) {
            ImGui::TextDisabled("Runtime event model unavailable.");
        } else if (ImGui::BeginTable(
                       "BottomEvents", 4,
                       ImGuiTableFlags_RowBg |
                       ImGuiTableFlags_BordersInnerH |
                       ImGuiTableFlags_Resizable |
                       ImGuiTableFlags_ScrollY,
                       ImGui::GetContentRegionAvail())) {
            ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 65);
            ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 105);
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, 0.30f);
            ImGui::TableSetupColumn("Data", ImGuiTableColumnFlags_WidthStretch, 0.70f);
            ImGui::TableHeadersRow();
            const auto& rows = context.runtimeEventsModel->Events();
            const size_t start = rows.size() > 200 ? rows.size() - 200 : 0;
            for (size_t i = start; i < rows.size(); ++i) {
                const auto& row = rows[i];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%llu", static_cast<unsigned long long>(row.id));
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%llu", static_cast<unsigned long long>(row.timestampMs));
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(row.type.c_str());
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(row.dataJson.c_str());
            }
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Console")) {
        if (!session) {
            ImGui::TextDisabled("Select a process to inspect the runtime API log.");
        } else if (context.runtimeEventsModel) {
            ImGui::BeginChild("BottomConsole", ImVec2(0, 0), ImGuiChildFlags_Borders);
            for (const auto& line : context.runtimeEventsModel->ApiLog())
                ImGui::TextUnformatted(line.c_str());
            ImGui::EndChild();
        }
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Breakpoints")) {
        if (!session || !context.debuggerModel) {
            ImGui::TextDisabled("Select a process to inspect breakpoints.");
        } else {
            const auto& rows = context.debuggerModel->Breakpoints();
            ImGui::Text("%zu breakpoint(s)", rows.size());
            if (ImGui::BeginTable(
                    "BottomBreakpoints", 5,
                    ImGuiTableFlags_RowBg |
                    ImGuiTableFlags_BordersInnerH |
                    ImGuiTableFlags_Resizable |
                    ImGuiTableFlags_ScrollY,
                    ImGui::GetContentRegionAvail())) {
                ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 45);
                ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 145);
                ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 110);
                ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 70);
                ImGui::TableSetupColumn("Hits", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();
                for (const auto& bp : rows) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%d", bp.id);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("0x%llX", static_cast<unsigned long long>(bp.address));
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(bp.kind.c_str());
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(bp.pauseOnHit ? "pause" : "log");
                    ImGui::TableSetColumnIndex(4);
                    ImGui::Text("%llu", static_cast<unsigned long long>(bp.hitCount));
                }
                ImGui::EndTable();
            }
        }
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Watches")) {
        if (!session || !context.watchesModel) {
            ImGui::TextDisabled("Select a process to inspect watches.");
        } else {
            if (ImGui::SmallButton("Refresh watches")) {
                std::string error;
                if (!context.watchesModel->Refresh(&error))
                    context.status = "Watch refresh failed: " + error;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%zu watch(es), %zu freeze(s)",
                                context.watchesModel->Watches().size(),
                                context.watchesModel->Freezes().size());
            if (ImGui::BeginTable(
                    "BottomWatches", 4,
                    ImGuiTableFlags_RowBg |
                    ImGuiTableFlags_BordersInnerH |
                    ImGuiTableFlags_Resizable |
                    ImGuiTableFlags_ScrollY,
                    ImGui::GetContentRegionAvail())) {
                ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch, 0.28f);
                ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch, 0.30f);
                ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 90);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.42f);
                ImGui::TableHeadersRow();
                for (const auto& watch : context.watchesModel->Watches()) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(watch.label.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(watch.address.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(watch.type.c_str());
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(watch.hasValue ? watch.value.c_str() : "-");
                }
                ImGui::EndTable();
            }
        }
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("AI Activity")) {
        if (!context.aiActivityModel) {
            ImGui::TextDisabled("AI activity model unavailable.");
        } else {
            if (ImGui::SmallButton("Clear history"))
                context.aiActivityModel->ClearHistory();
            ImGui::SameLine();
            ImGui::TextDisabled("%zu row(s)",
                                context.aiActivityModel->Activities().size());

            if (ImGui::BeginTable(
                    "BottomAiActivity", 6,
                    ImGuiTableFlags_RowBg |
                    ImGuiTableFlags_BordersInnerH |
                    ImGuiTableFlags_Resizable |
                    ImGuiTableFlags_ScrollY,
                    ImGui::GetContentRegionAvail())) {
                ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 110);
                ImGui::TableSetupColumn("Client", ImGuiTableColumnFlags_WidthFixed, 140);
                ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 75);
                ImGui::TableSetupColumn("Phase", ImGuiTableColumnFlags_WidthFixed, 85);
                ImGui::TableSetupColumn("Tool", ImGuiTableColumnFlags_WidthStretch, 0.35f);
                ImGui::TableSetupColumn("Summary", ImGuiTableColumnFlags_WidthStretch, 0.65f);
                ImGui::TableHeadersRow();

                for (const auto& row : context.aiActivityModel->Activities()) {
                    ImGui::PushID(static_cast<int>(row.sequence));
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%llu", static_cast<unsigned long long>(row.timestampMs));
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(row.client.empty() ? "-" : row.client.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(row.kind.c_str());
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(row.phase.c_str());
                    ImGui::TableSetColumnIndex(4);
                    ImGui::TextUnformatted(row.tool.empty() ? "-" : row.tool.c_str());
                    ImGui::TableSetColumnIndex(5);
                    ImGui::TextUnformatted(row.summary.empty() ? "-" : row.summary.c_str());
                    if (ImGui::IsItemHovered()) {
                        ImGui::BeginTooltip();
                        ImGui::TextWrapped("%s", row.detailsJson.c_str());
                        ImGui::EndTooltip();
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        }
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Diagnostics")) {
        if (!session || !context.diagnosticsModel) {
            ImGui::TextDisabled("Select a process to inspect diagnostics.");
        } else {
            if (ImGui::SmallButton("Refresh diagnostics")) {
                std::string error;
                if (!context.diagnosticsModel->Refresh(&error))
                    context.status = "Diagnostics refresh failed: " + error;
            }
            ImGui::TextWrapped("%s",
                               context.diagnosticsModel->Summary().empty()
                                   ? "No diagnostic summary loaded."
                                   : context.diagnosticsModel->Summary().c_str());
            if (!context.diagnosticsModel->Hooks().empty()) {
                ImGui::Separator();
                for (const auto& hook : context.diagnosticsModel->Hooks()) {
                    ImGui::BulletText("%s: %s (%s)",
                                      hook.name.c_str(),
                                      hook.installed ? "installed" : "inactive",
                                      hook.backend.empty() ? "-" : hook.backend.c_str());
                }
            }
        }
        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
}

} // namespace cortex::ui
