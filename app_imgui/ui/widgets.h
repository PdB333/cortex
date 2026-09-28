#pragma once

#include "fonts.h"

#include <imgui.h>

#include <cstdarg>
#include <utility>

namespace cortex::ui {

// Converts a layout length written for the original 13px font at 100% into
// pixels for the current font size and monitor DPI. Every hard-coded width,
// height or threshold in the UI goes through this so layouts scale together
// with the text.
inline float Px(float value) {
    const ImGuiStyle& style = ImGui::GetStyle();
    return value * style.FontScaleDpi * (style.FontSizeBase / 13.0f);
}

// Monospace text for addresses, bytes and instructions, so columns of hex
// line up row to row while the rest of the UI uses the proportional face.
inline void MonoTextUnformatted(const char* text) {
    MonoFont mono;
    ImGui::TextUnformatted(text);
}

inline void MonoText(const char* format, ...) IM_FMTARGS(1);
inline void MonoText(const char* format, ...) {
    MonoFont mono;
    va_list args;
    va_start(args, format);
    ImGui::TextV(format, args);
    va_end(args);
}

// Code and JSON editors use the monospace face.
template <typename... Args>
bool MonoInputTextMultiline(Args&&... args) {
    MonoFont mono;
    return ImGui::InputTextMultiline(std::forward<Args>(args)...);
}

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

// Width of a regular or small button for a given label, matching ImGui's own
// size calculation (ignores anything after "##").
inline float ButtonWidth(const char* label) {
    return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
}

inline float CheckboxWidth(const char* label) {
    return ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x +
           ImGui::CalcTextSize(label, nullptr, true).x;
}

inline float TextWidth(const char* text) { return ImGui::CalcTextSize(text).x; }

// SameLine() for inline forms and toolbars: keeps the next item on the current
// line only when it fits, otherwise lets it wrap to a new line instead of
// running past the panel edge.
inline void FlowSameLine(float nextWidth) {
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float lineEnd = ImGui::GetItemRectMax().x;
    const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    if (lineEnd + spacing + nextWidth <= right) ImGui::SameLine();
}

// BeginTable for data grids docked in panels of any width. A scrolling table
// that would give its columns less than a readable width scrolls sideways
// instead of truncating every header to "A...". Non-scrolling tables are left
// untouched because horizontal scrolling would also change their height.
inline bool BeginDataTable(const char* id, int columns, ImGuiTableFlags flags = 0,
                           const ImVec2& outerSize = ImVec2(0.0f, 0.0f),
                           float minColumnWidth = 120.0f) {
    float innerWidth = 0.0f;
    if ((flags & ImGuiTableFlags_ScrollY) && !(flags & ImGuiTableFlags_ScrollX)) {
        const float minWidth = static_cast<float>(columns) * Px(minColumnWidth);
        const float available =
            outerSize.x > 0.0f ? outerSize.x : ImGui::GetContentRegionAvail().x;
        if (available < minWidth) {
            flags |= ImGuiTableFlags_ScrollX;
            innerWidth = minWidth;
        }
    }
    return ImGui::BeginTable(id, columns, flags, outerSize, innerWidth);
}

}  // namespace cortex::ui
