#pragma once

#include "ui_context.h"
#include "widgets.h"
#include "workspace_registry.h"

#include <imgui.h>

#include <string>

namespace cortex::ui {

// Shown by a workspace that needs a running process when none is attached.
// It says what the workspace is for, offers to select a process, and points
// to the layouts that need none, so the panel never reads as a dead end.
inline void NoTargetHint(UiContext& context, const char* purpose) {
    ImGui::PushID("NoTargetHint");
    const std::string text = std::string("No process attached. ") + purpose;
    HintText(text.c_str());
    ImGui::Spacing();
    if (ImGui::SmallButton("Select a process...")) context.requestProcessPicker = true;
    FlowSameLine(TextWidth("or"));
    ImGui::TextDisabled("or");
    FlowSameLine(ButtonWidth("Analyze a file instead"));
    if (ImGui::SmallButton("Analyze a file instead"))
        context.requestPreset = static_cast<int>(WorkspacePreset::Files);
    ImGui::PopID();
}

// The Cortex mark (a "C" ring with a probe), drawn with the draw list so it
// needs no texture. `size` is its height in pixels; the cursor moves past it.
inline void CortexMark(float size) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 center(origin.x + size * 0.5f, origin.y + size * 0.5f);
    const float thickness = size * 0.22f;
    const float radius = size * 0.5f - thickness * 0.5f;
    const ImU32 ring = IM_COL32(52, 196, 170, 255);
    const ImU32 probe = IM_COL32(214, 220, 240, 255);
    constexpr float kPi = 3.14159265f;
    draw->PathArcTo(center, radius, kPi * 0.22f, kPi * 1.78f, 32);
    draw->PathStroke(ring, ImDrawFlags_None, thickness);
    const float bar = size * 0.07f;
    draw->AddRectFilled(ImVec2(origin.x - size * 0.05f, center.y - bar * 0.5f),
                        ImVec2(center.x, center.y + bar * 0.5f), probe, bar * 0.5f);
    const float head = size * 0.16f;
    draw->AddRect(ImVec2(center.x - head * 0.5f, center.y - head * 0.5f),
                  ImVec2(center.x + head * 0.5f, center.y + head * 0.5f), probe, 2.0f, 0, size * 0.05f);
    ImGui::Dummy(ImVec2(size, size));
}

} // namespace cortex::ui
