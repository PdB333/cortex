#include "debugger_workspace.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>

namespace cortex::ui {
namespace {

const char* BreakpointKind(int index) {
    switch (index) {
        case 1: return "hw_execute";
        case 2: return "hw_write";
        case 3: return "hw_readwrite";
        default: return "software";
    }
}

} // namespace

bool DebuggerWorkspace::RequireMutation(UiContext& context) {
    if (context.mutationAllowed) return true;
    context.status = "Enable writes before debugger mutations";
    return false;
}

void DebuggerWorkspace::Refresh(UiContext& context, bool attachRuntimeState) {
    if (!context.debuggerModel) return;
    std::string error;
    if (!context.debuggerModel->RefreshThreads(&error)) {
        context.status = "Thread refresh failed: " + error;
        return;
    }
    if (attachRuntimeState && !context.debuggerModel->RefreshRuntime(&error)) {
        context.status = "Debugger refresh failed: " + error;
        return;
    }
    context.status = std::to_string(context.debuggerModel->Threads().size()) + " thread(s)";
}

void DebuggerWorkspace::SelectThread(UiContext& context, uint64_t threadId) {
    if (!context.debuggerModel) return;
    std::string error;
    if (!context.debuggerModel->SelectThread(threadId, &error)) {
        context.status = "Register read failed: " + error;
        return;
    }
    context.status = "Thread " + std::to_string(threadId);
}

void DebuggerWorkspace::Draw(UiContext& context) {
    const auto session = context.sessions ? context.sessions->Active() : nullptr;
    if (!session || !context.debuggerModel) {
        ImGui::TextDisabled("Select a process to use the debugger.");
        return;
    }

    if (targetId_ != session->Target().id) {
        targetId_ = session->Target().id;
        context.debuggerModel->Reset();
        if (context.settings) {
            breakpointAction_ =
                context.settings->Values().breakpointDefaultAction == "pause" ? 1 : 0;
            processGlobal_ = context.settings->Values().hardwareBreakpointsGlobal;
        }
        Refresh(context, false);
    }

    auto& debugger = *context.debuggerModel;
    const std::string backend = debugger.Backend();

    ImGui::TextUnformatted("Debugger");
    ImGui::SameLine();
    ImGui::TextDisabled("backend: %s%s", backend.c_str(),
                        backend == "veh" ? " (in-process)" : " (external)");
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh")) Refresh(context, debugger.Ready());

    ImGui::SameLine();
    if (!debugger.Ready()) {
        const bool injectionNeedsPermission = backend == "veh" && !context.mutationAllowed;
        ImGui::BeginDisabled(injectionNeedsPermission);
        if (ImGui::SmallButton("Attach debugger")) {
            std::string error;
            if (debugger.EnsureAttached(&error)) {
                Refresh(context, true);
                context.status = "Debugger attached (" + backend + ")";
            } else {
                context.status = "Debugger attach failed: " + error;
            }
        }
        ImGui::EndDisabled();
    } else {
        ImGui::TextDisabled("attached");
    }

    ImGui::Separator();

    const uint64_t currentThread = debugger.CurrentThread();
    ImGui::BeginDisabled(!context.mutationAllowed || currentThread == 0);
    if (ImGui::Button("Pause")) {
        std::string error;
        if (!debugger.Pause(&error)) context.status = "Pause failed: " + error;
        else context.status = "Thread paused";
    }
    ImGui::SameLine();
    if (ImGui::Button("Continue")) {
        std::string error;
        if (!debugger.Resume(&error)) context.status = "Continue failed: " + error;
        else context.status = "Thread continued";
    }
    ImGui::SameLine();
    if (ImGui::Button("Step Into")) {
        std::string error;
        if (!debugger.Step(2000, &error)) context.status = "Step failed: " + error;
        else context.status = "Step complete";
    }
    ImGui::SameLine();
    if (ImGui::Button("Step Over")) {
        std::string error;
        if (!debugger.StepOver(5000, &error)) context.status = "Step over failed: " + error;
        else context.status = "Step over complete";
    }
    ImGui::EndDisabled();

    const auto& snapshot = debugger.Snapshot();
    if (snapshot.instructionPointer) {
        ImGui::SameLine();
        if (ImGui::Button("Disassemble IP")) {
            context.navigationAddress = snapshot.instructionPointer;
            context.navigationAddressPending = true;
            context.requestWorkspace = "disassembly";
        }
    }

    ImGui::Spacing();
    const float upperHeight = std::clamp(ImGui::GetContentRegionAvail().y * 0.48f, 230.0f, 420.0f);
    const float threadWidth = 220.0f;

    ImGui::BeginChild("ThreadList", ImVec2(threadWidth, upperHeight), ImGuiChildFlags_Borders);
    ImGui::Text("Threads (%zu)", debugger.Threads().size());
    ImGui::Separator();
    for (const uint64_t threadId : debugger.Threads()) {
        const bool selected = threadId == currentThread;
        const std::string label = "TID " + std::to_string(threadId);
        if (ImGui::Selectable(label.c_str(), selected)) SelectThread(context, threadId);
    }
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("RegisterView", ImVec2(0, upperHeight), ImGuiChildFlags_Borders);
    ImGui::Text("Registers - TID %llu", static_cast<unsigned long long>(currentThread));
    ImGui::Separator();
    if (snapshot.registers.empty()) {
        ImGui::TextDisabled("Select a thread to read its registers.");
    } else if (ImGui::BeginTable("RegisterTable", 2,
                                 ImGuiTableFlags_RowBg |
                                 ImGuiTableFlags_BordersInnerH |
                                 ImGuiTableFlags_ScrollY,
                                 ImGui::GetContentRegionAvail())) {
        ImGui::TableSetupColumn("Register", ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const auto& reg : snapshot.registers) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(reg.name.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("0x%llX", static_cast<unsigned long long>(reg.value));
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::BeginChild("BreakpointPanel", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::TextUnformatted("Breakpoints");
    ImGui::SameLine();
    ImGui::BeginDisabled(!debugger.Ready());
    if (ImGui::SmallButton("Refresh state")) Refresh(context, true);
    ImGui::EndDisabled();

    ImGui::SetNextItemWidth(210);
    ImGui::InputTextWithHint("##BreakpointAddress", "address or module+offset",
                             breakpointAddress_, sizeof(breakpointAddress_));
    ImGui::SameLine();
    const char* kinds[] = {"Software", "HW execute", "HW write", "HW read/write"};
    ImGui::SetNextItemWidth(130);
    ImGui::Combo("##BreakpointKind", &breakpointKind_, kinds, IM_ARRAYSIZE(kinds));
    ImGui::SameLine();
    if (breakpointKind_ >= 2) {
        ImGui::SetNextItemWidth(90);
        ImGui::InputInt("##BreakpointSize", &breakpointSize_);
        breakpointSize_ = std::clamp(breakpointSize_, 1, 8);
        ImGui::SameLine();
    }
    const char* actions[] = {"Log", "Pause"};
    ImGui::SetNextItemWidth(90);
    ImGui::Combo("##BreakpointAction", &breakpointAction_, actions, IM_ARRAYSIZE(actions));
    ImGui::SameLine();
    ImGui::Checkbox("Process global", &processGlobal_);
    ImGui::SameLine();

    ImGui::BeginDisabled(!context.mutationAllowed || breakpointAddress_[0] == '\0');
    if (ImGui::Button("Add breakpoint")) {
        std::string error;
        const int size = breakpointKind_ == 1 ? 1 : breakpointSize_;
        const uint64_t threadId = processGlobal_ ? 0 : currentThread;
        if (!debugger.AddBreakpoint(breakpointAddress_, BreakpointKind(breakpointKind_),
                                    size, breakpointAction_ == 1, processGlobal_, threadId,
                                    &error)) {
            context.status = "Add breakpoint failed: " + error;
        } else {
            context.status = "Breakpoint added";
        }
    }
    ImGui::EndDisabled();

    ImGui::Separator();

    if (ImGui::BeginTable("BreakpointTable", 7,
                          ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_BordersInnerH |
                          ImGuiTableFlags_Resizable |
                          ImGuiTableFlags_ScrollY,
                          ImVec2(0, 0))) {
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 42);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 145);
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 105);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 65);
        ImGui::TableSetupColumn("Hits", ImGuiTableColumnFlags_WidthFixed, 65);
        ImGui::TableSetupColumn("Coverage", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableHeadersRow();

        int removeBreakpointId = -1;
        for (const auto& bp : debugger.Breakpoints()) {
            ImGui::PushID(bp.id);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", bp.id);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("0x%llX", static_cast<unsigned long long>(bp.address));
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(bp.kind.c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(bp.pauseOnHit ? "pause" : "log");
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%llu", static_cast<unsigned long long>(bp.hitCount));
            ImGui::TableSetColumnIndex(5);
            if (bp.totalThreads)
                ImGui::Text("%zu / %zu threads", bp.appliedThreads, bp.totalThreads);
            else
                ImGui::TextUnformatted(bp.processGlobal ? "process" : "thread");
            ImGui::TableSetColumnIndex(6);
            ImGui::BeginDisabled(!context.mutationAllowed);
            if (ImGui::SmallButton("Remove"))
                removeBreakpointId = bp.id;
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();

        if (removeBreakpointId >= 0) {
            std::string error;
            if (!debugger.RemoveBreakpoint(removeBreakpointId, &error))
                context.status = "Remove breakpoint failed: " + error;
            else
                context.status = "Breakpoint removed";
        }
    }

    ImGui::EndChild();
}

} // namespace cortex::ui
