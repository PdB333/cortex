#pragma once

#include <imgui.h>

namespace cortex::ui {

// Muted explanatory text that wraps inside narrow docked panels instead of
// running past the panel edge.
inline void HintText(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

// Secondary caption placed after a panel title. It is only drawn on the same
// line when it fits; otherwise it moves to a tooltip on the title.
inline void Subtitle(const char* text) {
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float needed = ImGui::CalcTextSize(text).x + spacing;
    const bool titleHovered = ImGui::IsItemHovered();
    ImGui::SameLine();
    if (needed <= ImGui::GetContentRegionAvail().x) {
        ImGui::TextDisabled("%s", text);
    } else {
        ImGui::NewLine();
        if (titleHovered) ImGui::SetTooltip("%s", text);
    }
}

}  // namespace cortex::ui
