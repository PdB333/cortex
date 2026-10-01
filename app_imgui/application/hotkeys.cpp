#include "hotkeys.h"

#include <algorithm>
#include <cctype>
#include <sstream>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace cortex::application {
namespace {

constexpr unsigned kModAlt = 0x1;
constexpr unsigned kModControl = 0x2;
constexpr unsigned kModShift = 0x4;
constexpr unsigned kModWin = 0x8;
constexpr int kFirstHotkeyId = 0x4C00;

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return text;
}

std::string Trim(const std::string& text) {
    const auto begin = text.find_first_not_of(" \t");
    if (begin == std::string::npos) return {};
    const auto end = text.find_last_not_of(" \t");
    return text.substr(begin, end - begin + 1);
}

std::vector<HotkeyKey> BuildKeys() {
    std::vector<HotkeyKey> keys;
    static const char* const kLetters[] = {
        "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
        "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z"};
    for (unsigned i = 0; i < 26; ++i) keys.push_back({kLetters[i], 0x41 + i});
    static const char* const kDigits[] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
    for (unsigned i = 0; i < 10; ++i) keys.push_back({kDigits[i], 0x30 + i});
    static const char* const kFunctions[] = {
        "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
        "F13", "F14", "F15", "F16", "F17", "F18", "F19", "F20", "F21", "F22", "F23", "F24"};
    for (unsigned i = 0; i < 24; ++i) keys.push_back({kFunctions[i], 0x70 + i});
    static const char* const kKeypad[] = {
        "Keypad0", "Keypad1", "Keypad2", "Keypad3", "Keypad4",
        "Keypad5", "Keypad6", "Keypad7", "Keypad8", "Keypad9"};
    for (unsigned i = 0; i < 10; ++i) keys.push_back({kKeypad[i], 0x60 + i});
    const HotkeyKey others[] = {
        {"KeypadMultiply", 0x6A}, {"KeypadAdd", 0x6B}, {"KeypadSubtract", 0x6D},
        {"KeypadDecimal", 0x6E}, {"KeypadDivide", 0x6F},
        {"Insert", 0x2D}, {"Delete", 0x2E}, {"Home", 0x24}, {"End", 0x23},
        {"PageUp", 0x21}, {"PageDown", 0x22},
        {"LeftArrow", 0x25}, {"UpArrow", 0x26}, {"RightArrow", 0x27}, {"DownArrow", 0x28},
        {"Space", 0x20}, {"Enter", 0x0D}, {"Tab", 0x09}, {"Backspace", 0x08},
        {"Pause", 0x13}, {"ScrollLock", 0x91}, {"PrintScreen", 0x2C},
        {"Minus", 0xBD}, {"Equal", 0xBB}, {"Comma", 0xBC}, {"Period", 0xBE},
        {"Semicolon", 0xBA}, {"Slash", 0xBF}, {"GraveAccent", 0xC0},
        {"LeftBracket", 0xDB}, {"Backslash", 0xDC}, {"RightBracket", 0xDD}, {"Apostrophe", 0xDE}};
    keys.insert(keys.end(), std::begin(others), std::end(others));
    return keys;
}

} // namespace

const std::vector<HotkeyAction>& HotkeyActions() {
    static const std::vector<HotkeyAction> actions = {
        {"pause_target", "Pause / resume the target"},
        {"attach_foreground", "Attach to the process in the foreground"},
        {"show_cortex", "Bring Cortex to the front"},
        {"scan_next", "Next scan with the current settings"},
        {"scan_exact", "Next scan: exact value"},
        {"scan_increased", "Next scan: increased value"},
        {"scan_decreased", "Next scan: decreased value"},
        {"scan_changed", "Next scan: changed value"},
        {"scan_unchanged", "Next scan: unchanged value"},
        {"scan_undo", "Undo the last scan"},
        {"scan_cancel", "Cancel the running scan"},
        {"freeze_toggle_all", "Freeze / unfreeze the address list"},
    };
    return actions;
}

const std::vector<HotkeyKey>& HotkeyKeys() {
    static const std::vector<HotkeyKey> keys = BuildKeys();
    return keys;
}

