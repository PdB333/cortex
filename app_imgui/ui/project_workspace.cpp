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

    if (targetId_ != TargetKey(session->Target())) {
        targetId_ = TargetKey(session->Target());
        context.projectModel->Reset();
        ClearAddressForm();
        ClearPointerForm();
        ClearNoteForm();
        lastEventSeen_ = context.runtimeEventsModel ? context.runtimeEventsModel->LastEventId() : 0;
        if (context.contextModel) context.contextModel->Reset();
        if (context.payload && context.payload->Ready()) {
            Refresh(context);
            RefreshContext(context);
        }
    }
    FollowRuntimeEvents(context);

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
        if (ImGui::BeginTabItem("Knowledge")) {
            DrawKnowledgeTab(context);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Investigation context")) {
            DrawContextTab(context);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void ProjectWorkspace::FollowRuntimeEvents(UiContext& context) {
    if (!context.runtimeEventsModel || !context.payload || !context.payload->Ready()) return;
    bool knowledgeChanged = false, contextChanged = false;
    uint64_t newest = lastEventSeen_;
    for (const auto& event : context.runtimeEventsModel->Events()) {
        if (event.id <= lastEventSeen_) continue;
        newest = std::max(newest, event.id);
        if (event.type == "knowledge.updated" || event.type == "evidence.created") knowledgeChanged = true;
        if (event.type.rfind("context.", 0) == 0 || event.type.rfind("agent.action_", 0) == 0) contextChanged = true;
    }
    lastEventSeen_ = newest;
    if (knowledgeChanged) Refresh(context);
    if (contextChanged) RefreshContext(context);
}

void ProjectWorkspace::RefreshContext(UiContext& context) {
    if (!context.contextModel || !context.sessions) return;
    const auto target = context.sessions->ActiveTarget();
    std::string error;
    if (!context.contextModel->Refresh(target ? target->generation : 0, &error))
        context.status = "Investigation context refresh failed: " + error;
}

void ProjectWorkspace::DrawKnowledgeTab(UiContext& context) {
    HintText("Revisioned claims saved by you or an agent (project_knowledge_put). A status is what its author "
             "asserted; Cortex never upgrades a claim on its own.");
    const auto& rows = context.projectModel->Knowledge();
    if (rows.empty()) {
        ImGui::TextDisabled("No knowledge records in this project yet.");
        return;
    }
    if (!BeginDataTable("ProjectKnowledgeTable", 7, ImGuiTableFlags_ScrollY, ImGui::GetContentRegionAvail())) return;
    ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, Px(150));
    ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, Px(90));
    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, Px(90));
    ImGui::TableSetupColumn("Statement", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Rev", ImGuiTableColumnFlags_WidthFixed, Px(45));
    ImGui::TableSetupColumn("Evidence", ImGuiTableColumnFlags_WidthFixed, Px(70));
    ImGui::TableSetupColumn("Byte check", ImGuiTableColumnFlags_WidthFixed, Px(90));
    ImGui::TableHeadersRow();
    for (const auto& row : rows) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(row.id.c_str());
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(row.kind.c_str());
        ImGui::TableSetColumnIndex(2);
        ImGui::TextUnformatted(row.status.c_str());
        ImGui::TableSetColumnIndex(3);
        ImGui::TextWrapped("%s", row.statement.c_str());
        ImGui::TableSetColumnIndex(4);
        ImGui::Text("%d", row.revision);
        ImGui::TableSetColumnIndex(5);
        ImGui::Text("%d", row.evidenceCount);
        ImGui::TableSetColumnIndex(6);
        ImGui::TextUnformatted(row.verification.c_str());
    }
    ImGui::EndTable();
}

void ProjectWorkspace::DrawContextTab(UiContext& context) {
    if (!context.contextModel) return;
    auto& model = *context.contextModel;
    if (ImGui::Button("Refresh context")) RefreshContext(context);
    ImGui::SameLine();
    if (model.Pid())
        ImGui::TextDisabled("PID %llu, generation %llu%s", static_cast<unsigned long long>(model.Pid()),
                            static_cast<unsigned long long>(model.Generation()),
                            model.DroppedEntries() ? " (older entries dropped)" : "");
    HintText("Shared with MCP clients (context_get). Right-click any address > Share with investigation context. "
             "Agent entries are assertions with their session as provenance, not verified facts.");

    if (BeginDataTable("ProjectContextTable", 6, ImGuiTableFlags_ScrollY,
                       ImVec2(0.0f, ImGui::GetContentRegionAvail().y * 0.62f))) {
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, Px(45));
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, Px(90));
        ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, Px(80));
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, Px(220));
        ImGui::TableSetupColumn("Text", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, Px(110));
        ImGui::TableHeadersRow();
        const auto& entries = model.Entries();
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
            ImGui::PushID(it->id.c_str());
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%llu", static_cast<unsigned long long>(it->sequence));
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(it->kind.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(it->source.c_str());
            ImGui::TableSetColumnIndex(3);
            const std::string where = it->location.empty() ? it->address : it->location;
            if (!it->address.empty()) {
                if (ImGui::Selectable(where.c_str())) {
                    try {
                        context.NavigateTo("memory-browser", std::stoull(it->address, nullptr, 0));
                    } catch (...) {
                        context.status = "Invalid context address " + it->address;
                    }
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s - click to open in the memory viewer", it->address.c_str());
            }
            ImGui::TableSetColumnIndex(4);
            ImGui::TextWrapped("%s", it->text.c_str());
            ImGui::TableSetColumnIndex(5);
            ImGui::TextUnformatted(it->status.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::TextDisabled("Recent tool calls");
    if (BeginDataTable("ProjectContextHistory", 5, ImGuiTableFlags_ScrollY, ImGui::GetContentRegionAvail())) {
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, Px(45));
        ImGui::TableSetupColumn("Tool", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Risk", ImGuiTableColumnFlags_WidthFixed, Px(90));
        ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, Px(80));
        ImGui::TableSetupColumn("Result", ImGuiTableColumnFlags_WidthFixed, Px(110));
        ImGui::TableHeadersRow();
        const auto& history = model.History();
        for (auto it = history.rbegin(); it != history.rend(); ++it) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%llu", static_cast<unsigned long long>(it->sequence));
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(it->tool.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(it->risk.c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(it->source.c_str());
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%s %llums", it->status.c_str(), static_cast<unsigned long long>(it->durationMs));
        }
        ImGui::EndTable();
    }
}

} // namespace cortex::ui
