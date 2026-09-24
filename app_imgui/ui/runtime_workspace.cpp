#include "runtime_workspace.h"

#include "api/mcp_contract.h"
#include "api/semantic_tools.h"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <utility>

namespace cortex::ui {
namespace {

using json = nlohmann::json;

void CopyToBuffer(std::array<char, 8192>& buffer, const std::string& text) {
    buffer.fill(0);
    const size_t count = std::min(text.size(), buffer.size() - 1);
    std::memcpy(buffer.data(), text.data(), count);
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

json RouteResult(const json& output) {
    if (!output.is_object()) return output;
    const auto found = output.find("result");
    return found != output.end() ? *found : output;
}

} // namespace

bool RuntimeWorkspace::Visible(const Tool& tool) const {
    if (modeIndex_ == 0 && tool.semantic) return false;
    if (modeIndex_ == 2 && !tool.semantic) return false;
    if (filter_[0] == '\0') return true;
    const std::string needle = Lower(filter_.data());
    return Lower(tool.name + " " + tool.description).find(needle) != std::string::npos;
}

void RuntimeWorkspace::ApplyPreset(UiContext& context) {
    if (context.requestSemanticRuntime) {
        modeIndex_ = 2;
        context.requestSemanticRuntime = false;
    }
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

    json output;
    std::string error;
    if (!context.payload->CallTool("tools", json::object(), output, &error)) {
        context.status = "Runtime tools unavailable: " + error;
        return;
    }

    const json manifest = RouteResult(output);
    if (!manifest.is_array()) {
        context.status = "Runtime returned an invalid tool catalog";
        return;
    }

    tools_.clear();
    for (const auto& entry : manifest) {
        if (!entry.is_object()) continue;
        Tool tool;
        tool.name = entry.value("name", std::string());
        if (tool.name.empty() || tool.name == "mcp") continue;
        tool.description = entry.value("description", std::string());
        tool.method = entry.value("method", std::string("GET"));
        tool.path = entry.value("path", std::string());
        const auto risk = api::mcp_contract::ClassifyTool(tool.name, tool.method, tool.path);
        tool.risk = api::mcp_contract::RiskName(risk);
        tool.mutationRequired = api::mcp_contract::RequiresMutationPermission(risk);

        json hints = json::object();
        if (entry.contains("body")) hints["body"] = entry["body"];
        if (entry.contains("query")) hints["_query"] = entry["query"];
        if (tool.path.find('{') != std::string::npos)
            hints["_path"] = "required path substitutions";
        if (!hints.empty()) tool.hint = hints.dump(2);
        tools_.push_back(std::move(tool));
    }

    for (const auto& entry : api::semantic::Catalog()) {
        if (!entry.is_object()) continue;
        Tool tool;
        tool.name = entry.value("name", std::string());
        if (tool.name.empty()) continue;
        tool.description = entry.value("description", std::string());
        tool.method = "MCP";
        tool.risk = "semantic";
        tool.semantic = true;
        tool.argumentTemplate = "{\n  \"objective\": \"\"\n}";
        if (entry.contains("inputSchema")) tool.hint = entry["inputSchema"].dump(2);
        tools_.push_back(std::move(tool));
    }

    std::sort(tools_.begin(), tools_.end(),
              [](const Tool& a, const Tool& b) { return a.name < b.name; });
    if (selected_ >= static_cast<int>(tools_.size())) selected_ = -1;

    const size_t semanticCount =
        static_cast<size_t>(std::count_if(tools_.begin(), tools_.end(),
                                          [](const Tool& tool) { return tool.semantic; }));
    context.status = std::to_string(tools_.size() - semanticCount) +
                     " primitive(s), " + std::to_string(semanticCount) +
                     " semantic tool(s)";
    ApplyPreset(context);
}

void RuntimeWorkspace::CallSelected(UiContext& context) {
    if (!context.payload || selected_ < 0 ||
        selected_ >= static_cast<int>(tools_.size())) return;

    const Tool& tool = tools_[static_cast<size_t>(selected_)];
    json arguments;
    try {
        arguments = json::parse(arguments_.data());
        if (!arguments.is_object()) {
            context.status = "Tool arguments must be a JSON object";
            return;
        }
    } catch (const std::exception& exception) {
        context.status = std::string("Invalid JSON: ") + exception.what();
        return;
    }

    if (tool.mutationRequired && !context.mutationAllowed) {
        context.status = "Enable writes before calling this runtime tool";
        return;
    }
    if (tool.semantic && arguments.value("mutation_permission", false) &&
        !context.mutationAllowed) {
        context.status = "Enable writes before semantic mutation execution";
        return;
    }
    if (!tool.semantic && tool.mutationRequired)
        arguments["mutation_permission"] = true;

    json result;
    std::string error;
    const bool ok = context.payload->CallTool(tool.name, arguments, result, &error);
    output_ = result.is_null() ? std::string() : result.dump(2);
    if (!ok) {
        context.status = "Tool failed: " + error;
        return;
    }
    context.status = "Tool completed: " + tool.name;
}

void RuntimeWorkspace::Draw(UiContext& context) {
    const auto session = context.sessions ? context.sessions->Active() : nullptr;
    if (!session || !context.payload) {
        ImGui::TextDisabled("Select a process to use advanced runtime tools.");
        return;
    }

    if (targetId_ != session->Target().id) {
        targetId_ = session->Target().id;
        tools_.clear();
        selected_ = -1;
        output_.clear();
        initializedArgs_ = false;
        filter_.fill(0);
        modeIndex_ =
            context.settings && context.settings->Values().mcpToolProfile == "all"
                ? 1 : 0;
    }

    if (!initializedArgs_) {
        CopyToBuffer(arguments_, "{}");
        initializedArgs_ = true;
    }

    ImGui::TextUnformatted("Advanced runtime");
    ImGui::SameLine();
    ImGui::TextDisabled("Primitive + semantic Cortex tool catalog");
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
        if (ImGui::SmallButton("Refresh catalog")) RefreshTools(context);
    }

