#include "debugger_workspace.h"

#include <imgui.h>

namespace cortex::ui {

void DebuggerWorkspace::RefreshThreads(UiContext& context) {
    if (!context.debugger) return;
    std::string error;
    threads_ = context.debugger->Threads(&error);
    if (!error.empty()) {
        context.status = "Thread enumeration failed: " + error;
        return;
    }
    context.status = std::to_string(threads_.size()) + " thread(s)";
    if (!threads_.empty() &&
        std::find(threads_.begin(), threads_.end(), selectedThread_) == threads_.end()) {
        SelectThread(context, threads_.front());
    }
}

void DebuggerWorkspace::SelectThread(UiContext& context, uint64_t threadId) {
    if (!context.debugger) return;
    target::ThreadRegisterSnapshot snapshot;
    std::string error;
    if (!context.debugger->Registers(threadId, snapshot, &error)) {
        context.status = "Register read failed: " + error;
        return;
    }
    selectedThread_ = threadId;
    snapshot_ = std::move(snapshot);
    context.status = "Thread " + std::to_string(threadId);
}

void DebuggerWorkspace::Draw(UiContext& context) {
    const auto session = context.sessions ? context.sessions->Active() : nullptr;
    if (!session) {
        ImGui::TextDisabled("Select a process to inspect threads and registers.");
        return;
    }

    if (targetId_ != session->Target().id) {
        targetId_ = session->Target().id;
        threads_.clear();
        selectedThread_ = 0;
        snapshot_ = {};
        RefreshThreads(context);
    }

    ImGui::TextUnformatted("Debugger");
    ImGui::SameLine();
    ImGui::TextDisabled("External thread/register view");
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh")) RefreshThreads(context);
    ImGui::Spacing();

    const float left = 220.0f;
    ImGui::BeginChild("ThreadList", ImVec2(left, 0), ImGuiChildFlags_Borders);
    ImGui::TextUnformatted("Threads");
    ImGui::Separator();
    for (const uint64_t threadId : threads_) {
        const bool selected = threadId == selectedThread_;
        const std::string label = "TID " + std::to_string(threadId);
        if (ImGui::Selectable(label.c_str(), selected)) SelectThread(context, threadId);
    }
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("RegisterView", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::Text("Registers - TID %llu", static_cast<unsigned long long>(selectedThread_));
    if (snapshot_.instructionPointer) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Disassemble IP")) {
            context.navigationAddress = snapshot_.instructionPointer;
            context.navigationAddressPending = true;
            context.requestWorkspace = "disassembly";
        }
    }
    ImGui::Separator();

    if (snapshot_.registers.empty()) {
        ImGui::TextDisabled("Select a thread to read its register snapshot.");
    } else if (ImGui::BeginTable("RegisterTable", 2,
                                 ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                 ImGuiTableFlags_ScrollY,
                                 ImGui::GetContentRegionAvail())) {
        ImGui::TableSetupColumn("Register", ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const auto& reg : snapshot_.registers) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(reg.name.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("0x%llX", static_cast<unsigned long long>(reg.value));
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

} // namespace cortex::ui
