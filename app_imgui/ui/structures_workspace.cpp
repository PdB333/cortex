#include "structures_workspace.h"
#include "widgets.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace cortex::ui {
namespace {

void Copy(char* destination, size_t capacity, const std::string& source) {
    if (!destination || capacity == 0) return;
    std::memset(destination, 0, capacity);
    std::memcpy(destination, source.data(), std::min(capacity - 1, source.size()));
}

} // namespace

void StructuresWorkspace::SyncSelection(UiContext& context) {
    if (!context.structuresModel) return;
    Copy(name_.data(), name_.size(), context.structuresModel->SelectedName());
    Copy(fieldsJson_.data(), fieldsJson_.size(),
         context.structuresModel->SelectedFieldsJson().empty()
             ? "[\n  {\"name\":\"field0\",\"offset\":0,\"type\":\"i32\"}\n]"
             : context.structuresModel->SelectedFieldsJson());
}

void StructuresWorkspace::Draw(UiContext& context) {
    const auto session = context.sessions ? context.sessions->Active() : nullptr;
    if (!session || !context.structuresModel) {
        HintText("Select a process to inspect structures.");
        return;
    }

    if (targetId_ != session->Target().id) {
        targetId_ = session->Target().id;
        context.structuresModel->Reset();
        name_.fill(0);
        fieldsJson_.fill(0);
        valuesJson_.fill(0);
        instancesJson_.fill(0);
        Copy(fieldsJson_.data(), fieldsJson_.size(),
             "[\n  {\"name\":\"field0\",\"offset\":0,\"type\":\"i32\"}\n]");
        Copy(valuesJson_.data(), valuesJson_.size(), "{\n}");
        Copy(instancesJson_.data(), instancesJson_.size(), "[\n  \"0x0\"\n]");
    }

    uint64_t navigationAddress = 0;
    if (context.ConsumeNavigation("structures", navigationAddress)) {
        char buffer[32] = {};
        std::snprintf(buffer, sizeof(buffer), "0x%llX",
                      static_cast<unsigned long long>(navigationAddress));
        Copy(address_.data(), address_.size(), buffer);
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Runtime structures");
    FlowSameLine(ButtonWidth("Refresh"));
    if (ImGui::Button("Refresh")) {
        std::string error;
        if (!context.structuresModel->Refresh(&error))
            context.status = "Structures refresh failed: " + error;
        else
            context.status = context.structuresModel->Status();
    }
    FlowSameLine(ButtonWidth("Connect existing runtime"));
    if (context.payload && !context.payload->Ready()) {
        if (ImGui::Button("Connect existing runtime")) {
            std::string error;
            if (!context.payload->TryConnectExisting(&error))
                context.status = "Runtime connect failed: " + error;
            else
                context.structuresModel->Refresh(nullptr);
        }
        FlowSameLine(ButtonWidth("Enable runtime"));
        ImGui::BeginDisabled(!context.mutationAllowed);
        if (ImGui::Button("Enable runtime")) {
            context.RunInBackground("Loading the Cortex runtime into the target", [=, &context]() {
                std::string error;
                if (!context.payload->EnsureReady(&error))
                    context.status = "Runtime enable failed: " + error;
                else
                    context.structuresModel->Refresh(nullptr);
            });
        }
        ImGui::EndDisabled();
    }

    ImGui::Separator();

    const float leftWidth = Px(230.0f);
    ImGui::BeginChild("StructureList", ImVec2(leftWidth, 0), ImGuiChildFlags_Borders);
    ImGui::Text("Definitions (%zu)", context.structuresModel->Definitions().size());
    ImGui::Separator();
    if (ImGui::Selectable("+ New structure",
                          context.structuresModel->SelectedName().empty())) {
        context.structuresModel->ClearSelection();
        name_.fill(0);
        fieldsJson_.fill(0);
        Copy(fieldsJson_.data(), fieldsJson_.size(),
             "[\n  {\"name\":\"field0\",\"offset\":0,\"type\":\"i32\"}\n]");
    }
    for (const auto& def : context.structuresModel->Definitions()) {
        const bool selected = def.name == context.structuresModel->SelectedName();
        const std::string label = def.name + " (" + std::to_string(def.fieldCount) + ")";
        if (ImGui::Selectable(label.c_str(), selected)) {
            std::string error;
            if (context.structuresModel->Select(def.name, &error))
                SyncSelection(context);
            else
                context.status = "Structure selection failed: " + error;
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("StructureEditor", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (ImGui::BeginTabBar("StructureTabs")) {
        if (ImGui::BeginTabItem("Definition")) {
            ImGui::SetNextItemWidth(Px(260));
            ImGui::InputTextWithHint("##StructureName", "Structure name",
                                     name_.data(), name_.size());
            ImGui::TextDisabled("Fields JSON");
            MonoInputTextMultiline("##StructureFields", fieldsJson_.data(),
                                      fieldsJson_.size(), ImVec2(-1, 210));

            ImGui::BeginDisabled(!context.mutationAllowed);
            if (ImGui::Button("Define / update")) {
                std::string error;
                if (context.structuresModel->Define(
                        name_.data(), fieldsJson_.data(),
                        context.mutationAllowed, &error)) {
                    SyncSelection(context);
                    context.status = context.structuresModel->Status();
                } else {
                    context.status = "Define structure failed: " + error;
                }
            }
            FlowSameLine(ButtonWidth("Delete selected"));
            ImGui::BeginDisabled(context.structuresModel->SelectedName().empty());
            if (ImGui::Button("Delete selected")) {
                std::string error;
                const std::string selected = context.structuresModel->SelectedName();
                if (context.structuresModel->Delete(
                        selected, context.mutationAllowed, &error)) {
                    name_.fill(0);
                    context.status = context.structuresModel->Status();
                } else {
                    context.status = "Delete structure failed: " + error;
                }
            }
            ImGui::EndDisabled();
            ImGui::EndDisabled();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Read / write")) {
            ImGui::SetNextItemWidth(Px(260));
            ImGui::InputTextWithHint("##StructureAddress", "Address / runtime expression",
                                     address_.data(), address_.size());
            ImGui::SameLine();
            ImGui::BeginDisabled(context.structuresModel->SelectedName().empty() ||
                                 address_[0] == '\0');
            if (ImGui::Button("Read")) {
                std::string error;
                if (!context.structuresModel->Read(
                        context.structuresModel->SelectedName(),
                        address_.data(), &error))
                    context.status = "Structure read failed: " + error;
                else
                    context.status = context.structuresModel->Status();
            }
            ImGui::EndDisabled();

            ImGui::Spacing();
            if (BeginDataTable("StructureReadFields", 3,
                                  ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_BordersInnerH |
                                  ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_ScrollY,
                                  ImVec2(0, Px(220)))) {
                ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthStretch, 0.30f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.45f);
                ImGui::TableSetupColumn("Error", ImGuiTableColumnFlags_WidthStretch, 0.25f);
                ImGui::TableHeadersRow();
                for (const auto& row : context.structuresModel->ReadFields()) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(row.name.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(row.value.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(row.error.empty() ? "-" : row.error.c_str());
                }
                ImGui::EndTable();
            }

            ImGui::TextDisabled("Write values JSON");
            MonoInputTextMultiline("##StructureValues", valuesJson_.data(),
                                      valuesJson_.size(), ImVec2(-1, 130));
            ImGui::BeginDisabled(!context.mutationAllowed ||
                                 context.structuresModel->SelectedName().empty() ||
                                 address_[0] == '\0');
            if (ImGui::Button("Write fields")) {
                std::string error;
                if (!context.structuresModel->Write(
                        context.structuresModel->SelectedName(),
                        address_.data(), valuesJson_.data(),
                        context.mutationAllowed, &error))
                    context.status = "Structure write failed: " + error;
                else
                    context.status = context.structuresModel->Status();
            }
            ImGui::EndDisabled();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Infer")) {
            ImGui::TextDisabled("Instance addresses JSON");
            MonoInputTextMultiline("##StructureInstances", instancesJson_.data(),
                                      instancesJson_.size(), ImVec2(-1, 120));
            ImGui::SetNextItemWidth(Px(140));
            ImGui::InputInt("Size", &inferSize_);
            inferSize_ = std::clamp(inferSize_, 4, 1024 * 1024);
            FlowSameLine(CheckboxWidth("Define result"));
            ImGui::Checkbox("Define result", &inferDefine_);
            if (inferDefine_) {
                ImGui::SameLine();
                ImGui::TextDisabled("name: %s", name_.data());
            }

            ImGui::BeginDisabled(inferDefine_ &&
                                 (!context.mutationAllowed || name_[0] == '\0'));
            if (ImGui::Button(inferDefine_ ? "Infer + define" : "Infer")) {
                context.RunInBackground("Inferring the structure", [=, &context]() {
                    std::string error;
                    if (!context.structuresModel->Infer(
                            instancesJson_.data(), inferSize_, inferDefine_,
                            name_.data(), context.mutationAllowed, &error))
                        context.status = "Structure inference failed: " + error;
                    else {
                        context.status = context.structuresModel->Status();
                        if (inferDefine_) SyncSelection(context);
                    }
                });
            }
            ImGui::EndDisabled();

            if (BeginDataTable("StructureInference", 7,
                                  ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_BordersInnerH |
                                  ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_ScrollY,
                                  ImGui::GetContentRegionAvail())) {
                ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthStretch, 0.20f);
                ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, Px(80));
                ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, Px(60));
                ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, Px(90));
                ImGui::TableSetupColumn("Confidence", ImGuiTableColumnFlags_WidthFixed, Px(90));
                ImGui::TableSetupColumn("Distinct", ImGuiTableColumnFlags_WidthFixed, Px(70));
                ImGui::TableSetupColumn("Evidence", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();
                for (const auto& row : context.structuresModel->InferenceFields()) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(row.name.c_str());
                    ImGui::TableSetColumnIndex(1);
                    MonoText("0x%llX", static_cast<unsigned long long>(row.offset));
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%llu", static_cast<unsigned long long>(row.byteSize));
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(row.type.c_str());
                    ImGui::TableSetColumnIndex(4);
                    ImGui::Text("%.2f", row.confidence);
                    ImGui::TableSetColumnIndex(5);
                    ImGui::Text("%llu%s",
                                static_cast<unsigned long long>(row.distinctValues),
                                row.constant ? " const" : "");
                    ImGui::TableSetColumnIndex(6);
                    ImGui::TextUnformatted(row.reasonsJson.c_str());
                    if (ImGui::IsItemHovered()) {
                        ImGui::BeginTooltip();
                        ImGui::TextWrapped("values: %s", row.valuesJson.c_str());
                        ImGui::EndTooltip();
                    }
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
}

} // namespace cortex::ui
