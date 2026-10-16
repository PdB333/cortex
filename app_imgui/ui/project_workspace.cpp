#include "project_workspace.h"
#include "widgets.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>

namespace cortex::ui {
namespace {

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool ParseNumber(const std::string& text, uint64_t& value, int defaultBase = 0) {
    if (text.empty()) return false;
    try {
        size_t used = 0;
        int base = defaultBase;
        if (base == 0) {
            const bool hexAlpha = std::any_of(text.begin(), text.end(), [](unsigned char ch) {
                ch = static_cast<unsigned char>(std::tolower(ch));
                return ch >= 'a' && ch <= 'f';
            });
            if (hexAlpha) base = 16;
        }
        value = std::stoull(text, &used, base);
        return used == text.size();
    } catch (...) {
        return false;
    }
}

void Copy(char* destination, size_t capacity, const std::string& source) {
    if (!destination || capacity == 0) return;
    std::memset(destination, 0, capacity);
    std::memcpy(destination, source.data(), std::min(capacity - 1, source.size()));
}

} // namespace

void ProjectWorkspace::ClearAddressForm() {
    addressName_.fill(0);
    addressExpression_.fill(0);
    addressType_.fill(0);
    addressNotes_.fill(0);
}

void ProjectWorkspace::ClearPointerForm() {
    pointerName_.fill(0);
    pointerModule_.fill(0);
    pointerBase_.fill(0);
    pointerOffsets_.fill(0);
    Copy(pointerOffsets_.data(), pointerOffsets_.size(), "[]");
    pointerType_.fill(0);
    pointerNotes_.fill(0);
}

void ProjectWorkspace::ClearNoteForm() {
    noteText_.fill(0);
    noteTags_.fill(0);
    Copy(noteTags_.data(), noteTags_.size(), "[]");
}

bool ProjectWorkspace::Refresh(UiContext& context) {
    if (!context.projectModel) return false;
    std::string error;
    if (!context.projectModel->Refresh(&error)) {
        context.status = "Project refresh failed: " + error;
        return false;
    }
    context.status = "Project knowledge refreshed";
    return true;
}

bool ProjectWorkspace::Navigate(UiContext& context, const std::string& expression,
                                const char* workspace) {
    uint64_t address = 0;
    if (ParseNumber(expression, address)) {
        context.NavigateTo(workspace, address);
        return true;
    }

    const size_t plus = expression.find_last_of('+');
    if (plus == std::string::npos || !context.modules) {
        context.status = "Project address is not directly resolvable: " + expression;
        return false;
    }

    const std::string moduleName = Lower(expression.substr(0, plus));
    uint64_t offset = 0;
    if (!ParseNumber(expression.substr(plus + 1), offset, 16)) {
        context.status = "Invalid project module offset";
        return false;
    }

    std::string error;
    const auto modules = context.modules->List(&error);
    for (const auto& module : modules) {
        if (Lower(module.name) != moduleName) continue;
        if (offset > std::numeric_limits<uint64_t>::max() - module.base) {
            context.status = "Project address overflow";
            return false;
        }
        context.NavigateTo(workspace, module.base + offset);
        return true;
    }
    context.status = error.empty() ? "Project module not found" : error;
    return false;
}

void ProjectWorkspace::Draw(UiContext& context) {
    const auto session = context.sessions ? context.sessions->Active() : nullptr;
    if (!session || !context.projectModel) {
        HintText("Select a process to use persistent project knowledge.");
        return;
    }

    if (targetId_ != session->Target().id) {
        targetId_ = session->Target().id;
        context.projectModel->Reset();
        ClearAddressForm();
        ClearPointerForm();
        ClearNoteForm();
        knowledgeFocus_.fill(0);
        knowledgeDepth_ = 1;
        selectedKnowledgeId_.clear();
        if (context.payload && context.payload->Ready()) Refresh(context);
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Persistent target knowledge");
    Subtitle("addresses, pointer paths and notes");
    FlowSameLine(ButtonWidth("Refresh"));
    if (ImGui::Button("Refresh")) Refresh(context);

    FlowSameLine(ButtonWidth("Connect existing runtime"));
    if (context.payload && !context.payload->Ready()) {
        if (ImGui::Button("Connect existing runtime")) {
            std::string error;
            if (!context.payload->TryConnectExisting(&error))
                context.status = "Runtime connect failed: " + error;
            else
                Refresh(context);
        }
        FlowSameLine(ButtonWidth("Enable runtime"));
        ImGui::BeginDisabled(!context.mutationAllowed);
        if (ImGui::Button("Enable runtime")) {
            context.RunInBackground("Loading the Cortex runtime into the target", [=, &context]() {
                std::string error;
                if (!context.payload->EnsureReady(&error))
                    context.status = "Runtime enable failed: " + error;
                else
                    Refresh(context);
            });
        }
        ImGui::EndDisabled();
    }

    ImGui::Separator();

    if (ImGui::BeginTabBar("ProjectTabs")) {
        if (ImGui::BeginTabItem("Addresses")) {
            ImGui::SetNextItemWidth(Px(160));
            ImGui::InputTextWithHint("##ProjectAddressName", "Name",
                                     addressName_.data(), addressName_.size());
            FlowSameLine(Px(190));
            ImGui::SetNextItemWidth(Px(190));
            ImGui::InputTextWithHint("##ProjectAddressExpression", "Address / module+offset",
                                     addressExpression_.data(), addressExpression_.size());
            FlowSameLine(Px(110));
            ImGui::SetNextItemWidth(Px(110));
            ImGui::InputTextWithHint("##ProjectAddressType", "Type",
                                     addressType_.data(), addressType_.size());
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-150);
            ImGui::InputTextWithHint("##ProjectAddressNotes", "Notes",
                                     addressNotes_.data(), addressNotes_.size());
            FlowSameLine(ButtonWidth("Save address"));
            ImGui::BeginDisabled(!context.mutationAllowed);
            if (ImGui::Button("Save address")) {
                std::string error;
                if (context.projectModel->SetAddress(
                        addressName_.data(), addressExpression_.data(),
                        addressType_.data(), addressNotes_.data(),
                        context.mutationAllowed, &error)) {
                    ClearAddressForm();
                    context.status = "Project address saved";
                } else {
                    context.status = "Project address save failed: " + error;
                }
            }
            ImGui::EndDisabled();

            ImGui::Spacing();
            std::string deleteAddress;
            if (BeginDataTable("ProjectAddressesTable", 5,
                                  ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_BordersInnerH |
                                  ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_ScrollY,
                                  ImGui::GetContentRegionAvail())) {
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.18f);
                ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch, 0.22f);
                ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, Px(100));
                ImGui::TableSetupColumn("Notes", ImGuiTableColumnFlags_WidthStretch, 0.35f);
                ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, Px(180));
                ImGui::TableHeadersRow();

