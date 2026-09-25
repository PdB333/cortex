#include "modules_workspace.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>

namespace cortex::ui {
namespace {

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool Matches(const target::ModuleInfo& module, const char* filter) {
    if (!filter || !*filter) return true;
    const std::string q = Lower(filter);
    return Lower(module.name).find(q) != std::string::npos ||
           Lower(module.path).find(q) != std::string::npos;
}

} // namespace

void ModulesWorkspace::Refresh(UiContext& context) {
    if (!context.modules) return;
    std::string error;
    modules_ = context.modules->List(&error);
    context.status = error.empty()
        ? std::to_string(modules_.size()) + " module(s)"
        : "Module enumeration failed: " + error;
}

void ModulesWorkspace::Draw(UiContext& context) {
    const auto session = context.sessions ? context.sessions->Active() : nullptr;
    if (!session) {
        ImGui::TextDisabled("Select a process to inspect modules.");
        return;
    }

    if (targetId_ != session->Target().id) {
        targetId_ = session->Target().id;
        Refresh(context);
    }

    ImGui::TextUnformatted("Loaded modules");
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh")) Refresh(context);
    ImGui::Spacing();
    ImGui::SetNextItemWidth(360);
    ImGui::InputTextWithHint("##ModuleFilter", "Filter modules...", filter_, sizeof(filter_));
    ImGui::Spacing();

    if (ImGui::BeginTable("ModulesTable", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                          ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
                          ImGui::GetContentRegionAvail())) {
        ImGui::TableSetupColumn("Module", ImGuiTableColumnFlags_WidthStretch, 0.25f);
        ImGui::TableSetupColumn("Base", ImGuiTableColumnFlags_WidthFixed, 145);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch, 0.75f);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < modules_.size(); ++i) {
            const auto& module = modules_[i];
            if (!Matches(module, filter_)) continue;
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (ImGui::Selectable(module.name.c_str(), false,
                                  ImGuiSelectableFlags_SpanAllColumns |
                                  ImGuiSelectableFlags_AllowDoubleClick)) {
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    context.NavigateTo("disassembly", module.base);
                }
            }
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Disassemble module entry")) {
                    context.NavigateTo("disassembly", module.base);
                }
                if (ImGui::MenuItem("Browse memory")) {
                    context.NavigateTo("memory-browser", module.base);
                }
                ImGui::EndPopup();
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("0x%llX", static_cast<unsigned long long>(module.base));
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("0x%llX", static_cast<unsigned long long>(module.size));
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(module.path.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

} // namespace cortex::ui
