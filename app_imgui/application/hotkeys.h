#pragma once

#include <map>
#include <string>
#include <vector>

namespace cortex::application {

// System-wide hotkeys, active while a game has the focus. Bindings are stored
// in the settings as "Ctrl+Alt+F5" style chords keyed by action id; the
// desktop turns a WM_HOTKEY into the action id and dispatches it as a UI
// command.

struct HotkeyAction {
    const char* id;
    const char* label;
};

const std::vector<HotkeyAction>& HotkeyActions();

struct HotkeyChord {
    unsigned modifiers = 0;   // MOD_ALT 1, MOD_CONTROL 2, MOD_SHIFT 4, MOD_WIN 8
    unsigned virtualKey = 0;  // Windows virtual-key code
    bool Valid() const { return virtualKey != 0; }
};

bool ParseHotkeyChord(const std::string& text, HotkeyChord& chord);
std::string FormatHotkeyChord(const HotkeyChord& chord);

// Keys a hotkey can use. Names match ImGui::GetKeyName so the settings page
// can record a chord from the keyboard.
struct HotkeyKey {
    const char* name;
    unsigned virtualKey;
};
const std::vector<HotkeyKey>& HotkeyKeys();

// Registers bindings on a window with RegisterHotKey and maps WM_HOTKEY ids
// back to action ids. Re-applies only when the bindings change.
class HotkeyRegistrar {
public:
    ~HotkeyRegistrar();

    // Returns the bindings that could not be registered (already taken by
    // another program, or not a valid chord).
    std::vector<std::string> Apply(void* window, const std::map<std::string, std::string>& bindings);
    void Clear();
    std::string ActionFor(int id) const;
    const std::vector<std::string>& Failures() const { return failures_; }

private:
    void* window_ = nullptr;
    std::map<std::string, std::string> applied_;
    std::map<int, std::string> actions_;
    std::vector<std::string> failures_;
    bool initialized_ = false;
};

} // namespace cortex::application
