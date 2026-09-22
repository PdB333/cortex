#include "re_workspace.h"

#include <imgui.h>

#include <algorithm>
#include <cstring>

namespace cortex::ui {
namespace {

void Copy(char* destination, size_t capacity, const char* source) {
    if (!destination || capacity == 0) return;
    std::memset(destination, 0, capacity);
    if (!source) return;
    const size_t size = std::strlen(source);
    std::memcpy(destination, source, std::min(capacity - 1, size));
}

} // namespace

void ReWorkspace::ResetForTarget(UiContext& context, const std::string& targetId) {
    targetId_ = targetId;
    if (context.reModel) context.reModel->Reset();

    trackName_.fill(0);
    trackAddress_.fill(0);
    trackPointerPath_.fill(0);
    trackStruct_.fill(0);
    trackSize_ = 256;
    trackPersist_ = true;

    analysisAddress_.fill(0);
    analysisSize_ = 1;
    analysisTimeoutMs_ = 5000;
    subobjectSize_ = 256;

    transitionJson_.fill(0);
    Copy(transitionJson_.data(), transitionJson_.size(),
         "{\n  \"address\": \"0x0\",\n  \"size\": 4,\n  \"timeout_ms\": 5000\n}");

    experimentJson_.fill(0);
    Copy(experimentJson_.data(), experimentJson_.size(),
         "{\n  \"steps\": [\n    {\"action\":\"delay\",\"ms\":100}\n  ],\n  \"rollback_ranges\": [],\n  \"commit\": false\n}");

    factKey_.fill(0);
    factValue_.fill(0);
    checkpointLabel_.fill(0);
    checkpointRanges_.fill(0);
    Copy(checkpointRanges_.data(), checkpointRanges_.size(), "[]");
    selectedCheckpoint_ = -1;
    sessionA_ = -1;
    sessionB_ = -1;

    ghidraName_.fill(0);
    ghidraImportJson_.fill(0);
    Copy(ghidraImportJson_.data(), ghidraImportJson_.size(), "{\"symbols\":[]}");
    breakpointTemplatesJson_.fill(0);
    Copy(breakpointTemplatesJson_.data(), breakpointTemplatesJson_.size(), "[]");
}

void ReWorkspace::RefreshAll(UiContext& context) {
    if (!context.reModel) return;
    std::string error;
    if (!context.reModel->Refresh(&error)) {
        context.status = "RE refresh failed: " + error;
        return;
    }
    context.reModel->RefreshSessions(nullptr);
    context.reModel->RefreshCheckpoints(nullptr);
    context.status = std::to_string(context.reModel->Tracks().size()) + " tracked object(s)";
}

void ReWorkspace::Draw(UiContext& context) {
    const auto session = context.sessions ? context.sessions->Active() : nullptr;
    if (!session || !context.reModel) {
        ImGui::TextDisabled("Select a process to use the RE workspace.");
        return;
    }

    if (targetId_ != session->Target().id)
        ResetForTarget(context, session->Target().id);

    ImGui::TextUnformatted("Reverse Engineering");
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
                RefreshAll(context);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!context.mutationAllowed);
        if (ImGui::SmallButton("Enable runtime")) {
            std::string error;
            if (!context.payload->EnsureReady(&error))
                context.status = "Runtime enable failed: " + error;
            else
                RefreshAll(context);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
    }
    if (ImGui::SmallButton("Refresh")) RefreshAll(context);

    ImGui::Separator();

