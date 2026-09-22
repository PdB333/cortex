#include "scripts_workspace.h"

#include <imgui.h>

#include <algorithm>
#include <cstring>

namespace cortex::ui {
namespace {
void Copy(char* destination, size_t capacity, const std::string& source) {
    if (!destination || capacity == 0) return;
    std::memset(destination, 0, capacity);
    std::memcpy(destination, source.data(), std::min(capacity - 1, source.size()));
}
} // namespace

void ScriptsWorkspace::SyncEditor(UiContext& context) {
    if (!context.scriptsModel) return;
    Copy(name_.data(), name_.size(), context.scriptsModel->SelectedName());
    Copy(source_.data(), source_.size(), context.scriptsModel->SelectedSource());
}

void ScriptsWorkspace::Draw(UiContext& context) {
    const auto session = context.sessions ? context.sessions->Active() : nullptr;
    if (!session || !context.scriptsModel) {
        ImGui::TextDisabled("Select a process to use Lua scripts.");
        return;
    }
    if (targetId_ != session->Target().id) {
        targetId_ = session->Target().id;
        context.scriptsModel->Reset();
        name_.fill(0);
        source_.fill(0);
    }

    ImGui::TextUnformatted("Lua scripts");
    ImGui::SameLine();
    if (context.payload && !context.payload->Ready()) {
        if (ImGui::SmallButton("Connect existing")) {
            std::string error;
            if (!context.payload->TryConnectExisting(&error))
                context.status = "Runtime connect failed: " + error;
            else
                context.scriptsModel->Refresh(nullptr);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!context.mutationAllowed);
        if (ImGui::SmallButton("Enable runtime")) {
            std::string error;
            if (!context.payload->EnsureReady(&error))
                context.status = "Runtime enable failed: " + error;
            else
                context.scriptsModel->Refresh(nullptr);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
    }
    if (ImGui::SmallButton("Refresh")) {
        std::string error;
        if (!context.scriptsModel->Refresh(&error))
            context.status = "Scripts refresh failed: " + error;
        else
            context.status = std::to_string(context.scriptsModel->Scripts().size()) +
                             " script(s)";
    }

    ImGui::Separator();

    const float leftWidth = 220.0f;
    ImGui::BeginChild("ScriptList", ImVec2(leftWidth, 0), ImGuiChildFlags_Borders);
    if (ImGui::Selectable("+ New script", context.scriptsModel->SelectedName().empty())) {
        context.scriptsModel->ClearSelection();
        name_.fill(0);
        source_.fill(0);
    }
    for (const auto& script : context.scriptsModel->Scripts()) {
        const bool selected = script == context.scriptsModel->SelectedName();
        if (ImGui::Selectable(script.c_str(), selected)) {
            std::string error;
            if (!context.scriptsModel->Load(script, &error))
                context.status = "Load script failed: " + error;
            else
                SyncEditor(context);
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("ScriptEditor", ImVec2(0, 0), ImGuiChildFlags_Borders);

    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##ScriptName", "Script name",
                             name_.data(), name_.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt("Timeout ms", &timeoutMs_);
    timeoutMs_ = std::clamp(timeoutMs_, 100, 120000);
    ImGui::SameLine();

    ImGui::BeginDisabled(!context.mutationAllowed || name_[0] == '\0');
    if (ImGui::Button("Save")) {
        std::string error;
        if (!context.scriptsModel->Save(
                name_.data(), source_.data(), context.mutationAllowed, &error))
            context.status = "Save script failed: " + error;
        else {
            SyncEditor(context);
            context.status = "Script saved";
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Run saved")) {
        std::string error;
        if (!context.scriptsModel->RunSaved(
                name_.data(), timeoutMs_, context.mutationAllowed, &error))
            context.status = "Run script failed: " + error;
        else
            context.status = "Saved script executed";
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete")) {
        std::string error;
        if (!context.scriptsModel->Delete(
                name_.data(), context.mutationAllowed, &error))
            context.status = "Delete script failed: " + error;
        else {
            name_.fill(0);
            source_.fill(0);
            context.status = "Script deleted";
        }
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!context.mutationAllowed || source_[0] == '\0');
    if (ImGui::Button("Run buffer")) {
        std::string error;
        if (!context.scriptsModel->RunBuffer(
                source_.data(), timeoutMs_, context.mutationAllowed, &error))
            context.status = "Run buffer failed: " + error;
        else
            context.status = "Script buffer executed";
    }
    ImGui::EndDisabled();

    const float outputHeight = 150.0f;
    ImGui::InputTextMultiline("##LuaSource", source_.data(), source_.size(),
                              ImVec2(-1, -outputHeight - 28));
    ImGui::TextDisabled("Output");
    ImGui::BeginChild("ScriptOutput", ImVec2(0, outputHeight), ImGuiChildFlags_Borders);
    ImGui::TextWrapped("%s", context.scriptsModel->Output().empty()
                              ? "No script output."
                              : context.scriptsModel->Output().c_str());
    ImGui::EndChild();

    ImGui::EndChild();
}

} // namespace cortex::ui
