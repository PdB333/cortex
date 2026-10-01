#pragma once

#include "application/hotkeys.h"

#include <imgui.h>

#include <cctype>
#include <string>

namespace cortex::ui {

enum class HotkeyCaptureResult { Waiting, Captured, Cleared, Cancelled, Rejected };

// Records a global hotkey chord from the keys pressed this frame. Escape
// cancels, Backspace alone clears. Letters, digits and editing keys need a
// modifier: a bare global hotkey would take the key from every program.
inline HotkeyCaptureResult CaptureHotkeyChord(std::string& chord, std::string& message) {
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) return HotkeyCaptureResult::Cancelled;
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace, false) && !io.KeyCtrl && !io.KeyAlt && !io.KeyShift) {
        chord.clear();
        return HotkeyCaptureResult::Cleared;
    }
    auto isModifier = [](ImGuiKey key) {
        return key == ImGuiKey_LeftCtrl || key == ImGuiKey_RightCtrl || key == ImGuiKey_LeftShift ||
               key == ImGuiKey_RightShift || key == ImGuiKey_LeftAlt || key == ImGuiKey_RightAlt ||
               key == ImGuiKey_LeftSuper || key == ImGuiKey_RightSuper || key == ImGuiKey_ReservedForModCtrl ||
               key == ImGuiKey_ReservedForModShift || key == ImGuiKey_ReservedForModAlt ||
               key == ImGuiKey_ReservedForModSuper;
    };
    auto needsModifier = [](const std::string& key) {
        if (key.size() >= 2 && key[0] == 'F' && std::isdigit(static_cast<unsigned char>(key[1]))) return false;
        if (key.rfind("Keypad", 0) == 0) return false;
        return key != "Pause" && key != "ScrollLock" && key != "Insert" && key != "Home" && key != "End" &&
               key != "PageUp" && key != "PageDown" && key != "PrintScreen";
    };
    for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; ++key) {
        const auto imguiKey = static_cast<ImGuiKey>(key);
        if (isModifier(imguiKey) || !ImGui::IsKeyPressed(imguiKey, false)) continue;
        const std::string name = ImGui::GetKeyName(imguiKey);
        std::string prefix;
        if (io.KeyCtrl) prefix += "Ctrl+";
        if (io.KeyAlt) prefix += "Alt+";
        if (io.KeyShift) prefix += "Shift+";
        if (io.KeySuper) prefix += "Win+";
        application::HotkeyChord parsed;
        if (!application::ParseHotkeyChord(prefix + name, parsed)) continue;
        if (prefix.empty() && needsModifier(name)) {
            message = "Add Ctrl, Alt or Shift to " + name + ": alone it would block that key everywhere";
            return HotkeyCaptureResult::Rejected;
        }
        chord = application::FormatHotkeyChord(parsed);
        return HotkeyCaptureResult::Captured;
    }
    return HotkeyCaptureResult::Waiting;
}

} // namespace cortex::ui
