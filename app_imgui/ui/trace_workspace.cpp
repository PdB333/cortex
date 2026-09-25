#include "trace_workspace.h"

#include <imgui.h>

#include <algorithm>
#include <utility>

namespace cortex::ui {
namespace {

using json = nlohmann::json;

json RouteResult(const json& output) {
    if (!output.is_object()) return output;
    const auto found = output.find("result");
    return found != output.end() ? *found : output;
}

} // namespace

void TraceWorkspace::ResetForTarget(UiContext& context, const std::string& targetId) {
    targetId_ = targetId;
    traces_.clear();
    events_.clear();
    selectedTraceId_ = -1;
    threadId_ = context.debuggerModel ? context.debuggerModel->CurrentThread() : 0;
    if (context.settings) {
        maxSteps_ = context.settings->Values().traceMaxSteps;
        eventLimit_ = context.settings->Values().traceEventLoadLimit;
    } else {
        maxSteps_ = 10000;
        eventLimit_ = 250;
    }
}

bool TraceWorkspace::EnsureRuntime(UiContext& context, bool allowInjection) {
    if (!context.payload) {
        context.status = "Runtime client unavailable";
        return false;
    }
    if (context.payload->Ready()) return true;

    std::string error;
    if (context.payload->TryConnectExisting(&error)) return true;

    if (!allowInjection) {
        context.status = "Cortex runtime is not active in this target";
        return false;
    }
    if (!context.mutationAllowed) {
        context.status = "Enable writes before loading the runtime";
        return false;
    }
    if (!context.payload->EnsureReady(&error)) {
        context.status = "Runtime enable failed: " + error;
        return false;
    }
    return true;
}

bool TraceWorkspace::Call(UiContext& context, const std::string& tool,
                          json arguments, json& result, bool mutation) {
    result = json::object();
    if (!EnsureRuntime(context, mutation)) return false;
    if (mutation) {
        if (!context.mutationAllowed) {
            context.status = "Enable writes before this trace operation";
            return false;
        }
        arguments["mutation_permission"] = true;
    }

    json output;
    std::string error;
    if (!context.payload->CallTool(tool, arguments, output, &error)) {
        context.status = tool + " failed: " + error;
        return false;
    }
    result = RouteResult(output);
    return true;
}

bool TraceWorkspace::RefreshTraces(UiContext& context) {
    json result;
    if (!Call(context, "trace_list", json::object(), result, false)) return false;

    traces_.clear();
    bool selectedStillExists = false;
    const json rows = result.value("traces", json::array());
    if (rows.is_array()) {
        for (const auto& item : rows) {
            if (!item.is_object()) continue;
            TraceRow row;
            row.id = item.value("id", -1);
            row.threadId = item.value("thread_id", uint64_t{0});
            row.active = item.value("active", false);
            row.reason = item.value("stop_reason", std::string());
            row.steps = item.value("steps", uint64_t{0});
            row.eventCount = item.value("event_count", uint64_t{0});
            row.truncated = item.value("truncated", false);
            if (row.id == selectedTraceId_) selectedStillExists = true;
            traces_.push_back(std::move(row));
        }
    }

    if (!selectedStillExists) {
        selectedTraceId_ = -1;
        events_.clear();
    }
    context.status = std::to_string(traces_.size()) + " trace(s)";
    return true;
}

bool TraceWorkspace::LoadEvents(UiContext& context, int traceId) {
    if (traceId < 0) return false;
    eventLimit_ = std::clamp(eventLimit_, 50, 1000);

    json result;
    if (!Call(context, "trace_events",
              {{"_path", {{"id", traceId}}},
               {"_query", {{"offset", 0}, {"limit", eventLimit_}}}},
              result, false)) return false;

    events_.clear();
    const json rows = result.value("events", json::array());
    if (rows.is_array()) {
        for (const auto& item : rows) {
            if (!item.is_object()) continue;
            TraceEvent event;
            event.seq = item.value("seq", uint64_t{0});
            event.threadId = item.value("thread_id", uint64_t{0});
            event.timestampMs = item.value("timestamp_ms", uint64_t{0});
            event.instruction = item.value("instruction", std::string());
            event.bytes = item.value("bytes", std::string());
            if (item.contains("registers")) event.registers = item.at("registers").dump();
            events_.push_back(std::move(event));
        }
    }
    selectedTraceId_ = traceId;
    context.status = std::to_string(events_.size()) + " trace event(s)";
    return true;
}

bool TraceWorkspace::StartTrace(UiContext& context) {
    if (threadId_ == 0) {
        context.status = "Select or enter a thread id";
        return false;
    }
    maxSteps_ = std::clamp(maxSteps_, 100, 1000000);

    json result;
    if (!Call(context, "trace_start",
              {{"thread_id", threadId_},
               {"max_steps", static_cast<uint64_t>(maxSteps_)},
               {"max_events", static_cast<uint64_t>(std::min(maxSteps_, 50000))}},
              result, true)) return false;

    selectedTraceId_ = result.value("id", -1);
    RefreshTraces(context);
    if (selectedTraceId_ >= 0) LoadEvents(context, selectedTraceId_);
    context.status = "Trace started";
    return true;
}

bool TraceWorkspace::StopTrace(UiContext& context, int traceId) {
    json result;
    if (!Call(context, "trace_stop",
              {{"_path", {{"id", traceId}}}}, result, true)) return false;
    RefreshTraces(context);
    if (selectedTraceId_ == traceId) LoadEvents(context, traceId);
    context.status = "Trace stopped";
    return true;
}

bool TraceWorkspace::DeleteTrace(UiContext& context, int traceId) {
    json result;
    if (!Call(context, "trace_delete",
              {{"_path", {{"id", traceId}}}}, result, true)) return false;
    if (selectedTraceId_ == traceId) {
        selectedTraceId_ = -1;
        events_.clear();
    }
    RefreshTraces(context);
    context.status = "Trace deleted";
    return true;
}

void TraceWorkspace::Draw(UiContext& context) {
    const auto session = context.sessions ? context.sessions->Active() : nullptr;
    if (!session) {
        ImGui::TextDisabled("Select a process to trace execution.");
        return;
    }

    if (targetId_ != session->Target().id)
        ResetForTarget(context, session->Target().id);

    if (threadId_ == 0 && context.debuggerModel)
        threadId_ = context.debuggerModel->CurrentThread();

    ImGui::TextUnformatted("Execution traces");
    ImGui::SameLine();
    ImGui::TextDisabled(context.payload && context.payload->Ready()
                            ? "runtime connected" : "runtime disconnected");

    ImGui::SameLine();
    if (ImGui::SmallButton("Connect existing"))
        EnsureRuntime(context, false);
    ImGui::SameLine();
    ImGui::BeginDisabled(!context.mutationAllowed);
    if (ImGui::SmallButton("Enable runtime"))
        EnsureRuntime(context, true);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh"))
        RefreshTraces(context);

    ImGui::Separator();

    ImGui::SetNextItemWidth(155);
    ImGui::InputScalar("Thread", ImGuiDataType_U64, &threadId_);
    ImGui::SameLine();
    if (context.debuggerModel && context.debuggerModel->CurrentThread() != 0) {
        if (ImGui::SmallButton("Use debugger thread"))
            threadId_ = context.debuggerModel->CurrentThread();
        ImGui::SameLine();
    }
    ImGui::SetNextItemWidth(130);
    ImGui::InputInt("Max steps", &maxSteps_);
    maxSteps_ = std::clamp(maxSteps_, 100, 1000000);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    ImGui::InputInt("Events", &eventLimit_);
    eventLimit_ = std::clamp(eventLimit_, 50, 1000);
    ImGui::SameLine();
    ImGui::BeginDisabled(!context.mutationAllowed || threadId_ == 0);
    if (ImGui::Button("Start trace")) StartTrace(context);
    ImGui::EndDisabled();

    const float traceListHeight =
        std::clamp(ImGui::GetContentRegionAvail().y * 0.36f, 170.0f, 300.0f);

    int stopId = -1;
    int deleteId = -1;
    if (ImGui::BeginTable("TraceTable", 7,
                          ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_BordersInnerH |
                          ImGuiTableFlags_Resizable |
                          ImGuiTableFlags_ScrollY,
                          ImVec2(0, traceListHeight))) {
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 45);
        ImGui::TableSetupColumn("Thread", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 75);
        ImGui::TableSetupColumn("Steps", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Events", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Stop reason", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 155);
        ImGui::TableHeadersRow();

        for (const auto& trace : traces_) {
            ImGui::PushID(trace.id);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (ImGui::Selectable(std::to_string(trace.id).c_str(),
                                  trace.id == selectedTraceId_,
                                  ImGuiSelectableFlags_SpanAllColumns)) {
                LoadEvents(context, trace.id);
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%llu", static_cast<unsigned long long>(trace.threadId));
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(trace.active ? "active" : "stopped");
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%llu", static_cast<unsigned long long>(trace.steps));
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%llu%s",
                        static_cast<unsigned long long>(trace.eventCount),
                        trace.truncated ? "+" : "");
            ImGui::TableSetColumnIndex(5);
            ImGui::TextUnformatted(trace.reason.empty() ? "-" : trace.reason.c_str());
            ImGui::TableSetColumnIndex(6);
            if (trace.active) {
                ImGui::BeginDisabled(!context.mutationAllowed);
                if (ImGui::SmallButton("Stop")) stopId = trace.id;
                ImGui::EndDisabled();
                ImGui::SameLine();
            }
            ImGui::BeginDisabled(!context.mutationAllowed);
            if (ImGui::SmallButton("Delete")) deleteId = trace.id;
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (stopId >= 0) StopTrace(context, stopId);
    if (deleteId >= 0) DeleteTrace(context, deleteId);

    ImGui::Spacing();
    ImGui::Text("Trace events%s",
                selectedTraceId_ >= 0
                    ? (" #" + std::to_string(selectedTraceId_)).c_str()
                    : "");
    ImGui::SameLine();
    ImGui::BeginDisabled(selectedTraceId_ < 0);
    if (ImGui::SmallButton("Reload events"))
        LoadEvents(context, selectedTraceId_);
    ImGui::EndDisabled();

    if (events_.empty()) {
        ImGui::TextDisabled(selectedTraceId_ < 0
                                ? "Select a trace to inspect its events."
                                : "No events recorded for this trace.");
        return;
    }

    if (ImGui::BeginTable("TraceEventsTable", 6,
                          ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_BordersInnerH |
                          ImGuiTableFlags_Resizable |
                          ImGuiTableFlags_ScrollY,
                          ImGui::GetContentRegionAvail())) {
        ImGui::TableSetupColumn("Seq", ImGuiTableColumnFlags_WidthFixed, 65);
        ImGui::TableSetupColumn("Thread", ImGuiTableColumnFlags_WidthFixed, 85);
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 115);
        ImGui::TableSetupColumn("Instruction", ImGuiTableColumnFlags_WidthStretch, 0.42f);
        ImGui::TableSetupColumn("Bytes", ImGuiTableColumnFlags_WidthStretch, 0.22f);
        ImGui::TableSetupColumn("Registers", ImGuiTableColumnFlags_WidthStretch, 0.36f);
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(events_.size()));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const auto& event = events_[static_cast<size_t>(row)];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%llu", static_cast<unsigned long long>(event.seq));
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%llu", static_cast<unsigned long long>(event.threadId));
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%llu", static_cast<unsigned long long>(event.timestampMs));
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(event.instruction.c_str());
                ImGui::TableSetColumnIndex(4);
                ImGui::TextUnformatted(event.bytes.c_str());
                ImGui::TableSetColumnIndex(5);
                ImGui::TextUnformatted(event.registers.c_str());
            }
        }
        ImGui::EndTable();
    }
}

} // namespace cortex::ui
