#include "theme.h"

#include <imgui.h>

namespace cortex::ui {

void ApplyCortexTheme() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(14.0f, 12.0f);
    style.FramePadding = ImVec2(10.0f, 7.0f);
    style.CellPadding = ImVec2(9.0f, 6.0f);
    style.ItemSpacing = ImVec2(8.0f, 7.0f);
    style.ItemInnerSpacing = ImVec2(7.0f, 6.0f);
    style.ScrollbarSize = 13.0f;
    style.GrabMinSize = 10.0f;

    style.WindowRounding = 8.0f;
    style.ChildRounding = 7.0f;
    style.FrameRounding = 6.0f;
    style.PopupRounding = 7.0f;
    style.ScrollbarRounding = 8.0f;
    style.GrabRounding = 6.0f;
    style.TabRounding = 6.0f;

    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;

    ImVec4* c = style.Colors;
    c[ImGuiCol_Text]                 = ImVec4(0.91f, 0.93f, 0.95f, 1.00f);
    c[ImGuiCol_TextDisabled]         = ImVec4(0.48f, 0.52f, 0.57f, 1.00f);
    c[ImGuiCol_WindowBg]             = ImVec4(0.055f, 0.064f, 0.078f, 1.00f);
    c[ImGuiCol_ChildBg]              = ImVec4(0.071f, 0.082f, 0.098f, 1.00f);
    c[ImGuiCol_PopupBg]              = ImVec4(0.065f, 0.075f, 0.090f, 0.98f);
    c[ImGuiCol_Border]               = ImVec4(0.16f, 0.19f, 0.23f, 1.00f);
    c[ImGuiCol_FrameBg]              = ImVec4(0.095f, 0.110f, 0.132f, 1.00f);
    c[ImGuiCol_FrameBgHovered]       = ImVec4(0.125f, 0.147f, 0.176f, 1.00f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.145f, 0.170f, 0.202f, 1.00f);
    c[ImGuiCol_TitleBg]              = ImVec4(0.055f, 0.064f, 0.078f, 1.00f);
    c[ImGuiCol_TitleBgActive]        = ImVec4(0.055f, 0.064f, 0.078f, 1.00f);
    c[ImGuiCol_Button]               = ImVec4(0.075f, 0.38f, 0.39f, 1.00f);
    c[ImGuiCol_ButtonHovered]        = ImVec4(0.090f, 0.46f, 0.47f, 1.00f);
    c[ImGuiCol_ButtonActive]         = ImVec4(0.070f, 0.32f, 0.33f, 1.00f);
    c[ImGuiCol_Header]               = ImVec4(0.075f, 0.29f, 0.31f, 0.75f);
    c[ImGuiCol_HeaderHovered]        = ImVec4(0.090f, 0.38f, 0.40f, 0.85f);
    c[ImGuiCol_HeaderActive]         = ImVec4(0.085f, 0.43f, 0.44f, 1.00f);
    c[ImGuiCol_CheckMark]            = ImVec4(0.32f, 0.83f, 0.80f, 1.00f);
    c[ImGuiCol_SliderGrab]           = ImVec4(0.27f, 0.70f, 0.68f, 1.00f);
    c[ImGuiCol_SliderGrabActive]     = ImVec4(0.35f, 0.87f, 0.83f, 1.00f);
    c[ImGuiCol_Separator]            = ImVec4(0.15f, 0.18f, 0.22f, 1.00f);
    c[ImGuiCol_ResizeGrip]           = ImVec4(0.12f, 0.42f, 0.43f, 0.30f);
    c[ImGuiCol_ResizeGripHovered]    = ImVec4(0.16f, 0.55f, 0.55f, 0.65f);
    c[ImGuiCol_ResizeGripActive]     = ImVec4(0.20f, 0.65f, 0.64f, 0.90f);
    c[ImGuiCol_Tab]                  = ImVec4(0.080f, 0.095f, 0.115f, 1.00f);
    c[ImGuiCol_TabHovered]           = ImVec4(0.10f, 0.36f, 0.37f, 1.00f);
    c[ImGuiCol_TabSelected]          = ImVec4(0.075f, 0.29f, 0.31f, 1.00f);
    c[ImGuiCol_TableHeaderBg]        = ImVec4(0.090f, 0.105f, 0.126f, 1.00f);
    c[ImGuiCol_TableBorderStrong]    = ImVec4(0.16f, 0.19f, 0.23f, 1.00f);
    c[ImGuiCol_TableBorderLight]     = ImVec4(0.11f, 0.13f, 0.16f, 1.00f);
    c[ImGuiCol_TableRowBgAlt]        = ImVec4(1.0f, 1.0f, 1.0f, 0.018f);
    c[ImGuiCol_NavHighlight]         = ImVec4(0.25f, 0.76f, 0.73f, 0.85f);
}

} // namespace cortex::ui
