#pragma once

#include "workspace.h"

#include <array>
#include <string>

namespace cortex::ui {

class ProjectWorkspace final : public IWorkspace {
public:
    const char* Id() const override { return "project"; }
    const char* Title() const override { return "Project"; }
    void Draw(UiContext& context) override;

private:
    bool Refresh(UiContext& context);
    bool Navigate(UiContext& context, const std::string& expression,
                  const char* workspace);
    void ClearAddressForm();
    void ClearPointerForm();
    void ClearNoteForm();

    std::string targetId_;
    std::array<char, 128> addressName_ = {};
    std::array<char, 160> addressExpression_ = {};
    std::array<char, 64> addressType_ = {};
    std::array<char, 256> addressNotes_ = {};

    std::array<char, 128> pointerName_ = {};
    std::array<char, 128> pointerModule_ = {};
    std::array<char, 80> pointerBase_ = {};
    std::array<char, 256> pointerOffsets_ = {};
    std::array<char, 64> pointerType_ = {};
    std::array<char, 256> pointerNotes_ = {};

    std::array<char, 512> noteText_ = {};
    std::array<char, 256> noteTags_ = {};
};

} // namespace cortex::ui
