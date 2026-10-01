#pragma once

#include <imgui.h>

namespace cortex::ui {

// Fonts used by the desktop UI. `ui` is the default proportional face,
// `mono` is for addresses, bytes, disassembly and code, and `heading` for
// titles. Any of them may fall back to ImGui's built-in font.
struct FontSet {
    ImFont* ui = nullptr;
    ImFont* mono = nullptr;
    ImFont* heading = nullptr;
};

FontSet& Fonts();

// Loads the Windows system faces (Segoe UI, Cascadia Mono/Consolas) into the
// atlas, falling back to ImGui's default font when a face is missing. Must be
// called once after ImGui::CreateContext().
void LoadFonts();

// Applies a monitor DPI scale: resets the theme metrics, scales them, and
// sets the global font scale. Safe to call again when the DPI changes.
void ApplyDpiScale(float scale);

// Scoped monospace font for data columns, hex bytes, disassembly and code.
class MonoFont {
public:
    MonoFont() : pushed_(Fonts().mono != nullptr) {
        if (pushed_) ImGui::PushFont(Fonts().mono, 0.0f);
    }
    ~MonoFont() {
        if (pushed_) ImGui::PopFont();
    }
    MonoFont(const MonoFont&) = delete;
    MonoFont& operator=(const MonoFont&) = delete;

private:
    bool pushed_;
};

// Scoped heading font, one step larger than body text.
class HeadingFont {
public:
    HeadingFont() {
        ImGui::PushFont(Fonts().heading, ImGui::GetStyle().FontSizeBase * 1.2f);
    }
    ~HeadingFont() { ImGui::PopFont(); }
    HeadingFont(const HeadingFont&) = delete;
    HeadingFont& operator=(const HeadingFont&) = delete;
};

}  // namespace cortex::ui
