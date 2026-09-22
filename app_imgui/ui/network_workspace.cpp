#include "network_workspace.h"

#include <imgui.h>

namespace cortex::ui {

void NetworkWorkspace::Refresh(UiContext& context, bool reportStatus) {
    if (!context.networkModel) return;
    std::string error;
    if (!context.networkModel->Refresh(&error)) {
        if (reportStatus) context.status = "Network refresh failed: " + error;
        return;
    }
    captureEnabled_ = context.networkModel->CaptureEnabled();
    lastRefresh_ = std::chrono::steady_clock::now();
    if (reportStatus)
        context.status = std::to_string(context.networkModel->Events().size()) +
                         " network event(s)";
}

void NetworkWorkspace::Draw(UiContext& context) {
    const auto session = context.sessions ? context.sessions->Active() : nullptr;
    if (!session || !context.networkModel) {
        ImGui::TextDisabled("Select a process to inspect runtime network traffic.");
        return;
    }
    if (targetId_ != session->Target().id) {
        targetId_ = session->Target().id;
        context.networkModel->Reset();
        captureEnabled_ = false;
        lastRefresh_ = {};
    }

    ImGui::TextUnformatted("Network capture");
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

    ImGui::Separator();

    const bool oldCapture = captureEnabled_;
    ImGui::Checkbox("Capture enabled", &captureEnabled_);
    if (captureEnabled_ != oldCapture) {
        if (!context.mutationAllowed) {
            captureEnabled_ = oldCapture;
            context.status = "Enable writes before changing network capture";
        } else {
            std::string error;
            if (!context.networkModel->SetCapture(
                    captureEnabled_, context.mutationAllowed, &error)) {
                captureEnabled_ = oldCapture;
                context.status = "Network capture update failed: " + error;
            } else {
                captureEnabled_ = context.networkModel->CaptureEnabled();
                context.status = captureEnabled_ ? "Network capture enabled"
                                                 : "Network capture disabled";
            }
        }
    }

    if (context.payload && context.payload->Ready()) {
        const int refreshMs = context.settings ? context.settings->Values().autoRefreshMs : 750;
        const auto now = std::chrono::steady_clock::now();
        if (lastRefresh_.time_since_epoch().count() == 0 ||
            now - lastRefresh_ >= std::chrono::milliseconds(refreshMs))
            Refresh(context, false);
    }

    if (ImGui::BeginTable("NetworkEvents", 6,
                          ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_BordersInnerH |
                          ImGuiTableFlags_Resizable |
                          ImGuiTableFlags_ScrollY,
                          ImGui::GetContentRegionAvail())) {
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 65);
        ImGui::TableSetupColumn("Tick", ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("Dir", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("Socket", ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 75);
        ImGui::TableSetupColumn("Preview", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (const auto& event : context.networkModel->Events()) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%llu", static_cast<unsigned long long>(event.id));
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%llu", static_cast<unsigned long long>(event.tickMs));
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(event.direction.c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%llu", static_cast<unsigned long long>(event.socket));
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%llu", static_cast<unsigned long long>(event.size));
            ImGui::TableSetColumnIndex(5);
            ImGui::TextUnformatted(event.previewHex.c_str());
        }
        ImGui::EndTable();
    }
}

} // namespace cortex::ui
