#include "overview_workspace.h"
#include "file_dialog.h"
#include "target_hint.h"
#include "widgets.h"
#include "workspace_registry.h"

#include <imgui.h>

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <sstream>

namespace cortex::ui {
namespace {

std::string CapabilitySummary(const target::TargetDescriptor& target) {
    std::ostringstream out;
    const auto names = target.capabilities.Names();
    for (size_t i = 0; i < names.size(); ++i) {
        if (i) out << ", ";
        out << names[i];
    }
    return out.str();
}

// The MCP client configuration for this cortex.exe, read-only (no
// --allow-writes): the operator adds that argument themselves.
std::string McpConfiguration() {
    std::wstring module(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    std::string path = "C:/Cortex/cortex.exe";
    if (length && length < module.size()) {
        module.resize(length);
        path = std::filesystem::path(module).generic_u8string();
    }
    std::string escaped;
    for (const char ch : path) {
        if (ch == '"' || ch == '\\') escaped += '\\';
        escaped += ch;
    }
    return "{ \"mcpServers\": { \"cortex\": { \"command\": \"" + escaped + "\", \"args\": [\"mcp\"] } } }";
}

// A bordered card of the start page with a title, a description and its
// own buttons below.
// A height of 0 fits the card to its content (stacked cards).
void Card(const char* id, const char* title, const char* body, float width, float height,
          const std::function<void()>& actions) {
    ImGui::BeginChild(id, ImVec2(width, height),
                      ImGuiChildFlags_Borders | (height > 0 ? 0 : ImGuiChildFlags_AutoResizeY));
    ImGui::Dummy(ImVec2(0, Px(4)));
    {
        HeadingFont heading;
        ImGui::TextUnformatted(title);
    }
    ImGui::Spacing();
    HintText(body);
    ImGui::Dummy(ImVec2(0, Px(8)));
    actions();
    ImGui::EndChild();
}

std::string FileName(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // namespace

void OverviewWorkspace::HandleCommand(UiContext& context, const std::string& command) {
    static const std::string open = "home_open ";
    if (command.rfind(open, 0) == 0) {
        context.requestPreset = static_cast<int>(WorkspacePreset::Files);
        context.commands.push_back("inspect_artifact " + command.substr(open.size()));
        return;
    }
    if (command == "home_ai") context.requestPreset = static_cast<int>(WorkspacePreset::Ai);
}

void OverviewWorkspace::Draw(UiContext& context) {
    const auto session = context.sessions ? context.sessions->Active() : nullptr;
    if (session) DrawTarget(context);
    else DrawStart(context);
}

void OverviewWorkspace::DrawStart(UiContext& context) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float available = ImGui::GetContentRegionAvail().x;
    const float maxWidth = std::min(available, Px(1180));
    const float left = std::max(0.0f, (available - maxWidth) * 0.5f);

    ImGui::Dummy(ImVec2(0, Px(16)));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + left);
    CortexMark(Px(40));
    ImGui::SameLine(0.0f, Px(12));
    ImGui::BeginGroup();
    {
        HeadingFont heading;
        ImGui::TextUnformatted("Welcome to Cortex");
    }
    ImGui::TextDisabled("Analyze software you own or are authorized to inspect.");
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(0, Px(14)));

    // Side by side when the three fit; stacked (each as tall as its content,
    // so all three stay on screen) in a narrow window.
    const bool row = maxWidth >= Px(220) * 3 + style.ItemSpacing.x * 2;
    const float width = row ? (maxWidth - style.ItemSpacing.x * 2) / 3.0f : maxWidth;
    const float height = row ? Px(250) : 0.0f;
    auto place = [&](bool first) {
        if (row && !first) ImGui::SameLine();
        else ImGui::SetCursorPosX(ImGui::GetCursorPosX() + left);
    };

    place(true);
    Card("HomeFile", "Analyze a file",
         "Executables, DLLs, .NET assemblies, Electron .asar archives, JavaScript and ZIP files. "
         "The file is read, never run.",
         width, height, [&context]() {
             if (ImGui::Button("Open a file...", ImVec2(-1, Px(34)))) {
                 std::string chosen;
                 if (ShowOpenFileDialog(L"All files\0*.*\0Executables and libraries\0*.exe;*.dll\0"
                                        L"Electron archives\0*.asar\0",
                                        chosen))
                     context.commands.push_back("home_open " + chosen);
             }
             const auto* recent = context.settings ? &context.settings->Values().recentFiles : nullptr;
             if (recent && !recent->empty()) {
                 ImGui::Spacing();
                 ImGui::TextDisabled("Recent files");
                 for (size_t i = 0; i < recent->size() && i < 5; ++i) {
                     const std::string& path = (*recent)[i];
                     const std::string label = FileName(path) + "##recent" + std::to_string(i);
                     if (ImGui::Selectable(label.c_str())) context.commands.push_back("home_open " + path);
                     if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path.c_str());
                 }
             }
         });

