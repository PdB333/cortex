#pragma once

#include "workspace.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <initializer_list>
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

    bool Has(const std::string& id) const {
        for (const auto& entry : entries_)
            if (id == entry.workspace->Id()) return true;
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

    void ApplyPreset(WorkspacePreset preset, bool rebuildLayout = true) {
        for (auto& entry : entries_) entry.open = false;

        switch (preset) {
            case WorkspacePreset::Memory:
                OpenMany({"memory", "addresses", "memory-browser", "modules", "watches"});
                break;
            case WorkspacePreset::Debug:
                OpenMany({"disassembly", "debugger", "patches", "memory-browser", "modules", "watches"});
                break;
            case WorkspacePreset::ReverseEngineering:
                OpenMany({"re", "disassembly", "patches", "memory-browser", "modules", "project", "symbols",
                          "structures", "pointermaps", "snapshots", "instrumentation", "runtime"});
                break;
            case WorkspacePreset::Trace:
                OpenMany({"disassembly", "debugger", "trace", "memory-browser", "watches"});
                break;
            case WorkspacePreset::Automation:
                OpenMany({"scripts", "input", "actions", "events", "watches", "runtime"});
                break;
            case WorkspacePreset::Runtime:
                OpenMany({"runtime", "diagnostics", "network", "screenshots", "instrumentation",
                          "actions", "watches", "modules", "sessions", "settings"});
                break;
        }

        SetOpen("bottom", true);
        preset_ = preset;
        pendingFocus_ = PrimaryWorkspace(preset);
        if (rebuildLayout) layoutDirty_ = true;
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
        ImGui::Separator();
        if (ImGui::MenuItem("Rebuild current layout"))
            layoutDirty_ = true;
    }

    void PrepareDockLayout(ImGuiID dockspaceId, const ImVec2& dockspaceSize) {
        if (!initialLayoutChecked_) {
            initialLayoutChecked_ = true;
            const ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockspaceId);
            if (!node || (!node->ChildNodes[0] && !node->ChildNodes[1]))
                layoutDirty_ = true;
        }

        if (!layoutDirty_) return;

        ImGui::DockBuilderRemoveNode(dockspaceId);
        ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspaceId, dockspaceSize);

        ImGuiID center = dockspaceId;
        const ImGuiID bottom =
            ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.27f, nullptr, &center);
        const ImGuiID left =
            ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.23f, nullptr, &center);
        const ImGuiID right =
            ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.28f, nullptr, &center);

        std::vector<std::string> docked;
        DockMany(bottom, {"bottom"}, docked);

        switch (preset_) {
            case WorkspacePreset::Memory:
                DockMany(left, {"addresses", "modules"}, docked);
                DockMany(center, {"memory", "memory-browser"}, docked);
                DockMany(right, {"watches"}, docked);
                break;

            case WorkspacePreset::Debug:
                DockMany(left, {"modules", "watches"}, docked);
                DockMany(center, {"disassembly", "memory-browser"}, docked);
                DockMany(right, {"debugger", "patches"}, docked);
                break;

            case WorkspacePreset::ReverseEngineering:
                DockMany(left, {"project", "symbols", "structures", "pointermaps", "snapshots"}, docked);
                DockMany(center, {"re", "disassembly", "memory-browser"}, docked);
                DockMany(right, {"patches", "instrumentation", "runtime"}, docked);
                break;

            case WorkspacePreset::Trace:
                DockMany(left, {"watches"}, docked);
                DockMany(center, {"trace", "disassembly"}, docked);
                DockMany(right, {"debugger", "memory-browser"}, docked);
                break;

            case WorkspacePreset::Automation:
                DockMany(left, {"scripts", "input"}, docked);
                DockMany(center, {"actions", "events"}, docked);
                DockMany(right, {"watches", "runtime"}, docked);
                break;

            case WorkspacePreset::Runtime:
                DockMany(left, {"modules", "sessions", "settings"}, docked);
                DockMany(center, {"runtime", "diagnostics", "network"}, docked);
                DockMany(right, {"screenshots", "instrumentation", "actions", "watches"}, docked);
                break;
        }

        for (const auto& entry : entries_) {
            if (!entry.open) continue;
            const std::string id = entry.workspace->Id();
            if (std::find(docked.begin(), docked.end(), id) != docked.end()) continue;
            ImGui::DockBuilderDockWindow(entry.workspace->Title(), center);
        }

        ImGui::DockBuilderFinish(dockspaceId);
        layoutDirty_ = false;
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

            const bool isBottomPanel = std::string(entry.workspace->Id()) == "bottom";
            ImGui::SetNextWindowSize(
                isBottomPanel ? ImVec2(980, 280) : ImVec2(680, 520),
                ImGuiCond_FirstUseEver);
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

    static const char* PrimaryWorkspace(WorkspacePreset preset) {
        switch (preset) {
            case WorkspacePreset::Memory: return "memory";
            case WorkspacePreset::Debug: return "disassembly";
            case WorkspacePreset::ReverseEngineering: return "re";
            case WorkspacePreset::Trace: return "trace";
            case WorkspacePreset::Automation: return "scripts";
            case WorkspacePreset::Runtime: return "runtime";
        }
        return "memory";
    }

    void DockMany(ImGuiID node,
                  std::initializer_list<const char*> ids,
                  std::vector<std::string>& docked) {
        for (const char* id : ids) {
            for (const auto& entry : entries_) {
                if (id != std::string(entry.workspace->Id()) || !entry.open) continue;
                ImGui::DockBuilderDockWindow(entry.workspace->Title(), node);
                docked.emplace_back(id);
                break;
            }
        }
    }

    void OpenMany(std::initializer_list<const char*> ids) {
        for (const char* id : ids) SetOpen(id, true);
    }

    std::vector<Entry> entries_;
    WorkspacePreset preset_ = WorkspacePreset::Memory;
    std::string pendingFocus_;
    bool initialLayoutChecked_ = false;
    bool layoutDirty_ = false;
};

} // namespace cortex::ui
