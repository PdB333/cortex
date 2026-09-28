#include "instrumentation_workspace.h"
#include "widgets.h"

#include <imgui.h>

#include <algorithm>

namespace cortex::ui {

bool InstrumentationWorkspace::Navigate(UiContext& context,
                                        const std::string& rawAddress,
                                        const char* workspace) {
    try {
        size_t used = 0;
        const uint64_t address = std::stoull(rawAddress, &used, 0);
        if (used != rawAddress.size()) return false;
        context.NavigateTo(workspace, address);
        return true;
    } catch (...) {
        return false;
    }
}

void InstrumentationWorkspace::Draw(UiContext& context) {
    const auto session = context.sessions ? context.sessions->Active() : nullptr;
    if (!session || !context.instrumentationModel) {
        HintText("Select a process to use instrumentation.");
        return;
    }

    if (targetId_ != session->Target().id) {
        targetId_ = session->Target().id;
        context.instrumentationModel->Reset();
        allocationEnabled_ = false;
        allocationMinSize_ = 0;
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Runtime instrumentation");
    FlowSameLine(std::max(TextWidth("runtime connected"), TextWidth("runtime disconnected")));
    ImGui::TextDisabled(context.payload && context.payload->Ready()
                            ? "runtime connected" : "runtime disconnected");
    FlowSameLine(ButtonWidth("Connect existing"));
    if (context.payload && !context.payload->Ready()) {
        if (ImGui::Button("Connect existing")) {
            std::string error;
            if (!context.payload->TryConnectExisting(&error))
                context.status = "Runtime connect failed: " + error;
            else if (context.instrumentationModel->RefreshState(nullptr)) {
                allocationEnabled_ = context.instrumentationModel->AllocationWatchEnabled();
                allocationMinSize_ = context.instrumentationModel->AllocationWatchMinSize();
            }
        }
        FlowSameLine(ButtonWidth("Enable runtime"));
        ImGui::BeginDisabled(!context.mutationAllowed);
        if (ImGui::Button("Enable runtime")) {
            std::string error;
            if (!context.payload->EnsureReady(&error))
                context.status = "Runtime enable failed: " + error;
            else if (context.instrumentationModel->RefreshState(nullptr)) {
                allocationEnabled_ = context.instrumentationModel->AllocationWatchEnabled();
                allocationMinSize_ = context.instrumentationModel->AllocationWatchMinSize();
            }
        }
        ImGui::EndDisabled();
        FlowSameLine(ButtonWidth("Refresh state"));
    }
    if (ImGui::Button("Refresh state")) {
        std::string error;
        if (!context.instrumentationModel->RefreshState(&error))
            context.status = "Instrumentation refresh failed: " + error;
        else {
            allocationEnabled_ = context.instrumentationModel->AllocationWatchEnabled();
            allocationMinSize_ = context.instrumentationModel->AllocationWatchMinSize();
            context.status = "Instrumentation state refreshed";
        }
    }
    FlowSameLine(ButtonWidth("Refresh events"));
    if (ImGui::Button("Refresh events")) {
        std::string error;
        if (!context.instrumentationModel->RefreshEvents(&error))
            context.status = "Instrumentation events failed: " + error;
        else
            context.status = "Instrumentation events refreshed";
    }

    ImGui::Separator();

    if (ImGui::BeginTabBar("InstrumentationTabs")) {
        if (ImGui::BeginTabItem("Page access")) {
            ImGui::SetNextItemWidth(Px(220));
            ImGui::InputTextWithHint("##PageWatchAddress", "Address / expression",
                                     pageAddress_.data(), pageAddress_.size());
            FlowSameLine(Px(100));
            ImGui::SetNextItemWidth(Px(100));
            ImGui::InputInt("Size##PageWatch", &pageSize_);
            pageSize_ = std::clamp(pageSize_, 1, 64 * 1024 * 1024);
            FlowSameLine(Px(180));
            ImGui::SetNextItemWidth(Px(180));
            ImGui::InputTextWithHint("##PageWatchLabel", "Optional label",
                                     pageLabel_.data(), pageLabel_.size());
            FlowSameLine(ButtonWidth("Watch page access"));
            ImGui::BeginDisabled(!context.mutationAllowed || pageAddress_[0] == '\0');
            if (ImGui::Button("Watch page access")) {
                std::string error;
                if (!context.instrumentationModel->AddPageAccessWatch(
                        pageAddress_.data(), pageSize_, pageLabel_.data(),
                        context.mutationAllowed, &error))
                    context.status = "Page watch failed: " + error;
                else
                    context.status = "Page-access watch added";
            }
            ImGui::EndDisabled();

            int deleteId = -1;
            if (BeginDataTable("PageWatches", 5,
                                  ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_BordersInnerH |
                                  ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_ScrollY,
                                  ImVec2(0, Px(190)))) {
                ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, Px(50));
                ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch, 0.32f);
                ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, Px(90));
                ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch, 0.38f);
                ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, Px(80));
                ImGui::TableHeadersRow();
                for (const auto& watch : context.instrumentationModel->PageAccessWatches()) {
                    ImGui::PushID(watch.id);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%d", watch.id);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(watch.address.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%llu", static_cast<unsigned long long>(watch.size));
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(watch.label.empty() ? "-" : watch.label.c_str());
                    ImGui::TableSetColumnIndex(4);
                    ImGui::BeginDisabled(!context.mutationAllowed);
                    if (ImGui::SmallButton("Delete")) deleteId = watch.id;
                    ImGui::EndDisabled();
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            if (deleteId >= 0) {
                std::string error;
                if (!context.instrumentationModel->DeletePageAccessWatch(
                        deleteId, context.mutationAllowed, &error))
                    context.status = "Delete page watch failed: " + error;
                else
                    context.status = "Page-access watch deleted";
            }

