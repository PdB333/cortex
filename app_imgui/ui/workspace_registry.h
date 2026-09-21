#pragma once

#include "workspace.h"

#include <imgui.h>

#include <memory>
#include <string>
#include <utility>
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

    bool Select(const std::string& id) {
        for (size_t i = 0; i < workspaces_.size(); ++i) {
            if (id == workspaces_[i]->Id()) {
                active_ = i;
                return true;
            }
        }
        return false;
    }

    const char* ActiveId() const {
        return workspaces_.empty() || active_ >= workspaces_.size()
            ? "" : workspaces_[active_]->Id();
    }

    void DrawNavigation() {
        for (size_t i = 0; i < workspaces_.size(); ++i) {
            if (i != 0) ImGui::SameLine();
            const bool active = i == active_;
            if (active) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            }
            if (ImGui::Button(workspaces_[i]->Title())) active_ = i;
            if (active) ImGui::PopStyleColor();
        }
        if (!workspaces_.empty()) ImGui::Separator();
    }

    void DrawActive(UiContext& context) {
        if (!context.requestWorkspace.empty()) {
            Select(context.requestWorkspace);
            context.requestWorkspace.clear();
        }
        if (workspaces_.empty()) return;
        if (active_ >= workspaces_.size()) active_ = 0;
        workspaces_[active_]->Draw(context);
    }

private:
    std::vector<std::unique_ptr<IWorkspace>> workspaces_;
    size_t active_ = 0;
};

} // namespace cortex::ui