    if (context.payload->Ready() && tools_.empty()) RefreshTools(context);

    if (modeIndex_ != 2 && context.settings) {
        const int configuredMode =
            context.settings->Values().mcpToolProfile == "all" ? 1 : 0;
        if (modeIndex_ != configuredMode) modeIndex_ = configuredMode;
    }
    ApplyPreset(context);

    if (tools_.empty()) {
        ImGui::Dummy(ImVec2(0, 15));
        ImGui::TextWrapped("Enable or connect the runtime to load advanced Cortex tools.");
        return;
    }

    static const char* modes[] = {"Primitives", "All tools", "Semantic"};
    ImGui::SetNextItemWidth(130);
    if (ImGui::Combo("##RuntimeMode", &modeIndex_, modes, IM_ARRAYSIZE(modes)) &&
        modeIndex_ <= 1 && context.settings) {
        context.settings->Values().mcpToolProfile =
            modeIndex_ == 1 ? "all" : "compact";
        std::string error;
        if (!context.settings->SaveAndSync(&error))
            context.status = "MCP profile save failed: " + error;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(260);
    ImGui::InputTextWithHint("##RuntimeFilter", "Filter tools...",
                             filter_.data(), filter_.size());

    const size_t primitiveCount =
        static_cast<size_t>(std::count_if(tools_.begin(), tools_.end(),
                                          [](const Tool& tool) { return !tool.semantic; }));
    ImGui::SameLine();
    ImGui::TextDisabled("%zu primitives | %zu semantic",
                        primitiveCount, tools_.size() - primitiveCount);

    ImGui::Spacing();
    const float left =
        std::clamp(ImGui::GetContentRegionAvail().x * 0.34f, 300.0f, 460.0f);

    ImGui::BeginChild("ToolCatalog", ImVec2(left, 0), ImGuiChildFlags_Borders);
    for (size_t i = 0; i < tools_.size(); ++i) {
        const Tool& tool = tools_[i];
        if (!Visible(tool)) continue;
        ImGui::PushID(static_cast<int>(i));
        const bool selected = selected_ == static_cast<int>(i);
        if (ImGui::Selectable(tool.name.c_str(), selected)) {
            selected_ = static_cast<int>(i);
            CopyToBuffer(arguments_, tool.argumentTemplate);
            output_.clear();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", tool.semantic ? "semantic" : tool.risk.c_str());
        if (ImGui::IsItemHovered() && !tool.description.empty())
            ImGui::SetTooltip("%s", tool.description.c_str());
        ImGui::PopID();
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
    ImGui::SameLine();
    if (tool.semantic)
        ImGui::TextDisabled("semantic | server-side plan/execution");
    else
        ImGui::TextDisabled("%s %s | risk: %s",
                            tool.method.c_str(), tool.path.c_str(), tool.risk.c_str());
    if (!tool.description.empty()) ImGui::TextWrapped("%s", tool.description.c_str());

    ImGui::Separator();
    ImGui::TextDisabled("Arguments (JSON)");
    ImGui::InputTextMultiline("##ToolArguments", arguments_.data(), arguments_.size(),
                              ImVec2(-1, 160));

    const bool blocked = tool.mutationRequired && !context.mutationAllowed;
    ImGui::BeginDisabled(blocked);
    if (ImGui::Button("Call", ImVec2(140, 36))) CallSelected(context);
    ImGui::EndDisabled();
    if (blocked) {
        ImGui::SameLine();
        ImGui::TextDisabled("Enable writes to call this tool");
    }

    if (!tool.hint.empty() && ImGui::CollapsingHeader("Schema / hint"))
        ImGui::TextWrapped("%s", tool.hint.c_str());

    ImGui::Spacing();
    ImGui::TextDisabled("Result");
    ImGui::BeginChild("ToolOutput", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::TextUnformatted(output_.empty() ? "No result yet." : output_.c_str());
    ImGui::EndChild();
    ImGui::EndChild();
}

} // namespace cortex::ui