bool ParseHotkeyChord(const std::string& text, HotkeyChord& chord) {
    chord = HotkeyChord{};
    std::string token;
    std::istringstream stream(text);
    bool haveKey = false;
    while (std::getline(stream, token, '+')) {
        const std::string part = Lower(Trim(token));
        if (part.empty()) return false;
        if (part == "ctrl" || part == "control") chord.modifiers |= kModControl;
        else if (part == "alt") chord.modifiers |= kModAlt;
        else if (part == "shift") chord.modifiers |= kModShift;
        else if (part == "win" || part == "super") chord.modifiers |= kModWin;
        else {
            if (haveKey) return false;
            const auto& keys = HotkeyKeys();
            const auto found = std::find_if(keys.begin(), keys.end(),
                                            [&](const HotkeyKey& key) { return Lower(key.name) == part; });
            if (found == keys.end()) return false;
            chord.virtualKey = found->virtualKey;
            haveKey = true;
        }
    }
    return haveKey;
}

std::string FormatHotkeyChord(const HotkeyChord& chord) {
    if (!chord.Valid()) return {};
    std::string text;
    if (chord.modifiers & kModControl) text += "Ctrl+";
    if (chord.modifiers & kModAlt) text += "Alt+";
    if (chord.modifiers & kModShift) text += "Shift+";
    if (chord.modifiers & kModWin) text += "Win+";
    const auto& keys = HotkeyKeys();
    const auto found = std::find_if(keys.begin(), keys.end(),
                                    [&](const HotkeyKey& key) { return key.virtualKey == chord.virtualKey; });
    if (found == keys.end()) return {};
    return text + found->name;
}

bool HotkeyChordFromKeys(const std::vector<unsigned>& keys, HotkeyChord& chord) {
    chord = HotkeyChord{};
    for (const unsigned key : keys) {
        switch (key) {
            case 0x10: case 0xA0: case 0xA1: chord.modifiers |= kModShift; break;
            case 0x11: case 0xA2: case 0xA3: chord.modifiers |= kModControl; break;
            case 0x12: case 0xA4: case 0xA5: chord.modifiers |= kModAlt; break;
            case 0x5B: case 0x5C: chord.modifiers |= kModWin; break;
            default:
                if (chord.virtualKey && chord.virtualKey != key) return false;
                chord.virtualKey = key;
                break;
        }
    }
    const auto& known = HotkeyKeys();
    return chord.Valid() && std::any_of(known.begin(), known.end(),
                                        [&](const HotkeyKey& item) { return item.virtualKey == chord.virtualKey; });
}

std::vector<unsigned> HotkeyChordKeys(const HotkeyChord& chord) {
    std::vector<unsigned> keys;
    if (!chord.Valid()) return keys;
    if (chord.modifiers & kModControl) keys.push_back(0x11);
    if (chord.modifiers & kModAlt) keys.push_back(0x12);
    if (chord.modifiers & kModShift) keys.push_back(0x10);
    if (chord.modifiers & kModWin) keys.push_back(0x5B);
    keys.push_back(chord.virtualKey);
    return keys;
}

HotkeyRegistrar::~HotkeyRegistrar() {
    Clear();
}

void HotkeyRegistrar::Clear() {
#if defined(_WIN32)
    for (const auto& entry : actions_) UnregisterHotKey(static_cast<HWND>(window_), entry.first);
#endif
    registered_.clear();
    actions_.clear();
    applied_.clear();
    failures_.clear();
    initialized_ = false;
}

std::vector<std::string> HotkeyRegistrar::Apply(void* window, const std::map<std::string, std::string>& bindings) {
    if (initialized_ && window == window_ && bindings == applied_) return failures_;
    Clear();
    window_ = window;
    applied_ = bindings;
    initialized_ = true;
    int id = kFirstHotkeyId;
    for (const auto& binding : bindings) {
        if (Trim(binding.second).empty()) continue;
        HotkeyChord chord;
        if (!ParseHotkeyChord(binding.second, chord)) {
            failures_.push_back(binding.first);
            continue;
        }
        const std::string normalized = FormatHotkeyChord(chord);
        const auto shared = registered_.find(normalized);
        if (shared != registered_.end()) {
            if (shared->second < 0) failures_.push_back(binding.first);
            else actions_[shared->second].push_back(binding.first);
            continue;
        }
#if defined(_WIN32)
        if (!window || !RegisterHotKey(static_cast<HWND>(window), id, chord.modifiers | MOD_NOREPEAT, chord.virtualKey)) {
            registered_[normalized] = -1;
            failures_.push_back(binding.first);
            continue;
        }
#endif
        registered_[normalized] = id;
        actions_[id].push_back(binding.first);
        ++id;
    }
    return failures_;
}

std::vector<std::string> HotkeyRegistrar::ActionsFor(int id) const {
    const auto found = actions_.find(id);
    return found == actions_.end() ? std::vector<std::string>() : found->second;
}

} // namespace cortex::application
