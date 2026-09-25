#include "overview_workspace.h"

#include <imgui.h>

#include <sstream>

namespace cortex::ui {
namespace {

std::string CapabilitySummary(const target::TargetDescriptor& target) {
    std::ostringstream out;
    const auto names = target.capabilities.Names();
    for (size_t i = 0; i < names.size(); ++i) {
        if (i) out << ", ";
        out << names[i];
    }
    return out.str();
}

} // namespace

void OverviewWorkspace::Draw(UiContext& context) {
    const auto session = context.sessions ? context.sessions->Active() : nullptr;

    ImGui::SetWindowFontScale(1.35f);
    ImGui::TextUnformatted(session ? session->Target().name.c_str()
                                   : "No target selected");
    ImGui::SetWindowFontScale(1.0f);

    if (!session) {
        ImGui::TextDisabled("Choose a process from the target picker to begin.");
        ImGui::Dummy(ImVec2(0, 18));
        if (ImGui::Button("Select process", ImVec2(180, 38)))
            context.requestProcessPicker = true;
        return;
    }

    const auto& target = session->Target();
    ImGui::TextDisabled("PID %llu | %s | %s",
                        static_cast<unsigned long long>(target.processId),
                        target::PlatformName(target.platform),
                        target::ArchitectureName(target.architecture));

    ImGui::Separator();
    ImGui::TextUnformatted("Target");

    if (ImGui::BeginTable("OverviewTarget", 2,
                          ImGuiTableFlags_SizingFixedFit |
                          ImGuiTableFlags_BordersInnerH,
                          ImVec2(0, 220))) {
        ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

        auto row = [](const char* key, const std::string& value) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("%s", key);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(value.c_str());
        };

        row("Platform", target::PlatformName(target.platform));
        row("Architecture", target::ArchitectureName(target.architecture));
        row("Session", session->Alive() ? "active" : "stale");
        row("Attached targets",
            context.sessions ? std::to_string(context.sessions->AttachedTargets().size()) : "0");
        row("Mutation", context.mutationAllowed ? "Enabled" : "Disabled");
        row("Capabilities", CapabilitySummary(target));
        ImGui::EndTable();
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Actions");

    if (ImGui::Button("Change process")) context.requestProcessPicker = true;
    ImGui::SameLine();
    if (ImGui::Button("Sessions")) context.requestWorkspace = "sessions";
    ImGui::SameLine();
    if (ImGui::Button(context.mutationAllowed ? "Disable writes" : "Enable writes"))
        context.mutationAllowed = !context.mutationAllowed;
    ImGui::SameLine();
    if (ImGui::Button("Memory")) context.requestWorkspace = "memory";
    ImGui::SameLine();
    if (ImGui::Button("Debugger")) context.requestWorkspace = "debugger";

    ImGui::Spacing();
    ImGui::TextDisabled("%s", context.status.c_str());
}

} // namespace cortex::ui
