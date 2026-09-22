#include "input_workspace.h"

#include <imgui.h>

#include <algorithm>
#include <cstring>

namespace cortex::ui {
namespace {
constexpr const char* kModes[] = {"os", "game", "dinput"};

void Copy(char* destination, size_t capacity, const std::string& source) {
    if (!destination || capacity == 0) return;
    std::memset(destination, 0, capacity);
    std::memcpy(destination, source.data(), std::min(capacity - 1, source.size()));
}
} // namespace

void InputWorkspace::SyncRecording(UiContext& context) {
    if (!context.inputModel) return;
    Copy(stepsJson_.data(), stepsJson_.size(), context.inputModel->RecordingJson());
}

void InputWorkspace::Draw(UiContext& context) {
    const auto session = context.sessions ? context.sessions->Active() : nullptr;
    if (!session || !context.inputModel) {
        ImGui::TextDisabled("Select a process to record or replay input.");
        return;
    }
    if (targetId_ != session->Target().id) {
        targetId_ = session->Target().id;
        context.inputModel->Reset();
        stepsJson_.fill(0);
        Copy(stepsJson_.data(), stepsJson_.size(),
             "[\n  {\"action\":\"delay\",\"ms\":100}\n]");
        lastPoll_ = {};
    }

    ImGui::TextUnformatted("Input recording & replay");
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
                context.status = "Runtime connected";
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!context.mutationAllowed);
        if (ImGui::SmallButton("Enable runtime")) {
            std::string error;
            if (!context.payload->EnsureReady(&error))
                context.status = "Runtime enable failed: " + error;
            else
                context.status = "Runtime enabled";
        }
        ImGui::EndDisabled();
    }

    ImGui::Separator();

    ImGui::BeginDisabled(!context.mutationAllowed);
    if (!context.inputModel->Recording()) {
        if (ImGui::Button("Start recording")) {
            std::string error;
            if (!context.inputModel->StartRecording(context.mutationAllowed, &error))
                context.status = "Start recording failed: " + error;
            else
                context.status = "Input recording started";
        }
    } else {
        if (ImGui::Button("Stop recording")) {
            std::string error;
            if (!context.inputModel->StopRecording(context.mutationAllowed, &error))
                context.status = "Stop recording failed: " + error;
            else {
                SyncRecording(context);
                context.status = "Input recording stopped";
            }
        }
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    ImGui::Combo("Mode", &modeIndex_, kModes, IM_ARRAYSIZE(kModes));
    ImGui::SameLine();

    ImGui::BeginDisabled(!context.mutationAllowed || stepsJson_[0] == '\0');
    if (ImGui::Button("Run sequence")) {
        std::string error;
        if (!context.inputModel->StartSequence(
                stepsJson_.data(), kModes[modeIndex_],
                context.mutationAllowed, &error))
            context.status = "Input sequence failed: " + error;
        else
            context.status = "Input sequence started";
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!context.mutationAllowed ||
                         context.inputModel->RecordingJson().empty());
    if (ImGui::Button("Replay recording")) {
        std::string error;
        if (!context.inputModel->ReplayRecorded(
                kModes[modeIndex_], context.mutationAllowed, &error))
            context.status = "Replay failed: " + error;
        else
            context.status = "Recorded input replay started";
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(context.inputModel->JobId() < 1);
    if (ImGui::Button("Refresh job")) {
        std::string error;
        if (!context.inputModel->RefreshSequence(&error))
            context.status = "Input job refresh failed: " + error;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!context.mutationAllowed || !context.inputModel->JobRunning());
    if (ImGui::Button("Cancel job")) {
        std::string error;
        if (!context.inputModel->CancelSequence(context.mutationAllowed, &error))
            context.status = "Cancel input job failed: " + error;
        else
            context.status = "Input job cancellation requested";
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    if (context.inputModel->JobRunning()) {
        const auto now = std::chrono::steady_clock::now();
        if (lastPoll_.time_since_epoch().count() == 0 ||
            now - lastPoll_ >= std::chrono::milliseconds(500)) {
            context.inputModel->RefreshSequence(nullptr);
            lastPoll_ = now;
        }
    }

    ImGui::Text("Recording: %s | Job: %d | Status: %s | Step: %d / %d",
                context.inputModel->Recording() ? "active" : "idle",
                context.inputModel->JobId(),
                context.inputModel->JobStatus().empty()
                    ? "-" : context.inputModel->JobStatus().c_str(),
                context.inputModel->StepIndex(),
                context.inputModel->StepCount());

    ImGui::TextDisabled("Sequence JSON");
    ImGui::InputTextMultiline("##InputSequenceJson", stepsJson_.data(),
                              stepsJson_.size(), ImGui::GetContentRegionAvail());
}

} // namespace cortex::ui