    place(false);
    Card("HomeProcess", "Inspect a running program",
         "Scan and change values in memory, disassemble, set breakpoints and trace a program that runs now. "
         "Read-only until you switch the header to \"Writes allowed\".",
         width, height, [&context]() {
             if (ImGui::Button("Select a process...", ImVec2(-1, Px(34)))) context.requestProcessPicker = true;
             ImGui::Spacing();
             HintText("File > Attach to foreground process picks the last window you used.");
         });

    place(false);
    Card("HomeAi", "Work with an AI agent",
         "Your MCP client (Claude, an IDE...) starts cortex.exe mcp. The agent is read-only unless you add "
         "--allow-writes to its arguments. Follow and approve its work here.",
         width, height, [this, &context]() {
             if (ImGui::Button(copied_ ? "Copied###CopyMcp" : "Copy the MCP configuration###CopyMcp",
                               ImVec2(-1, Px(34)))) {
                 ImGui::SetClipboardText(McpConfiguration().c_str());
                 copied_ = true;
             }
             if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", McpConfiguration().c_str());
             if (ImGui::Button("Open the AI workspace", ImVec2(-1, 0))) context.commands.push_back("home_ai");
         });

    ImGui::Dummy(ImVec2(0, Px(14)));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + left);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + maxWidth);
    HintText("The Workspace menu switches layouts (Files, AI, Memory, Debug...); Home brings you back here. "
             "Help > Getting started explains a first session. Cortex does not bypass anti-cheat, DRM or "
             "other protections.");
    ImGui::PopTextWrapPos();
}

void OverviewWorkspace::DrawTarget(UiContext& context) {
    const auto session = context.sessions->Active();
    const auto& target = session->Target();
    {
        HeadingFont heading;
        ImGui::TextUnformatted(target.name.c_str());
    }
    ImGui::TextDisabled("PID %llu | %s | %s",
                        static_cast<unsigned long long>(target.processId),
                        target::PlatformName(target.platform),
                        target::ArchitectureName(target.architecture));

    ImGui::Separator();
    ImGui::TextUnformatted("Target");

    if (BeginDataTable("OverviewTarget", 2,
                          ImGuiTableFlags_SizingFixedFit |
                          ImGuiTableFlags_BordersInnerH,
                          ImVec2(0, Px(220)))) {
        ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, Px(150));
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

        auto row = [](const char* key, const std::string& value) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("%s", key);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(value.c_str());
        };

        row("Platform", target::PlatformName(target.platform));
        row("Architecture", target::ArchitectureName(target.architecture));
        row("Session", session->Alive() ? "active" : "stale");
        row("Attached targets",
            context.sessions ? std::to_string(context.sessions->AttachedTargets().size()) : "0");
        row("Writes", context.mutationAllowed ? "Allowed" : "Read-only");
        row("Capabilities", CapabilitySummary(target));
        ImGui::EndTable();
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Actions");

    if (ImGui::Button("Change process")) context.requestProcessPicker = true;
    FlowSameLine(ButtonWidth("Sessions"));
    if (ImGui::Button("Sessions")) context.requestWorkspace = "sessions";
    FlowSameLine(std::max(ButtonWidth("Disable writes"), ButtonWidth("Enable writes")));
    if (ImGui::Button(context.mutationAllowed ? "Disable writes" : "Enable writes"))
        context.mutationAllowed = !context.mutationAllowed;
    FlowSameLine(ButtonWidth("Memory"));
    if (ImGui::Button("Memory")) context.requestPreset = static_cast<int>(WorkspacePreset::Memory);
    FlowSameLine(ButtonWidth("Debugger"));
    if (ImGui::Button("Debugger")) context.requestPreset = static_cast<int>(WorkspacePreset::Debug);
    FlowSameLine(ButtonWidth("Analyze a file"));
    if (ImGui::Button("Analyze a file")) context.requestPreset = static_cast<int>(WorkspacePreset::Files);

    ImGui::Spacing();
    ImGui::TextDisabled("%s", context.status.c_str());
}

} // namespace cortex::ui