    if (ImGui::BeginTabBar("ReTabs")) {
        if (ImGui::BeginTabItem("Objects")) {
            ImGui::SetNextItemWidth(150);
            ImGui::InputTextWithHint("##ReTrackName", "Name",
                                     trackName_.data(), trackName_.size());
            ImGui::SameLine();
            ImGui::SetNextItemWidth(180);
            ImGui::InputTextWithHint("##ReTrackAddress", "Address",
                                     trackAddress_.data(), trackAddress_.size());
            ImGui::SameLine();
            ImGui::SetNextItemWidth(180);
            ImGui::InputTextWithHint("##ReTrackPath", "Pointer path",
                                     trackPointerPath_.data(), trackPointerPath_.size());
            ImGui::SameLine();
            ImGui::SetNextItemWidth(130);
            ImGui::InputTextWithHint("##ReTrackStruct", "Struct",
                                     trackStruct_.data(), trackStruct_.size());
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90);
            ImGui::InputInt("Size##ReTrack", &trackSize_);
            trackSize_ = std::clamp(trackSize_, 1, 1024 * 1024);
            ImGui::SameLine();
            ImGui::Checkbox("Persist", &trackPersist_);
            ImGui::SameLine();
            ImGui::BeginDisabled(!context.mutationAllowed ||
                                 (trackAddress_[0] == '\0' && trackPointerPath_[0] == '\0'));
            if (ImGui::Button("Track")) {
                std::string error;
                if (!context.reModel->TrackObject(
                        trackName_.data(), trackAddress_.data(), trackPointerPath_.data(),
                        trackSize_, trackPersist_, trackStruct_.data(),
                        context.mutationAllowed, &error))
                    context.status = "Track object failed: " + error;
                else
                    context.status = "Object tracked";
            }
            ImGui::EndDisabled();

            int deleteTrack = -1;
            if (ImGui::BeginTable("ReTracksTable", 7,
                                  ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_BordersInnerH |
                                  ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_ScrollY,
                                  ImVec2(0, 300))) {
                ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 65);
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.22f);
                ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch, 0.20f);
                ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 70);
                ImGui::TableSetupColumn("Pointer path", ImGuiTableColumnFlags_WidthStretch, 0.24f);
                ImGui::TableSetupColumn("Struct", ImGuiTableColumnFlags_WidthStretch, 0.16f);
                ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 135);
                ImGui::TableHeadersRow();

                for (const auto& row : context.reModel->Tracks()) {
                    ImGui::PushID(row.id);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(row.alive ? "alive" : "stale");
                    ImGui::TableSetColumnIndex(1);
                    if (ImGui::Selectable(row.name.c_str(),
                                          row.id == context.reModel->SelectedTrackId())) {
                        std::string error;
                        if (!context.reModel->SelectTrack(row.id, &error))
                            context.status = "Select track failed: " + error;
                    }
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(row.address.c_str());
                    ImGui::TableSetColumnIndex(3);
                    ImGui::Text("%d", row.size);
                    ImGui::TableSetColumnIndex(4);
                    ImGui::TextUnformatted(row.pointerPath.empty() ? "-" : row.pointerPath.c_str());
                    ImGui::TableSetColumnIndex(5);
                    ImGui::TextUnformatted(row.structName.empty() ? "-" : row.structName.c_str());
                    ImGui::TableSetColumnIndex(6);
                    if (ImGui::SmallButton("Analyze")) {
                        std::string error;
                        context.reModel->SelectTrack(row.id, &error);
                        Copy(analysisAddress_.data(), analysisAddress_.size(), row.address.c_str());
                    }
                    ImGui::SameLine();
                    ImGui::BeginDisabled(!context.mutationAllowed);
                    if (ImGui::SmallButton("Delete")) deleteTrack = row.id;
                    ImGui::EndDisabled();
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }

            if (deleteTrack >= 0) {
                std::string error;
                if (!context.reModel->DeleteTrack(
                        deleteTrack, context.mutationAllowed, &error))
                    context.status = "Delete track failed: " + error;
                else
                    context.status = "Tracked object deleted";
            }

            ImGui::TextUnformatted("Selected object");
            ImGui::BeginChild("ReSelectedObject", ImVec2(0, 0), ImGuiChildFlags_Borders);
            ImGui::TextWrapped("%s",
                context.reModel->SelectedTrackJson().empty()
                    ? "No tracked object selected."
                    : context.reModel->SelectedTrackJson().c_str());
            if (!context.reModel->TrackEventsJson().empty()) {
                ImGui::Separator();
                ImGui::TextUnformatted("Events");
                ImGui::TextWrapped("%s", context.reModel->TrackEventsJson().c_str());
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Analysis")) {
            if (analysisAddress_[0] == '\0' && context.reModel->SelectedTrackId() >= 0) {
                const std::string selected = context.reModel->SelectedTrackAddress();
                Copy(analysisAddress_.data(), analysisAddress_.size(), selected.c_str());
            }

            ImGui::SetNextItemWidth(260);
            ImGui::InputTextWithHint("##ReAnalysisAddress", "Address / module+RVA / symbol",
                                     analysisAddress_.data(), analysisAddress_.size());
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90);
            ImGui::InputInt("Size##ReAnalysis", &analysisSize_);
            analysisSize_ = std::clamp(analysisSize_, 1, 4096);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110);
            ImGui::InputInt("Timeout ms", &analysisTimeoutMs_);
            analysisTimeoutMs_ = std::clamp(analysisTimeoutMs_, 100, 120000);

            ImGui::BeginDisabled(!context.mutationAllowed || analysisAddress_[0] == '\0');
            if (ImGui::Button("Find last writer")) {
                std::string error;
                if (!context.reModel->FindLastWriter(
                        analysisAddress_.data(), analysisSize_, analysisTimeoutMs_,
                        context.mutationAllowed, &error))
                    context.status = "Find last writer failed: " + error;
                else
                    context.status = "Last-writer analysis complete";
            }
            ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::SetNextItemWidth(100);
            ImGui::InputInt("Object size", &subobjectSize_);
            subobjectSize_ = std::clamp(subobjectSize_, 1, 1024 * 1024);
            ImGui::SameLine();
            ImGui::BeginDisabled(analysisAddress_[0] == '\0');
            if (ImGui::Button("Detect C++ subobjects")) {
                std::string error;
                if (!context.reModel->DetectSubobjects(
                        analysisAddress_.data(), subobjectSize_, &error))
                    context.status = "Subobject analysis failed: " + error;
                else
                    context.status = "Subobject analysis complete";
            }
            ImGui::EndDisabled();

            ImGui::Spacing();
            ImGui::TextUnformatted("Result");
            ImGui::BeginChild("ReAnalysisResult", ImVec2(0, 0), ImGuiChildFlags_Borders);
            ImGui::TextWrapped("%s",
                context.reModel->ResultJson().empty()
                    ? "No analysis result yet."
                    : context.reModel->ResultJson().c_str());
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Transition")) {
            ImGui::TextDisabled("Transition-trace request JSON");
            ImGui::InputTextMultiline("##ReTransitionJson", transitionJson_.data(),
                                      transitionJson_.size(), ImVec2(-1, 300));
            ImGui::BeginDisabled(!context.mutationAllowed);
            if (ImGui::Button("Trace transition")) {
                std::string error;
                if (!context.reModel->TraceTransition(
                        transitionJson_.data(), context.mutationAllowed, &error))
                    context.status = "Transition trace failed: " + error;
                else
                    context.status = "Transition trace complete";
            }
            ImGui::EndDisabled();
            ImGui::Separator();
            ImGui::TextWrapped("%s",
                context.reModel->ResultJson().empty()
                    ? "No transition result yet."
                    : context.reModel->ResultJson().c_str());
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Experiments")) {
            ImGui::TextDisabled("Controlled test / experiment JSON");
            ImGui::InputTextMultiline("##ReExperimentJson", experimentJson_.data(),
                                      experimentJson_.size(), ImVec2(-1, 330));
            ImGui::BeginDisabled(!context.mutationAllowed);
            if (ImGui::Button("Run test")) {
                std::string error;
                if (!context.reModel->RunTest(
                        experimentJson_.data(), false,
                        context.mutationAllowed, &error))
                    context.status = "RE test failed: " + error;
                else
                    context.status = "RE test complete";
            }
            ImGui::SameLine();
            if (ImGui::Button("Run + rollback")) {
                std::string error;
                if (!context.reModel->RunTest(
                        experimentJson_.data(), true,
                        context.mutationAllowed, &error))
                    context.status = "RE experiment failed: " + error;
                else
                    context.status = "RE experiment complete";
            }
            ImGui::EndDisabled();
            ImGui::Separator();
            ImGui::TextWrapped("%s",
                context.reModel->ResultJson().empty()
                    ? "No experiment result yet."
                    : context.reModel->ResultJson().c_str());
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Sessions")) {
            ImGui::TextUnformatted("Persistent fact");
            ImGui::SetNextItemWidth(220);
            ImGui::InputTextWithHint("##ReFactKey", "Fact key",
                                     factKey_.data(), factKey_.size());
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-140);
            ImGui::InputTextWithHint("##ReFactValue", "JSON or text value",
                                     factValue_.data(), factValue_.size());
            ImGui::SameLine();
            ImGui::BeginDisabled(!context.mutationAllowed || factKey_[0] == '\0');
            if (ImGui::Button("Save fact")) {
                std::string error;
                if (!context.reModel->SaveFact(
                        factKey_.data(), factValue_.data(),
                        context.mutationAllowed, &error))
                    context.status = "Save fact failed: " + error;
                else
                    context.status = "RE fact saved";
            }
            ImGui::EndDisabled();

            ImGui::Separator();
            ImGui::TextUnformatted("Checkpoint / rollback");
            ImGui::SetNextItemWidth(180);
            ImGui::InputTextWithHint("##ReCheckpointLabel", "Label",
                                     checkpointLabel_.data(), checkpointLabel_.size());
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-150);
            ImGui::InputTextWithHint("##ReCheckpointRanges", "Ranges JSON",
                                     checkpointRanges_.data(), checkpointRanges_.size());
            ImGui::SameLine();
            ImGui::BeginDisabled(!context.mutationAllowed);
            if (ImGui::Button("Checkpoint")) {
                std::string error;
                if (!context.reModel->CreateCheckpoint(
                        checkpointLabel_.data(), checkpointRanges_.data(),
                        context.mutationAllowed, &error))
                    context.status = "Checkpoint failed: " + error;
                else
                    context.status = "Checkpoint created";
            }
            ImGui::EndDisabled();

            const auto& checkpoints = context.reModel->Checkpoints();
            if (!checkpoints.empty()) {
                if (selectedCheckpoint_ < 0 ||
                    selectedCheckpoint_ >= static_cast<int>(checkpoints.size()))
                    selectedCheckpoint_ = 0;
                ImGui::SetNextItemWidth(280);
                if (ImGui::BeginCombo("##ReCheckpointCombo",
                                      checkpoints[static_cast<size_t>(selectedCheckpoint_)].label.c_str())) {
                    for (size_t i = 0; i < checkpoints.size(); ++i) {
                        const bool selected = static_cast<int>(i) == selectedCheckpoint_;
                        if (ImGui::Selectable(checkpoints[i].label.c_str(), selected))
                            selectedCheckpoint_ = static_cast<int>(i);
                    }
                    ImGui::EndCombo();
                }
                const int checkpointId =
                    checkpoints[static_cast<size_t>(selectedCheckpoint_)].id;
                ImGui::SameLine();
                ImGui::BeginDisabled(!context.mutationAllowed);
                if (ImGui::Button("Rollback")) {
                    std::string error;
                    if (!context.reModel->RollbackCheckpoint(
                            checkpointId, false, context.mutationAllowed, &error))
                        context.status = "Rollback failed: " + error;
                    else
                        context.status = "Checkpoint rolled back";
                }
                ImGui::SameLine();
                if (ImGui::Button("Rollback + keep")) {
                    std::string error;
                    if (!context.reModel->RollbackCheckpoint(
                            checkpointId, true, context.mutationAllowed, &error))
                        context.status = "Rollback failed: " + error;
                    else
                        context.status = "Checkpoint rolled back and kept";
                }
                ImGui::SameLine();
                if (ImGui::Button("Delete checkpoint")) {
                    std::string error;
                    if (!context.reModel->DeleteCheckpoint(
                            checkpointId, context.mutationAllowed, &error))
                        context.status = "Delete checkpoint failed: " + error;
                    else {
                        selectedCheckpoint_ = -1;
                        context.status = "Checkpoint deleted";
                    }
                }
                ImGui::EndDisabled();
            } else {
                ImGui::TextDisabled("No checkpoints.");
            }

            ImGui::Separator();
            ImGui::TextUnformatted("Run history");
            if (ImGui::Button("Refresh runs")) {
                std::string error;
                if (!context.reModel->RefreshSessions(&error))
                    context.status = "Session refresh failed: " + error;
            }
            ImGui::SameLine();
            if (ImGui::Button("Export run")) {
                std::string error;
                if (!context.reModel->ExportSession(&error))
                    context.status = "Session export failed: " + error;
                else
                    context.status = "Session exported";
            }

            const auto& sessions = context.reModel->Sessions();
            if (!sessions.empty()) {
                if (sessionA_ < 0 || sessionA_ >= static_cast<int>(sessions.size())) sessionA_ = 0;
                if (sessionB_ < 0 || sessionB_ >= static_cast<int>(sessions.size()))
                    sessionB_ = sessions.size() > 1 ? 1 : 0;

                ImGui::SetNextItemWidth(240);
                if (ImGui::BeginCombo("Run A", sessions[static_cast<size_t>(sessionA_)].id.c_str())) {
                    for (size_t i = 0; i < sessions.size(); ++i) {
                        if (ImGui::Selectable(sessions[i].id.c_str(),
                                              static_cast<int>(i) == sessionA_))
                            sessionA_ = static_cast<int>(i);
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(240);
                if (ImGui::BeginCombo("Run B", sessions[static_cast<size_t>(sessionB_)].id.c_str())) {
                    for (size_t i = 0; i < sessions.size(); ++i) {
                        if (ImGui::Selectable(sessions[i].id.c_str(),
                                              static_cast<int>(i) == sessionB_))
                            sessionB_ = static_cast<int>(i);
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                ImGui::BeginDisabled(sessionA_ == sessionB_);
                if (ImGui::Button("Diff runs")) {
                    std::string error;
                    if (!context.reModel->DiffSessions(
                            sessions[static_cast<size_t>(sessionA_)].id,
                            sessions[static_cast<size_t>(sessionB_)].id, &error))
                        context.status = "Session diff failed: " + error;
                    else
                        context.status = "Session diff complete";
                }
                ImGui::EndDisabled();
            }

            ImGui::Separator();
            ImGui::TextUnformatted("Current RE session");
            ImGui::BeginChild("ReSessionState", ImVec2(0, 0), ImGuiChildFlags_Borders);
            ImGui::TextWrapped("%s",
                context.reModel->SessionJson().empty()
                    ? "No session data loaded."
                    : context.reModel->SessionJson().c_str());
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Interop")) {
            ImGui::TextUnformatted("Ghidra");
            ImGui::SetNextItemWidth(220);
            ImGui::InputTextWithHint("##ReGhidraName", "Optional export name",
                                     ghidraName_.data(), ghidraName_.size());
            ImGui::SameLine();
            if (ImGui::Button("Export Cortex -> Ghidra")) {
                std::string error;
                if (!context.reModel->GhidraExport(ghidraName_.data(), &error))
                    context.status = "Ghidra export failed: " + error;
                else
                    context.status = "Ghidra export complete";
            }

            ImGui::TextDisabled("Ghidra import JSON");
            ImGui::InputTextMultiline("##ReGhidraImport", ghidraImportJson_.data(),
                                      ghidraImportJson_.size(), ImVec2(-1, 180));
            ImGui::BeginDisabled(!context.mutationAllowed);
            if (ImGui::Button("Import Ghidra data")) {
                std::string error;
                if (!context.reModel->GhidraImport(
                        ghidraImportJson_.data(), context.mutationAllowed, &error))
                    context.status = "Ghidra import failed: " + error;
                else
                    context.status = "Ghidra data imported";
            }
            ImGui::EndDisabled();

            ImGui::Separator();
            ImGui::TextUnformatted("Breakpoint templates");
            ImGui::InputTextMultiline("##ReBreakpointTemplates",
                                      breakpointTemplatesJson_.data(),
                                      breakpointTemplatesJson_.size(),
                                      ImVec2(-1, 180));
            ImGui::BeginDisabled(!context.mutationAllowed);
            if (ImGui::Button("Save templates")) {
                std::string error;
                if (!context.reModel->SaveBreakpointTemplates(
                        breakpointTemplatesJson_.data(),
                        context.mutationAllowed, &error))
                    context.status = "Save templates failed: " + error;
                else
                    context.status = "Breakpoint templates saved";
            }
            ImGui::SameLine();
            if (ImGui::Button("Arm templates")) {
                std::string error;
                if (!context.reModel->ApplyBreakpointTemplates(
                        context.mutationAllowed, &error))
                    context.status = "Apply templates failed: " + error;
                else
                    context.status = "Breakpoint templates armed";
            }
            ImGui::EndDisabled();

            ImGui::Separator();
            ImGui::TextUnformatted("Latest result");
            ImGui::BeginChild("ReInteropResult", ImVec2(0, 0), ImGuiChildFlags_Borders);
            ImGui::TextWrapped("%s",
                context.reModel->ResultJson().empty()
                    ? "No interop result yet."
                    : context.reModel->ResultJson().c_str());
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
}

} // namespace cortex::ui
