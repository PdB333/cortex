#include "file_dialog.h"

#include <windows.h>
#include <commdlg.h>

#include <vector>

namespace cortex::ui {
namespace {

std::wstring Widen(const std::string& text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring wide(static_cast<size_t>(length > 0 ? length - 1 : 0), L'\0');
    if (length > 1) MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), length);
    return wide;
}

std::string Narrow(const wchar_t* text) {
    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string narrow(static_cast<size_t>(length > 0 ? length - 1 : 0), '\0');
    if (length > 1) WideCharToMultiByte(CP_UTF8, 0, text, -1, narrow.data(), length, nullptr, nullptr);
    return narrow;
}

bool Run(bool save, const wchar_t* filter, const wchar_t* extension, std::string& path) {
    std::vector<wchar_t> buffer(32768, L'\0');
    const std::wstring initial = Widen(path);
    if (!initial.empty() && initial.size() < buffer.size()) std::copy(initial.begin(), initial.end(), buffer.begin());
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = GetActiveWindow();
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.lpstrDefExt = extension;
    dialog.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR |
                   (save ? OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST : OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST);
    const BOOL ok = save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog);
    if (!ok) return false;
    path = Narrow(buffer.data());
    return true;
}

} // namespace

const wchar_t* ArtifactFileFilter() {
    return L"All files\0*.*\0"
           L"Executables and metadata\0*.exe;*.dll;*.sys;*.elf;*.winmd;*.pdb;*.tlb\0"
           L"Archives and packages\0*.asar;*.zip;*.jar;*.apk;*.nupkg;*.whl;*.cab;*.msi;*.msix;*.appx;*.msixbundle;*.appxbundle;*.tar;*.gz;*.tgz;*.a;*.lib;*.deb\0"
           L"Extensions and language packages\0*.crx;*.xpi;*.vsix;*.war;*.ear;*.jmod;*.pyz\0"
           L"Certificates and catalogs\0*.cer;*.crt;*.der;*.pem;*.p7b;*.p7m;*.cat\0"
           L"Dumps and registry hives\0*.dmp;*.hiv;*.regf\0";
}

bool ShowOpenFileDialog(const wchar_t* filter, std::string& path) {
    return Run(false, filter, nullptr, path);
}

bool ShowSaveFileDialog(const wchar_t* filter, const wchar_t* defaultExtension, std::string& path) {
    return Run(true, filter, defaultExtension, path);
}

} // namespace cortex::ui
