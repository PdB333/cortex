#pragma once

#include <array>

namespace cortex::ui {

// The Memory tools, in the order of ToolsWorkspace::Tab. Shared by the
// selector inside the workspace and by the Tools menu, so a tool can be
// opened from either without a row of buttons taking room in the panel.
struct ToolTabInfo {
    const char* request;  // UiContext::toolsTabRequest
    const char* label;
    const char* group;
    const char* hint;
};

inline const std::array<ToolTabInfo, 14>& ToolTabs() {
    static const std::array<ToolTabInfo, 14> kTabs = {{
        {"regions",   "Regions",        "Inspect", "Committed regions with protection and type"},
        {"pe",        "PE headers",     "Inspect", "Sections, directories, exports and imports"},
        {"strings",   "Strings",        "Inspect", "ASCII and UTF-16 strings in a module"},
        {"caves",     "Code caves",     "Inspect", "Runs of padding that can hold code"},
        {"signature", "AOB signature",  "Search",  "Generate and test a wildcard signature"},
        {"pointers",  "Pointer scan",   "Search",  "Find paths that reach an address"},
        {"symbols",   "Symbols",        "Names",   "User symbols and module exports"},
        {"assembler", "Assembler",      "Code",    "Assemble and write, or inject code"},
        {"speedhack", "Speedhack",      "Code",    "Scale the target's clock"},
        {"grouped",   "Grouped scan",   "Search",  "Values that sit close together"},
        {"dump",      "Dump",           "Files",   "Save memory to a file, load it back"},
        {"types",     "Custom types",   "Names",   "Value types of your own"},
        {"dissect",   "Dissect",        "Inspect", "What sits at each offset of a structure"},
        {"spider",    "Pointer spider", "Search",  "Follow the pointers out of an address"},
    }};
    return kTabs;
}

// The groups in the order the combo and the menu list them; the tools of a
// group are not next to each other in ToolTabs(), which follows the enum.
inline const std::array<const char*, 5>& ToolGroups() {
    static const std::array<const char*, 5> kGroups = {"Inspect", "Search", "Code", "Names", "Files"};
    return kGroups;
}

} // namespace cortex::ui
