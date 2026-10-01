#pragma once

#include "workspace.h"

#include <array>
#include <chrono>
#include <string>

namespace cortex::ui {

class AddressesWorkspace final : public IWorkspace {
public:
    const char* Id() const override { return "addresses"; }
    const char* Title() const override { return "Addresses"; }
    void Draw(UiContext& context) override;

private:
    bool RefreshAll(UiContext& context, bool reportStatus);
    bool ResolveExpression(UiContext& context, const std::string& expression,
                           uint64_t& address) const;
    bool Navigate(UiContext& context, const std::string& expression,
                  const char* workspace) const;
    void ClearEditor();
    void BeginEdit(const application::ProjectAddress& row);
    bool SaveEditor(UiContext& context);
    bool RemoveSelected(UiContext& context);
    void SyncSelection();
    void HandleNavigation(UiContext& context, uint64_t address);
    void HandleShortcuts(UiContext& context);

    std::string targetId_;
    int selectedIndex_ = -1;
    std::string editingOriginalName_;

    std::array<char, 160> name_ = {};
    std::array<char, 220> address_ = {};
    std::array<char, 320> notes_ = {};
    int typeIndex_ = 0;

    std::chrono::steady_clock::time_point lastWatchRefresh_{};
};

} // namespace cortex::ui
