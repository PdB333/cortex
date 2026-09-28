#include "scripts_workspace.h"
#include "widgets.h"

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
        HintText("Select a process to use Lua scripts.");
        return;
    }
    if (targetId_ != session->Target().id) {
        targetId_ = session->Target().id;
        context.scriptsModel->Reset();
        name_.fill(0);
        source_.fill(0);
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Lua scripts");
    FlowSameLine(ButtonWidth("Connect existing"));
    if (context.payload && !context.payload->Ready()) {
        if (ImGui::Button("Connect existing")) {
            std::string error;
            if (!context.payload->TryConnectExisting(&error))
                context.status = "Runtime connect failed: " + error;
            else
                context.scriptsModel->Refresh(nullptr);
        }
        FlowSameLine(ButtonWidth("Enable runtime"));
        ImGui::BeginDisabled(!context.mutationAllowed);
        if (ImGui::Button("Enable runtime")) {
            context.RunInBackground("Loading the Cortex runtime into the target", [=, &context]() {
                std::string error;
                if (!context.payload->EnsureReady(&error))
                    context.status = "Runtime enable failed: " + error;
                else
                    context.scriptsModel->Refresh(nullptr);
            });
        }
        ImGui::EndDisabled();
        FlowSameLine(ButtonWidth("Refresh"));
    }
    if (ImGui::Button("Refresh")) {
        std::string error;
        if (!context.scriptsModel->Refresh(&error))
            context.status = "Scripts refresh failed: " + error;
        else
            context.status = std::to_string(context.scriptsModel->Scripts().size()) +
                             " script(s)";
    }

    ImGui::Separator();

    // The editor needs the width; in a narrow dock the script list moves
    // above it instead of squeezing it into a sliver.
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    const bool stacked = availableWidth < Px(560.0f);
    const ImVec2 listSize = stacked
        ? ImVec2(0.0f, Px(120.0f))
        : ImVec2(std::clamp(availableWidth * 0.28f, Px(160.0f), Px(240.0f)), 0.0f);
    ImGui::BeginChild("ScriptList", listSize, ImGuiChildFlags_Borders);
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

    if (!stacked) ImGui::SameLine();
    ImGui::BeginChild("ScriptEditor", ImVec2(0, 0), ImGuiChildFlags_Borders);

    ImGui::SetNextItemWidth(Px(220));
    ImGui::InputTextWithHint("##ScriptName", "Script name",
                             name_.data(), name_.size());
    FlowSameLine(120 + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize("Timeout ms").x);
    ImGui::SetNextItemWidth(Px(120));
    ImGui::InputInt("Timeout ms", &timeoutMs_);
    timeoutMs_ = std::clamp(timeoutMs_, 100, 120000);
    FlowSameLine(ButtonWidth("Save"));

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
    FlowSameLine(ButtonWidth("Run saved"));
    if (ImGui::Button("Run saved")) {
        context.RunInBackground("Running the saved script", [=, &context]() {
            std::string error;
            if (!context.scriptsModel->RunSaved(
                    name_.data(), timeoutMs_, context.mutationAllowed, &error))
                context.status = "Run script failed: " + error;
            else
                context.status = "Saved script executed";
        });
    }
    FlowSameLine(ButtonWidth("Delete"));
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

    FlowSameLine(ButtonWidth("Run buffer"));
    ImGui::BeginDisabled(!context.mutationAllowed || source_[0] == '\0');
    if (ImGui::Button("Run buffer")) {
        context.RunInBackground("Running the script", [=, &context]() {
            std::string error;
            if (!context.scriptsModel->RunBuffer(
                    source_.data(), timeoutMs_, context.mutationAllowed, &error))
                context.status = "Run buffer failed: " + error;
            else
                context.status = "Script buffer executed";
        });
    }
    ImGui::EndDisabled();

    const float outputHeight =
        std::clamp(ImGui::GetContentRegionAvail().y * 0.3f, Px(60.0f), Px(150.0f));
    MonoInputTextMultiline("##LuaSource", source_.data(), source_.size(),
                              ImVec2(-1, -outputHeight - Px(28)));
    ImGui::TextDisabled("Output");
    ImGui::BeginChild("ScriptOutput", ImVec2(0, outputHeight), ImGuiChildFlags_Borders);
    ImGui::TextWrapped("%s", context.scriptsModel->Output().empty()
                              ? "No script output."
                              : context.scriptsModel->Output().c_str());
    ImGui::EndChild();

    ImGui::EndChild();
}

} // namespace cortex::ui
