#pragma once

#include "workspace.h"

#include <imgui.h>

#include <memory>
#include <vector>

namespace cortex::ui {

class WorkspaceRegistry {
public:
    template <typename T, typename... Args>
    T& Add(Args&&... args) {
        auto workspace = std::make_unique<T>(std::forward<Args>(args)...);
        T& reference = *workspace;
        workspaces_.push_back(std::move(workspace));
        return reference;
    }

    void DrawNavigation() {
        if (workspaces_.size() <= 1) return;
        for (size_t i = 0; i < workspaces_.size(); ++i) {
            if (i != 0) ImGui::SameLine();
            const bool active = i == active_;
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button(workspaces_[i]->Title())) active_ = i;
            if (active) ImGui::PopStyleColor();
        }
    }

    void DrawActive(UiContext& context) {
        if (workspaces_.empty()) return;
        if (active_ >= workspaces_.size()) active_ = 0;
        workspaces_[active_]->Draw(context);
    }

private:
    std::vector<std::unique_ptr<IWorkspace>> workspaces_;
    size_t active_ = 0;
};

} // namespace cortex::ui
