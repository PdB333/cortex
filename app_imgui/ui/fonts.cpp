#include "fonts.h"

#include "theme.h"

#include <filesystem>
#include <initializer_list>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace cortex::ui {
namespace {

// Body text size before DPI scaling. Segoe UI reads well at 15px where the
// built-in bitmap font needed 13px.
constexpr float kBaseFontSize = 15.0f;

std::filesystem::path SystemFontDirectory() {
#if defined(_WIN32)
    wchar_t buffer[MAX_PATH] = {};
    const UINT length = GetWindowsDirectoryW(buffer, MAX_PATH);
    if (length > 0 && length < MAX_PATH) return std::filesystem::path(buffer) / L"Fonts";
#endif
    return {};
}

ImFont* LoadFirstAvailable(std::initializer_list<const char*> fileNames) {
    const std::filesystem::path directory = SystemFontDirectory();
    if (directory.empty()) return nullptr;
    for (const char* fileName : fileNames) {
        const std::filesystem::path path = directory / fileName;
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error)) continue;
        ImFontConfig config;
        config.OversampleH = 2;
        if (ImFont* font = ImGui::GetIO().Fonts->AddFontFromFileTTF(
                path.u8string().c_str(), kBaseFontSize, &config))
            return font;
    }
    return nullptr;
}

}  // namespace

FontSet& Fonts() {
    static FontSet fonts;
    return fonts;
}

void LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    FontSet& fonts = Fonts();

    // The first font added becomes the default font.
    fonts.ui = LoadFirstAvailable({"segoeui.ttf", "tahoma.ttf", "arial.ttf"});
    if (!fonts.ui) fonts.ui = io.Fonts->AddFontDefault();
    fonts.mono = LoadFirstAvailable({"CascadiaMono.ttf", "consola.ttf", "cour.ttf"});
    if (!fonts.mono) fonts.mono = fonts.ui;
    fonts.heading = LoadFirstAvailable({"seguisb.ttf", "segoeuib.ttf"});
    if (!fonts.heading) fonts.heading = fonts.ui;
    io.FontDefault = fonts.ui;
}

void ApplyDpiScale(float scale) {
    if (!(scale > 0.0f)) scale = 1.0f;
    ImGuiStyle& style = ImGui::GetStyle();
    // Start from pristine metrics so repeated DPI changes do not compound.
    style = ImGuiStyle();
    ImGui::StyleColorsDark(&style);
    ApplyCortexTheme();
    style.ScaleAllSizes(scale);
    style.FontSizeBase = kBaseFontSize;
    style.FontScaleDpi = scale;
}

}  // namespace cortex::ui