                for (const auto& row : context.projectModel->Addresses()) {
                    ImGui::PushID(row.name.c_str());
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    if (ImGui::Selectable(row.name.c_str(), false,
                                          ImGuiSelectableFlags_AllowDoubleClick)) {
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                            Navigate(context, row.address, "memory-browser");
                    }
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(row.address.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(row.type.empty() ? "-" : row.type.c_str());
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(row.notes.empty() ? "-" : row.notes.c_str());
                    ImGui::TableSetColumnIndex(4);
                    if (ImGui::SmallButton("Memory"))
                        Navigate(context, row.address, "memory-browser");
                    FlowSameLine(ButtonWidth("Disasm"));
                    if (ImGui::SmallButton("Disasm"))
                        Navigate(context, row.address, "disassembly");
                    FlowSameLine(ButtonWidth("Delete"));
                    ImGui::BeginDisabled(!context.mutationAllowed);
                    if (ImGui::SmallButton("Delete")) deleteAddress = row.name;
                    ImGui::EndDisabled();
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            if (!deleteAddress.empty()) {
                std::string error;
                if (!context.projectModel->DeleteAddress(
                        deleteAddress, context.mutationAllowed, &error))
                    context.status = "Delete address failed: " + error;
                else
                    context.status = "Project address deleted";
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Pointer paths")) {
            ImGui::SetNextItemWidth(Px(140));
            ImGui::InputTextWithHint("##PointerName", "Name",
                                     pointerName_.data(), pointerName_.size());
            FlowSameLine(Px(130));
            ImGui::SetNextItemWidth(Px(130));
            ImGui::InputTextWithHint("##PointerModule", "Module",
                                     pointerModule_.data(), pointerModule_.size());
            FlowSameLine(Px(120));
            ImGui::SetNextItemWidth(Px(120));
            ImGui::InputTextWithHint("##PointerBase", "Base offset",
                                     pointerBase_.data(), pointerBase_.size());
            FlowSameLine(Px(190));
            ImGui::SetNextItemWidth(Px(190));
            ImGui::InputTextWithHint("##PointerOffsets", "Offsets JSON",
                                     pointerOffsets_.data(), pointerOffsets_.size());
            FlowSameLine(Px(100));
            ImGui::SetNextItemWidth(Px(100));
            ImGui::InputTextWithHint("##PointerType", "Final type",
                                     pointerType_.data(), pointerType_.size());

            ImGui::SetNextItemWidth(-140);
            ImGui::InputTextWithHint("##PointerNotes", "Notes",
                                     pointerNotes_.data(), pointerNotes_.size());
            FlowSameLine(ButtonWidth("Save path"));
            ImGui::BeginDisabled(!context.mutationAllowed);
            if (ImGui::Button("Save path")) {
                std::string error;
                if (context.projectModel->SetPointerPath(
                        pointerName_.data(), pointerModule_.data(), pointerBase_.data(),
                        pointerOffsets_.data(), pointerType_.data(), pointerNotes_.data(),
                        context.mutationAllowed, &error)) {
                    ClearPointerForm();
                    context.status = "Pointer path saved";
                } else {
                    context.status = "Pointer path save failed: " + error;
                }
            }
            ImGui::EndDisabled();

            std::string deletePath;
            if (BeginDataTable("ProjectPointersTable", 6,
                                  ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_BordersInnerH |
                                  ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_ScrollY,
                                  ImGui::GetContentRegionAvail())) {
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.16f);
                ImGui::TableSetupColumn("Module", ImGuiTableColumnFlags_WidthStretch, 0.16f);
                ImGui::TableSetupColumn("Base", ImGuiTableColumnFlags_WidthStretch, 0.13f);
                ImGui::TableSetupColumn("Offsets", ImGuiTableColumnFlags_WidthStretch, 0.23f);
                ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, Px(90));
                ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, Px(160));
                ImGui::TableHeadersRow();

                for (const auto& row : context.projectModel->PointerPaths()) {
                    ImGui::PushID(row.name.c_str());
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(row.name.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(row.module.empty() ? "-" : row.module.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(row.baseOffset.c_str());
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(row.offsetsJson.c_str());
                    ImGui::TableSetColumnIndex(4);
                    ImGui::TextUnformatted(row.finalType.empty() ? "-" : row.finalType.c_str());
                    ImGui::TableSetColumnIndex(5);
                    if (ImGui::SmallButton("Resolve")) {
                        std::string address;
                        std::string error;
                        if (context.projectModel->ResolvePointerPath(row.name, address, &error)) {
                            context.status = row.name + " = " + address;
                            Navigate(context, address, "memory-browser");
                        } else {
                            context.status = "Pointer resolve failed: " + error;
                        }
                    }
                    FlowSameLine(ButtonWidth("Delete"));
                    ImGui::BeginDisabled(!context.mutationAllowed);
                    if (ImGui::SmallButton("Delete")) deletePath = row.name;
                    ImGui::EndDisabled();
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            if (!deletePath.empty()) {
                std::string error;
                if (!context.projectModel->DeletePointerPath(
                        deletePath, context.mutationAllowed, &error))
                    context.status = "Delete pointer path failed: " + error;
                else
                    context.status = "Pointer path deleted";
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Knowledge graph")) {
            HintText("Persisted claims and typed links. NOT a proven causal graph.");
            ImGui::SetNextItemWidth(Px(235));
            ImGui::InputTextWithHint("##KnowledgeFocus","Focus knowledge ID (optional)",
                                     knowledgeFocus_.data(),knowledgeFocus_.size());
            FlowSameLine(Px(110));
            ImGui::SetNextItemWidth(Px(110));
            ImGui::SliderInt("Depth", &knowledgeDepth_, 0, 2);
            FlowSameLine(ButtonWidth("Load graph"));
            if(ImGui::Button("Load graph")) {
                std::string error;
                if(!context.projectModel->RefreshKnowledgeGraph(
                        knowledgeFocus_.data(),knowledgeDepth_,32,&error))
                    context.status = "Knowledge graph unavailable: " + error;
                else
                    context.status = "Read-only knowledge graph refreshed";
            }
            ImGui::SameLine();
            if(ImGui::Button("All records")) {
                knowledgeFocus_.fill(0);
                knowledgeDepth_ = 1;
                selectedKnowledgeId_.clear();
                std::string error;
                if(!context.projectModel->RefreshKnowledgeGraph("",1,32,&error))
                    context.status = "Knowledge graph unavailable: " + error;
                else
                    context.status = "Knowledge graph overview refreshed";
            }

            if (!context.projectModel->KnowledgeGraphLoaded()) {
                HintText("Load a bounded neighborhood from the active project runtime.");
            } else {
                const auto& nodes=context.projectModel->KnowledgeNodes();
                const auto& edges=context.projectModel->KnowledgeRelations();
                ImGui::Text("%zu claims, %zu directed relations",nodes.size(),edges.size());
                ImGui::TextUnformatted("Node status is a saved claim, not an independently verified explanation.");
                ImGui::Spacing();
                if(BeginDataTable("ProjectKnowledgeNodes",5,
                     ImGuiTableFlags_RowBg |
                     ImGuiTableFlags_BordersInnerH |
                     ImGuiTableFlags_Resizable |
                     ImGuiTableFlags_ScrollY,
                     ImVec2(0,Px(218)))) {
                    ImGui::TableSetupColumn("ID",ImGuiTableColumnFlags_WidthStretch,0.24f);
                    ImGui::TableSetupColumn("Kind",ImGuiTableColumnFlags_WidthFixed,Px(95));
                    ImGui::TableSetupColumn("Status",ImGuiTableColumnFlags_WidthFixed,Px(100));
                    ImGui::TableSetupColumn("Evidence",ImGuiTableColumnFlags_WidthFixed,Px(80));
                    ImGui::TableSetupColumn("Claim (untrusted)",ImGuiTableColumnFlags_WidthStretch,0.43f);
                    ImGui::TableHeadersRow();
                    for(const auto& row:nodes) {
                        ImGui::PushID(row.id.c_str());
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        if(ImGui::Selectable(row.id.c_str(),
                               selectedKnowledgeId_==row.id,
                               ImGuiSelectableFlags_AllowDoubleClick)) {
                            selectedKnowledgeId_=row.id;
                            if(ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                                Copy(knowledgeFocus_.data(),knowledgeFocus_.size(),row.id);
                        }
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextUnformatted(row.kind.c_str());
                        ImGui::TableSetColumnIndex(2);
                        ImGui::TextUnformatted(row.status.c_str());
                        ImGui::TableSetColumnIndex(3);
                        ImGui::Text("%d",row.evidenceCount);
                        ImGui::TableSetColumnIndex(4);
                        ImGui::TextUnformatted(row.statement.c_str());
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                ImGui::Spacing();
                ImGui::TextUnformatted("Typed relations (client assertions)");
                if(BeginDataTable("ProjectKnowledgeRelations",4,
                     ImGuiTableFlags_RowBg |
                     ImGuiTableFlags_BordersInnerH |
                     ImGuiTableFlags_Resizable |
                     ImGuiTableFlags_ScrollY,
                     ImGui::GetContentRegionAvail())) {
                    ImGui::TableSetupColumn("From",ImGuiTableColumnFlags_WidthStretch,0.29f);
                    ImGui::TableSetupColumn("Relation",ImGuiTableColumnFlags_WidthStretch,0.2f);
                    ImGui::TableSetupColumn("To",ImGuiTableColumnFlags_WidthStretch,0.29f);
                    ImGui::TableSetupColumn("Evidence ref (asserted)",
                                           ImGuiTableColumnFlags_WidthStretch,0.22f);
                    ImGui::TableHeadersRow();
                    for(const auto& edge:edges) {
                        ImGui::PushID(&edge);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted(edge.from.c_str());
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextUnformatted(edge.type.c_str());
                        ImGui::TableSetColumnIndex(2);
                        ImGui::TextUnformatted(edge.to.c_str());
                        ImGui::TableSetColumnIndex(3);
                        ImGui::TextUnformatted(edge.reference.empty() ? "-" : edge.reference.c_str());
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Notes")) {
            ImGui::SetNextItemWidth(-220);
            ImGui::InputTextWithHint("##ProjectNoteText", "Persistent note",
                                     noteText_.data(), noteText_.size());
            FlowSameLine(Px(130));
            ImGui::SetNextItemWidth(Px(130));
            ImGui::InputTextWithHint("##ProjectNoteTags", "Tags JSON",
                                     noteTags_.data(), noteTags_.size());
            FlowSameLine(ButtonWidth("Add note"));
            ImGui::BeginDisabled(!context.mutationAllowed);
            if (ImGui::Button("Add note")) {
                std::string error;
                if (context.projectModel->AddNote(
                        noteText_.data(), noteTags_.data(),
                        context.mutationAllowed, &error)) {
                    ClearNoteForm();
                    context.status = "Project note added";
                } else {
                    context.status = "Add note failed: " + error;
                }
            }
            ImGui::EndDisabled();

            int deleteNoteId = -1;
            if (BeginDataTable("ProjectNotesTable", 4,
                                  ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_BordersInnerH |
                                  ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_ScrollY,
                                  ImGui::GetContentRegionAvail())) {
                ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, Px(55));
                ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthStretch, 0.65f);
                ImGui::TableSetupColumn("Tags", ImGuiTableColumnFlags_WidthStretch, 0.35f);
                ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, Px(75));
                ImGui::TableHeadersRow();
                for (const auto& row : context.projectModel->Notes()) {
                    ImGui::PushID(row.id);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%d", row.id);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(row.text.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(row.tagsJson.c_str());
                    ImGui::TableSetColumnIndex(3);
                    ImGui::BeginDisabled(!context.mutationAllowed);
                    if (ImGui::SmallButton("Delete")) deleteNoteId = row.id;
                    ImGui::EndDisabled();
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            if (deleteNoteId >= 0) {
                std::string error;
                if (!context.projectModel->DeleteNote(
                        deleteNoteId, context.mutationAllowed, &error))
                    context.status = "Delete note failed: " + error;
                else
                    context.status = "Project note deleted";
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

} // namespace cortex::ui