            ImGui::Text("Access events (%zu)",
                        context.instrumentationModel->PageAccessEvents().size());
            if (BeginDataTable("PageAccessEvents", 9,
                                  ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_BordersInnerH |
                                  ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_ScrollY,
                                  ImGui::GetContentRegionAvail())) {
                ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, Px(105));
                ImGui::TableSetupColumn("Watch", ImGuiTableColumnFlags_WidthFixed, Px(55));
                ImGui::TableSetupColumn("Access", ImGuiTableColumnFlags_WidthFixed, Px(70));
                ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch, 0.18f);
                ImGui::TableSetupColumn("Instruction", ImGuiTableColumnFlags_WidthStretch, 0.18f);
                ImGui::TableSetupColumn("Thread", ImGuiTableColumnFlags_WidthFixed, Px(80));
                ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, Px(55));
                ImGui::TableSetupColumn("Before -> After", ImGuiTableColumnFlags_WidthStretch, 0.25f);
                ImGui::TableSetupColumn("Context", ImGuiTableColumnFlags_WidthStretch, 0.25f);
                ImGui::TableHeadersRow();

                for (const auto& event : context.instrumentationModel->PageAccessEvents()) {
                    ImGui::PushID(static_cast<int>(event.timestampMs ^ event.threadId));
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%lld", static_cast<long long>(event.timestampMs));
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%d", event.watchId);
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(event.access.c_str());
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(event.address.c_str());
                    if (ImGui::IsItemHovered() &&
                        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        Navigate(context, event.address, "memory-browser");
                    ImGui::TableSetColumnIndex(4);
                    ImGui::TextUnformatted(event.instruction.c_str());
                    if (ImGui::IsItemHovered() &&
                        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        Navigate(context, event.instruction, "disassembly");
                    ImGui::TableSetColumnIndex(5);
                    ImGui::Text("%llu", static_cast<unsigned long long>(event.threadId));
                    ImGui::TableSetColumnIndex(6);
                    ImGui::Text("%llu", static_cast<unsigned long long>(event.size));
                    ImGui::TableSetColumnIndex(7);
                    ImGui::Text("%s -> %s", event.before.c_str(), event.after.c_str());
                    ImGui::TableSetColumnIndex(8);
                    ImGui::TextUnformatted(event.label.empty() ? "details" : event.label.c_str());
                    if (ImGui::IsItemHovered()) {
                        ImGui::BeginTooltip();
                        ImGui::TextWrapped("registers: %s", event.registersJson.c_str());
                        ImGui::Separator();
                        ImGui::TextWrapped("stack: %s", event.stackJson.c_str());
                        ImGui::EndTooltip();
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Allocations")) {
            ImGui::Checkbox("Allocation watch enabled", &allocationEnabled_);
            FlowSameLine(Px(160));
            ImGui::SetNextItemWidth(Px(160));
            ImGui::InputScalar("Minimum size", ImGuiDataType_U64, &allocationMinSize_);
            FlowSameLine(ButtonWidth("Apply allocation watch"));
            ImGui::BeginDisabled(!context.mutationAllowed);
            if (ImGui::Button("Apply allocation watch")) {
                std::string error;
                if (!context.instrumentationModel->SetAllocationWatch(
                        allocationEnabled_, allocationMinSize_,
                        context.mutationAllowed, &error))
                    context.status = "Allocation watch failed: " + error;
                else {
                    allocationEnabled_ = context.instrumentationModel->AllocationWatchEnabled();
                    allocationMinSize_ = context.instrumentationModel->AllocationWatchMinSize();
                    context.status = "Allocation watch updated";
                }
            }
            ImGui::EndDisabled();

            if (BeginDataTable("AllocationEvents", 5,
                                  ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_BordersInnerH |
                                  ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_ScrollY,
                                  ImGui::GetContentRegionAvail())) {
                ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, Px(115));
                ImGui::TableSetupColumn("API", ImGuiTableColumnFlags_WidthStretch, 0.22f);
                ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch, 0.30f);
                ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, Px(100));
                ImGui::TableSetupColumn("Flags", ImGuiTableColumnFlags_WidthStretch, 0.20f);
                ImGui::TableHeadersRow();
                for (const auto& event : context.instrumentationModel->AllocationEvents()) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%lld", static_cast<long long>(event.timestampMs));
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(event.api.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(event.address.c_str());
                    if (ImGui::IsItemHovered() &&
                        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        Navigate(context, event.address, "memory-browser");
                    ImGui::TableSetColumnIndex(3);
                    ImGui::Text("%llu", static_cast<unsigned long long>(event.size));
                    ImGui::TableSetColumnIndex(4);
                    MonoText("0x%llX", static_cast<unsigned long long>(event.flags));
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
}

} // namespace cortex::ui
