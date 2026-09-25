#include "events_workspace.h"

#include <imgui.h>

namespace cortex::ui {

void EventsWorkspace::Refresh(UiContext& context, bool reportStatus) {
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
        " runtime event(s), " +
        std::to_string(context.runtimeEventsModel->ApiLog().size()) +
        " API log line(s)";
}

void EventsWorkspace::Draw(UiContext& context) {
    const auto session = context.sessions ? context.sessions->Active() : nullptr;
    if (!session || !context.runtimeEventsModel) {
        ImGui::TextDisabled("Select a process to inspect runtime events.");
        return;
    }

    if (targetId_ != session->Target().id) {
        targetId_ = session->Target().id;
        context.runtimeEventsModel->Reset();
        lastRefresh_ = {};
    }

    ImGui::TextUnformatted("Runtime events / API console");
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
    }

    if (ImGui::SmallButton("Refresh")) Refresh(context, true);
    ImGui::SameLine();
    ImGui::Checkbox("Auto refresh", &autoRefresh_);

    if (autoRefresh_ && context.payload && context.payload->Ready()) {
        const int refreshMs =
            context.settings ? context.settings->Values().autoRefreshMs : 750;
        const auto now = std::chrono::steady_clock::now();
        if (lastRefresh_.time_since_epoch().count() == 0 ||
            now - lastRefresh_ >= std::chrono::milliseconds(refreshMs))
            Refresh(context, false);
    }

    ImGui::Separator();

    if (ImGui::BeginTabBar("RuntimeEventTabs")) {
        if (ImGui::BeginTabItem("Events")) {
            if (ImGui::BeginTable("RuntimeEventsTable", 4,
                                  ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_BordersInnerH |
                                  ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_ScrollY,
                                  ImGui::GetContentRegionAvail())) {
                ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 70);
                ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 120);
                ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, 0.28f);
                ImGui::TableSetupColumn("Data", ImGuiTableColumnFlags_WidthStretch, 0.72f);
                ImGui::TableHeadersRow();

                for (const auto& event : context.runtimeEventsModel->Events()) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%llu", static_cast<unsigned long long>(event.id));
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%llu", static_cast<unsigned long long>(event.timestampMs));
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(event.type.c_str());
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(event.dataJson.c_str());
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("API log")) {
            ImGui::BeginChild("ApiLogBody", ImVec2(0, 0), ImGuiChildFlags_Borders);
            for (const auto& line : context.runtimeEventsModel->ApiLog())
                ImGui::TextUnformatted(line.c_str());
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

} // namespace cortex::ui
