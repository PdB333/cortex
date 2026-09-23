#include "patches_workspace.h"

#include <imgui.h>

#include <algorithm>

namespace cortex::ui {

void PatchesWorkspace::Draw(UiContext& context) {
    const auto session = context.sessions ? context.sessions->Active() : nullptr;
    if (!session || !context.patchesModel) {
        ImGui::TextDisabled("Select a process to inspect or apply patches.");
        return;
    }

    if (targetId_ != session->Target().id) {
        targetId_ = session->Target().id;
        context.patchesModel->Reset();
        mode_ = 0;
        address_.fill(0);
        value_.fill(0);
        label_.fill(0);
        extra_ = 5;
    }

    ImGui::TextUnformatted("Runtime patches");
    ImGui::SameLine();
    ImGui::TextDisabled(context.payload && context.payload->Ready()
                            ? "runtime connected" : "runtime disconnected");
    ImGui::SameLine();

    if (context.payload && !context.payload->Ready()) {
        if (ImGui::SmallButton("Connect existing")) {
            std::string error;
            if (!context.payload->TryConnectExisting(&error))
                context.status = "Runtime connect failed: " + error;
            else
                context.patchesModel->Refresh(nullptr);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!context.mutationAllowed);
        if (ImGui::SmallButton("Enable runtime")) {
            std::string error;
            if (!context.payload->EnsureReady(&error))
                context.status = "Runtime enable failed: " + error;
            else
                context.patchesModel->Refresh(nullptr);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
    }

    if (ImGui::SmallButton("Refresh")) {
        std::string error;
        if (!context.patchesModel->Refresh(&error))
            context.status = "Patch refresh failed: " + error;
        else
            context.status = std::to_string(context.patchesModel->Patches().size()) +
                             " active patch(es)";
    }

    ImGui::Separator();

    static const char* modes[] = {
        "Raw bytes", "NOP", "Assemble", "Detour", "Trampoline", "Code cave"
    };
    ImGui::SetNextItemWidth(125);
    ImGui::Combo("##PatchMode", &mode_, modes, IM_ARRAYSIZE(modes));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180);
    ImGui::InputTextWithHint("##PatchAddress",
                             mode_ == 5 ? "Near address" : "Address (0x...)",
                             address_.data(), address_.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-360);
    const char* hint =
        mode_ == 0 ? "Bytes hex, e.g. 909090" :
        mode_ == 1 ? "Size" :
        mode_ == 2 ? "Assembly; separate instructions with ;" :
        mode_ == 5 ? "Allocation size" : "Target address";
    ImGui::InputTextWithHint("##PatchValue", hint, value_.data(), value_.size());

    ImGui::SameLine();
    if (mode_ <= 2) {
        ImGui::SetNextItemWidth(140);
        ImGui::InputTextWithHint("##PatchLabel", "Label", label_.data(), label_.size());
        ImGui::SameLine();
    }

    if (mode_ == 3 || mode_ == 4) {
        ImGui::SetNextItemWidth(100);
        ImGui::InputInt(mode_ == 3 ? "JMP size" : "Min overwrite", &extra_);
        extra_ = std::max(5, extra_);
        ImGui::SameLine();
    }

    ImGui::BeginDisabled(!context.mutationAllowed ||
                         address_[0] == '\0' || value_[0] == '\0');
    if (ImGui::Button(mode_ == 2 ? "Assemble / write" :
                      mode_ == 5 ? "Allocate" : "Apply")) {
        std::string error;
        bool ok = false;
        switch (mode_) {
            case 0:
                ok = context.patchesModel->ApplyBytes(
                    address_.data(), value_.data(), label_.data(),
                    context.mutationAllowed, &error);
                break;
            case 1: {
                int size = 0;
                try { size = std::stoi(value_.data()); } catch (...) {}
                ok = context.patchesModel->ApplyNop(
                    address_.data(), size, label_.data(),
                    context.mutationAllowed, &error);
                break;
            }
            case 2:
                ok = context.patchesModel->ApplyAssembly(
                    address_.data(), value_.data(), label_.data(),
                    context.mutationAllowed, &error);
                break;
            case 3:
                ok = context.patchesModel->ApplyDetour(
                    address_.data(), value_.data(), extra_,
                    context.mutationAllowed, &error);
                break;
            case 4:
                ok = context.patchesModel->ApplyTrampoline(
                    address_.data(), value_.data(), extra_,
                    context.mutationAllowed, &error);
                break;
            case 5: {
                int size = 0;
                try { size = std::stoi(value_.data()); } catch (...) {}
                ok = context.patchesModel->AllocateCave(
                    address_.data(), size, context.mutationAllowed, &error);
                break;
            }
        }
        context.status = ok ? "Patch operation completed"
                            : "Patch operation failed: " + error;
    }
    ImGui::EndDisabled();

    ImGui::Spacing();

    int revertId = -1;
    const float resultHeight = 135.0f;
    if (ImGui::BeginTable("PatchTable", 6,
                          ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_BordersInnerH |
                          ImGuiTableFlags_Resizable |
                          ImGuiTableFlags_ScrollY,
                          ImVec2(0, -resultHeight))) {
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 50);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch, 0.18f);
        ImGui::TableSetupColumn("Original", ImGuiTableColumnFlags_WidthStretch, 0.22f);
        ImGui::TableSetupColumn("Current", ImGuiTableColumnFlags_WidthStretch, 0.22f);
        ImGui::TableSetupColumn("Label / Gateway", ImGuiTableColumnFlags_WidthStretch, 0.28f);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 75);
        ImGui::TableHeadersRow();

        for (const auto& patch : context.patchesModel->Patches()) {
            ImGui::PushID(patch.id);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", patch.id);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(patch.address.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(patch.originalBytes.c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(patch.currentBytes.c_str());
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%s%s%s",
                        patch.label.empty() ? "(unlabeled)" : patch.label.c_str(),
                        patch.gateway.empty() ? "" : " | ",
                        patch.gateway.c_str());
            ImGui::TableSetColumnIndex(5);
            ImGui::BeginDisabled(!context.mutationAllowed);
            if (ImGui::SmallButton("Revert")) revertId = patch.id;
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (revertId >= 0) {
        std::string error;
        if (!context.patchesModel->Revert(
                revertId, context.mutationAllowed, &error))
            context.status = "Patch revert failed: " + error;
        else
            context.status = "Patch reverted";
    }

    ImGui::TextDisabled("Last patch operation");
    ImGui::BeginChild("PatchOperationResult", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::TextWrapped("%s", context.patchesModel->OperationResult().empty()
                              ? "No patch operation yet."
                              : context.patchesModel->OperationResult().c_str());
    ImGui::EndChild();
}

} // namespace cortex::ui
