#pragma once

#include <string>

namespace cortex::ui {

// Native Windows Open and Save As dialogs. filter uses the Win32 format
// ("Cheat tables (*.CT)\0*.CT\0All files\0*.*\0"); paths are UTF-8.
// Both return false when the user cancels.
bool ShowOpenFileDialog(const wchar_t* filter, std::string& path);
bool ShowSaveFileDialog(const wchar_t* filter, const wchar_t* defaultExtension, std::string& path);

} // namespace cortex::ui
