#include "runtime_workspace.h"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>

namespace cortex::ui {
namespace {

using json = nlohmann::json;

void CopyToBuffer(std::array<char, 8192>& buffer, const std::string& text) {
    buffer.fill(0);
    const size_t count = std::min(text.size(), buffer.size() - 1);
    std::memcpy(buffer.data(), text.data(), count);
}

} // namespace

void RuntimeWorkspace::ApplyPreset(UiContext& context) {
    if (context.runtimeToolPreset.empty()) return;
    for (size_t i = 0; i < tools_.size(); ++i) {
        if (tools_[i].name == context.runtimeToolPreset) {
            selected_ = static_cast<int>(i);
            break;
        }
    }
    if (!context.runtimeArgumentsPreset.empty()) {
        CopyToBuffer(arguments_, context.runtimeArgumentsPreset);
        initializedArgs_ = true;
    }
    context.runtimeToolPreset.clear();
    context.runtimeArgumentsPreset.clear();
}

void RuntimeWorkspace::RefreshTools(UiContext& context) {
    if (!context.payload) return;
    json response;
    bool hasResponse = false;
    std::string error;
    const json request = {
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "tools/list"},
        {"params", json::object()}
    };
    if (!context.payload->ForwardMcp(request, "all", response, hasResponse, &error) || !hasResponse) {
        context.status = "Runtime tools unavailable: " + error;
        return;
    }

    tools_.clear();
    try {
        const auto& list = response.at("result").at("tools");
        for (const auto& item : list) {
            Tool tool;
            tool.name = item.value("name", std::string());
            tool.description = item.value("description", std::string());
            if (!tool.name.empty()) tools_.push_back(std::move(tool));
        }
    } catch (...) {
        context.status = "Runtime returned an invalid tool catalog";
        return;
    }
    std::sort(tools_.begin(), tools_.end(),
              [](const Tool& a, const Tool& b) { return a.name < b.name; });
    if (selected_ >= static_cast<int>(tools_.size())) selected_ = -1;
    context.status = std::to_string(tools_.size()) + " runtime tool(s)";
    ApplyPreset(context);
}

void RuntimeWorkspace::CallSelected(UiContext& context) {
    if (!context.payload || selected_ < 0 || selected_ >= static_cast<int>(tools_.size())) return;
    json arguments;
    try {
        arguments = json::parse(arguments_.data());
        if (!arguments.is_object()) {
            context.status = "Tool arguments must be a JSON object";
            return;
        }
    } catch (...) {
        context.status = "Tool arguments contain invalid JSON";
        return;
    }

    json result;
    std::string error;
    if (!context.payload->CallTool(tools_[static_cast<size_t>(selected_)].name,
                                   arguments, result, &error)) {
        output_ = result.is_null() ? std::string() : result.dump(2);
        context.status = "Tool failed: " + error;
        return;
    }
    output_ = result.dump(2);
    context.status = "Tool completed: " + tools_[static_cast<size_t>(selected_)].name;
}

void RuntimeWorkspace::Draw(UiContext& context) {
    const auto session = context.sessions ? context.sessions->Active() : nullptr;
    if (!session || !context.payload) {
        ImGui::TextDisabled("Select a process to use advanced runtime tools.");
        return;
    }

    if (targetId_ != session->Target().id) {
        targetId_ = session->Target().id;
        context.payload->Reset();
        tools_.clear();
        selected_ = -1;
        output_.clear();
        initializedArgs_ = false;
    }

    if (!initializedArgs_) {
        CopyToBuffer(arguments_, "{}");
        initializedArgs_ = true;
    }

    ImGui::TextUnformatted("Advanced runtime");
    ImGui::SameLine();
    ImGui::TextDisabled("Full Cortex MCP tool catalog without Qt");
    ImGui::Spacing();

    if (!context.payload->Ready()) {
        if (ImGui::Button("Connect existing runtime")) {
            std::string error;
            if (context.payload->TryConnectExisting(&error)) {
                context.status = "Connected to existing Cortex runtime";
                RefreshTools(context);
            } else {
                context.status = "No existing runtime: " + error;
            }
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!context.mutationAllowed);
        if (ImGui::Button("Enable runtime")) {
            std::string error;
            if (context.payload->EnsureReady(&error)) {
                context.status = "Cortex runtime enabled";
                RefreshTools(context);
            } else {
                context.status = "Runtime enable failed: " + error;
            }
        }
        ImGui::EndDisabled();
        if (!context.mutationAllowed) {
            ImGui::SameLine();
            ImGui::TextDisabled("Enable writes to inject runtime");
        }
    } else {
        ImGui::TextDisabled("Runtime connected");
        ImGui::SameLine();
        if (ImGui::SmallButton("Refresh tools")) RefreshTools(context);
    }

    if (context.payload->Ready() && tools_.empty()) RefreshTools(context);
    ApplyPreset(context);

    if (tools_.empty()) {
        ImGui::Dummy(ImVec2(0, 15));
        ImGui::TextWrapped("The native Memory / Disassembler / Modules / Debugger pages work without injection. "
                           "Enable the runtime only for advanced hooks, traces, RE, capture, scripting and MCP tools.");
        return;
    }

    ImGui::Spacing();
    const float left = std::clamp(ImGui::GetContentRegionAvail().x * 0.34f, 280.0f, 440.0f);

    ImGui::BeginChild("ToolCatalog", ImVec2(left, 0), ImGuiChildFlags_Borders);
    ImGui::Text("Tools (%zu)", tools_.size());
    ImGui::Separator();
    for (size_t i = 0; i < tools_.size(); ++i) {
        const bool selected = selected_ == static_cast<int>(i);
        if (ImGui::Selectable(tools_[i].name.c_str(), selected)) {
            selected_ = static_cast<int>(i);
            CopyToBuffer(arguments_, "{}");
        }
        if (ImGui::IsItemHovered() && !tools_[i].description.empty()) {
            ImGui::SetTooltip("%s", tools_[i].description.c_str());
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("ToolCall", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (selected_ < 0 || selected_ >= static_cast<int>(tools_.size())) {
        ImGui::TextDisabled("Select a runtime tool.");
        ImGui::EndChild();
        return;
    }

    const Tool& tool = tools_[static_cast<size_t>(selected_)];
    ImGui::TextUnformatted(tool.name.c_str());
    if (!tool.description.empty()) ImGui::TextWrapped("%s", tool.description.c_str());
    ImGui::Separator();

    ImGui::TextDisabled("Arguments (JSON)");
    ImGui::InputTextMultiline("##ToolArguments", arguments_.data(), arguments_.size(),
                              ImVec2(-1, 150));
    ImGui::BeginDisabled(!context.mutationAllowed &&
                         arguments_.data() &&
                         std::strstr(arguments_.data(), "mutation_permission") != nullptr);
    if (ImGui::Button("Run tool", ImVec2(140, 36))) CallSelected(context);
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::TextDisabled("Result");
    ImGui::BeginChild("ToolOutput", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::TextUnformatted(output_.empty() ? "No result yet." : output_.c_str());
    ImGui::EndChild();
    ImGui::EndChild();
}

} // namespace cortex::ui
