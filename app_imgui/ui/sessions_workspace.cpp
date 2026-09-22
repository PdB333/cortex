#include "sessions_workspace.h"

#include <imgui.h>

#include <algorithm>
#include <sstream>

namespace cortex::ui {

std::string SessionsWorkspace::CapabilitySummary(const target::TargetDescriptor& target) const {
    std::ostringstream out;
    const auto names = target.capabilities.Names();
    for (size_t i = 0; i < names.size(); ++i) {
        if (i) out << ", ";
        out << names[i];
    }
    return out.str();
}

void SessionsWorkspace::Refresh(UiContext& context) {
    targets_.clear();
    if (!context.sessions) return;
    context.sessions->PruneDeadSessions();
    targets_ = context.sessions->AttachedTargets();
    std::sort(targets_.begin(), targets_.end(), [](const auto& a, const auto& b) {
        if (a.name != b.name) return a.name < b.name;
        return a.processId < b.processId;
    });
}

void SessionsWorkspace::Draw(UiContext& context) {
    if (!context.sessions) {
        ImGui::TextDisabled("Session manager unavailable.");
        return;
    }

    Refresh(context);

    ImGui::TextUnformatted("Target sessions");
    ImGui::SameLine();
    ImGui::TextDisabled("(%zu attached)", targets_.size());
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh")) Refresh(context);

    ImGui::SameLine();
    ImGui::BeginDisabled(targets_.empty());
    if (ImGui::SmallButton("Detach all")) {
        if (context.debuggerModel) context.debuggerModel->Reset();
        if (context.projectModel) context.projectModel->Reset();
        if (context.symbolsModel) context.symbolsModel->Reset();
        if (context.structuresModel) context.structuresModel->Reset();
        if (context.pointerMapsModel) context.pointerMapsModel->Reset();
        if (context.snapshotsModel) context.snapshotsModel->Reset();
        if (context.reModel) context.reModel->Reset();
        context.sessions->DetachAll();
        if (context.payload) context.payload->Reset();
        context.mutationAllowed = false;
        context.status = "Detached all targets";
        targets_.clear();
    }
    ImGui::EndDisabled();

    ImGui::Spacing();

    if (targets_.empty()) {
        ImGui::TextDisabled("No attached target. Use Select process to create a session.");
        return;
    }

    if (ImGui::BeginTable("SessionsTable", 7,
                          ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_BordersInnerH |
                          ImGuiTableFlags_Resizable |
                          ImGuiTableFlags_ScrollY,
                          ImGui::GetContentRegionAvail())) {
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Process", ImGuiTableColumnFlags_WidthStretch, 0.22f);
        ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Arch", ImGuiTableColumnFlags_WidthFixed, 72.0f);
        ImGui::TableSetupColumn("Generation", ImGuiTableColumnFlags_WidthFixed, 94.0f);
        ImGui::TableSetupColumn("Capabilities", ImGuiTableColumnFlags_WidthStretch, 0.55f);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 170.0f);
        ImGui::TableHeadersRow();

        const std::string activeId = context.sessions->ActiveTargetId();
        for (size_t i = 0; i < targets_.size(); ++i) {
            const auto& target = targets_[i];
            const bool active = target.id == activeId;
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            if (active) ImGui::TextUnformatted("ACTIVE");
            else ImGui::TextDisabled("attached");

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(target.name.c_str());

            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%llu", static_cast<unsigned long long>(target.processId));

            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(target::ArchitectureName(target.architecture));

            ImGui::TableSetColumnIndex(4);
            if (target.generation)
                ImGui::Text("%llu", static_cast<unsigned long long>(target.generation));
            else
                ImGui::TextDisabled("-");

            ImGui::TableSetColumnIndex(5);
            const std::string capabilities = CapabilitySummary(target);
            ImGui::TextUnformatted(capabilities.empty() ? "-" : capabilities.c_str());

            ImGui::TableSetColumnIndex(6);
            ImGui::BeginDisabled(active);
            if (ImGui::SmallButton("Activate")) {
                if (context.sessions->Activate(target.id)) {
                    if (context.debuggerModel) context.debuggerModel->Reset();
                    if (context.projectModel) context.projectModel->Reset();
                    if (context.symbolsModel) context.symbolsModel->Reset();
                    if (context.structuresModel) context.structuresModel->Reset();
                    if (context.pointerMapsModel) context.pointerMapsModel->Reset();
                    if (context.snapshotsModel) context.snapshotsModel->Reset();
                    if (context.reModel) context.reModel->Reset();
        if (context.reModel) context.reModel->Reset();
                    if (context.payload) context.payload->Reset();
                    context.mutationAllowed = false;
                    context.status = "Active target: " + target.name;
                }
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::SmallButton("Detach")) {
                const bool wasActive = target.id == context.sessions->ActiveTargetId();
                if (wasActive && context.debuggerModel) context.debuggerModel->Reset();
                if (wasActive && context.projectModel) context.projectModel->Reset();
                if (wasActive && context.symbolsModel) context.symbolsModel->Reset();
                if (wasActive && context.structuresModel) context.structuresModel->Reset();
                if (wasActive && context.pointerMapsModel) context.pointerMapsModel->Reset();
                if (wasActive && context.snapshotsModel) context.snapshotsModel->Reset();
                if (wasActive && context.reModel) context.reModel->Reset();
                context.sessions->Detach(target.id);
                if (wasActive) {
                    if (context.payload) context.payload->Reset();
                    context.mutationAllowed = false;
                }
                context.status = "Detached " + target.name;
                Refresh(context);
                ImGui::PopID();
                break;
            }

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

} // namespace cortex::ui
