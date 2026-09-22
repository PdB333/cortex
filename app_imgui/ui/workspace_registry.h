#pragma once

#include "workspace.h"

#include <imgui.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cortex::ui {

enum class WorkspacePreset {
    Memory,
    Debug,
    ReverseEngineering,
    Trace,
    Automation,
    Runtime
};

class WorkspaceRegistry {
public:
    template <typename T, typename... Args>
    T& Add(Args&&... args) {
        Entry entry;
        entry.workspace = std::make_unique<T>(std::forward<Args>(args)...);
        T& reference = static_cast<T&>(*entry.workspace);
        entries_.push_back(std::move(entry));
        return reference;
    }

    bool Select(const std::string& id) {
        for (auto& entry : entries_) {
            if (id == entry.workspace->Id()) {
                entry.open = true;
                pendingFocus_ = id;
                return true;
            }
        }
        return false;
    }

    bool IsOpen(const std::string& id) const {
        for (const auto& entry : entries_)
            if (id == entry.workspace->Id()) return entry.open;
        return false;
    }

    void SetOpen(const std::string& id, bool open) {
        for (auto& entry : entries_) {
            if (id == entry.workspace->Id()) {
                entry.open = open;
                return;
            }
        }
    }

    void ApplyPreset(WorkspacePreset preset) {
        for (auto& entry : entries_) entry.open = false;

        switch (preset) {
            case WorkspacePreset::Memory:
                OpenMany({"memory", "memory-browser", "modules"});
                break;
            case WorkspacePreset::Debug:
                OpenMany({"disassembly", "debugger", "memory-browser", "modules"});
                break;
            case WorkspacePreset::ReverseEngineering:
                OpenMany({"disassembly", "memory-browser", "modules", "runtime"});
                break;
            case WorkspacePreset::Trace:
                OpenMany({"disassembly", "debugger", "runtime"});
                break;
            case WorkspacePreset::Automation:
                OpenMany({"runtime"});
                break;
            case WorkspacePreset::Runtime:
                OpenMany({"runtime", "modules", "sessions"});
                break;
        }

        preset_ = preset;
    }

    WorkspacePreset Preset() const { return preset_; }

    static const char* PresetName(WorkspacePreset preset) {
        switch (preset) {
            case WorkspacePreset::Memory: return "Memory";
            case WorkspacePreset::Debug: return "Debug";
            case WorkspacePreset::ReverseEngineering: return "RE";
            case WorkspacePreset::Trace: return "Trace";
            case WorkspacePreset::Automation: return "Automation";
            case WorkspacePreset::Runtime: return "Runtime";
        }
        return "Memory";
    }

    void DrawPresetButtons() {
        constexpr WorkspacePreset presets[] = {
            WorkspacePreset::Memory,
            WorkspacePreset::Debug,
            WorkspacePreset::ReverseEngineering,
            WorkspacePreset::Trace,
            WorkspacePreset::Automation,
            WorkspacePreset::Runtime
        };

        for (size_t i = 0; i < IM_ARRAYSIZE(presets); ++i) {
            if (i != 0) ImGui::SameLine();
            const bool active = presets[i] == preset_;
            if (active)
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button(PresetName(presets[i]))) ApplyPreset(presets[i]);
            if (active) ImGui::PopStyleColor();
        }
    }

    void DrawViewMenu() {
        for (auto& entry : entries_) {
            bool open = entry.open;
            if (ImGui::MenuItem(entry.workspace->Title(), nullptr, &open))
                entry.open = open;
        }
    }

    void DrawPresetMenu() {
        constexpr WorkspacePreset presets[] = {
            WorkspacePreset::Memory,
            WorkspacePreset::Debug,
            WorkspacePreset::ReverseEngineering,
            WorkspacePreset::Trace,
            WorkspacePreset::Automation,
            WorkspacePreset::Runtime
        };
        for (const auto preset : presets) {
            const bool selected = preset == preset_;
            if (ImGui::MenuItem(PresetName(preset), nullptr, selected))
                ApplyPreset(preset);
        }
    }

    void DrawDockWindows(UiContext& context) {
        if (!context.requestWorkspace.empty()) {
            Select(context.requestWorkspace);
            context.requestWorkspace.clear();
        }

        for (auto& entry : entries_) {
            if (!entry.open) continue;

            if (pendingFocus_ == entry.workspace->Id())
                ImGui::SetNextWindowFocus();

            ImGui::SetNextWindowSize(ImVec2(680, 520), ImGuiCond_FirstUseEver);
            if (ImGui::Begin(entry.workspace->Title(), &entry.open,
                             ImGuiWindowFlags_NoCollapse)) {
                entry.workspace->Draw(context);
            }
            ImGui::End();

            if (pendingFocus_ == entry.workspace->Id())
                pendingFocus_.clear();
        }
    }

private:
    struct Entry {
        std::unique_ptr<IWorkspace> workspace;
        bool open = false;
    };

    void OpenMany(std::initializer_list<const char*> ids) {
        for (const char* id : ids) SetOpen(id, true);
    }

    std::vector<Entry> entries_;
    WorkspacePreset preset_ = WorkspacePreset::Memory;
    std::string pendingFocus_;
};

} // namespace cortex::ui
